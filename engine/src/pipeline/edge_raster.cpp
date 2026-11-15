// edge_raster.cpp -- the imprint raster surface (see edge_raster.hpp).
//
// macOS keeps the upstream CoreText/CoreGraphics/vImage code, moved here
// verbatim from the overscan.cpp the Windows port started from (the CG
// context, its CTM chain, the CTLine draws and the vImage separable blur are
// byte for byte what they were). Everywhere else the same `Op`s draw through
// stb_truetype (v1.26; it parses the bundled OTF's CFF outlines) and the blur
// is a threaded separable convolution with clamp-to-edge indexing.
#include "edge_raster.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "line_pool.hpp"
#include "pipeline.hpp"

#ifdef __APPLE__
#include <Accelerate/Accelerate.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#else
#include <cstdint>
#include <fstream>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_malloc(x, u) ((void)(u), malloc(x))
#define STBTT_free(x, u)   ((void)(u), free(x))
#include <stdlib.h>
#include "stb_truetype.h"
#endif

namespace spk {
namespace edge_raster {
namespace {

// --- shared: film-mm maps and the group's bounding box -----------------------

// Canvas -> film inverted: (u, v) = A^-1 ((s, t) - (cs, ct)) + (cu, cv), in
// canvas millimetres.
void film_to_canvas_mm(const OverscanLayout& L, double s, double t, double& u, double& v) {
    const double det = L.A[0] * L.A[3] - L.A[1] * L.A[2];
    const double ds = s - L.cs, dt = t - L.ct;
    u = (L.A[3] * ds - L.A[1] * dt) / det + L.cu;
    v = (-L.A[2] * ds + L.A[0] * dt) / det + L.cv;
}

void film_to_canvas_px(const OverscanLayout& L, double s, double t, double& x, double& y) {
    double u, v;
    film_to_canvas_mm(L, s, t, u, v);
    x = u / L.px;
    y = v / L.px;
}

// Into the box's bitmap. The bitmap's rows ARE the canvas's rows in order
// (row 0 = the box's smallest canvas y = the film's smallest t), exactly as
// the Apple path's: CG's device-space y flip cancels against its bitmap
// memory order, so there is deliberately no flip of our own here -- a second
// one would hand add_mask a mirrored mask and the film edge's bands would
// swap edges.
struct FilmToBitmap {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    double x(double s, double t) const { return a * s + c * t + e; }
    double y(double s, double t) const { return b * s + d * t + f; }
};

FilmToBitmap film_to_bitmap(const OverscanLayout& L, int x0, int y0) {
    double ax, ay, bx, by, ox, oy;
    film_to_canvas_px(L, 1.0, 0.0, ax, ay);
    film_to_canvas_px(L, 0.0, 1.0, bx, by);
    film_to_canvas_px(L, 0.0, 0.0, ox, oy);
    FilmToBitmap m;
    m.a = ax - ox;              m.c = bx - ox;
    m.b = ay - oy;              m.d = by - oy;
    m.e = ox - double(x0);
    m.f = oy - double(y0);
    return m;
}

// The Gaussian kernel of the projection's softness, or an empty vector when
// the blur is off (sigma below a third of a pixel).
std::vector<float> blur_kernel(const OverscanLayout& L, const Group& g) {
    const double sigma = g.blur_mm / L.px;
    if (sigma <= 0.3) return {};
    const int r = int(std::ceil(3.0 * sigma));
    std::vector<float> k(size_t(2 * r + 1));
    double sum = 0.0;
    for (int i = -r; i <= r; ++i) {
        k[size_t(i + r)] = float(std::exp(-0.5 * i * i / (sigma * sigma)));
        sum += k[size_t(i + r)];
    }
    for (float& v : k) v = float(v / sum);
    return k;
}

// The projection's softness (RFC-031 §3.4): a small Gaussian on the mask.
// Separable, edges extended. On macOS through vImage rather than two scalar
// loops: the edge-print bands are full-width boxes (8,189 x 512 px at the
// full tier of a 45 MP frame, sigma ~5 px), and the loops cost 95 ms each at
// -O2 and ~1.2 s each in the Debug build's -O0. vImage is optimised whatever
// the build setting; elsewhere the loops run through the line pool.
bool apply_blur(std::vector<float>& cov, int w, int h, const std::vector<float>& k) {
    if (k.empty()) return true;
    const int r = int(k.size() / 2);
#ifdef __APPLE__
    std::vector<float> tmp(cov.size(), 0.0f);
    vImage_Buffer src{cov.data(), vImagePixelCount(h), vImagePixelCount(w), size_t(w) * sizeof(float)};
    vImage_Buffer dst{tmp.data(), vImagePixelCount(h), vImagePixelCount(w), size_t(w) * sizeof(float)};
    const uint32_t taps = uint32_t(2 * r + 1);
    if (vImageConvolve_PlanarF(&src, &dst, nullptr, 0, 0, k.data(), 1, taps, 0.0f, kvImageEdgeExtend)
            != kvImageNoError ||
        vImageConvolve_PlanarF(&dst, &src, nullptr, 0, 0, k.data(), taps, 1, 0.0f, kvImageEdgeExtend)
            != kvImageNoError)
        return false;
    return true;
#else
    // Same tap order as vImage per output pixel (kernel index -r..+r), each
    // line owned by one thread with its own private accumulator -- so the
    // result is deterministic for any worker count, and within ~1e-7 of the
    // vImage path (the two were never promised bit-identical: summation order
    // in float32 differs inside the SIMD lanes).
    std::vector<float> tmp(cov.size(), 0.0f);
    // horizontal: cov -> tmp
    for_each_line(uint32_t(h), [&](uint32_t y) {
        const float* row = cov.data() + size_t(y) * size_t(w);
        float* out = tmp.data() + size_t(y) * size_t(w);
        for (int x = 0; x < w; ++x) {
            float acc = 0.0f;
            for (int i = -r; i <= r; ++i) {
                const int xx = std::clamp(x + i, 0, w - 1);      // kvImageEdgeExtend
                acc += k[size_t(i + r)] * row[xx];
            }
            out[x] = acc;
        }
    });
    // vertical: tmp -> cov
    for_each_line(uint32_t(w), [&](uint32_t x) {
        for (int y = 0; y < h; ++y) {
            float acc = 0.0f;
            for (int i = -r; i <= r; ++i) {
                const int yy = std::clamp(y + i, 0, h - 1);      // kvImageEdgeExtend
                acc += k[size_t(i + r)] * tmp[size_t(yy) * size_t(w) + size_t(x)];
            }
            cov[size_t(y) * size_t(w) + size_t(x)] = acc;
        }
    });
    return true;
#endif
}

// The group's bounding box in canvas px from each op's film-space extent
// (generous for text), expanded by the blur's pad, clipped to the canvas.
void group_box(const OverscanLayout& L, const Group& g, int& x0, int& y0, int& w, int& h) {
    double xmin = 1e30, ymin = 1e30, xmax = -1e30, ymax = -1e30;
    auto grow = [&](double s, double t) {
        double x, y;
        film_to_canvas_px(L, s, t, x, y);
        xmin = std::min(xmin, x); ymin = std::min(ymin, y); xmax = std::max(xmax, x); ymax = std::max(ymax, y);
    };
    for (const Op& op : g.ops) {
        if (op.kind == Op::Poly) {
            for (size_t k = 0; k + 1 < op.pts.size(); k += 2) grow(op.pts[k], op.pts[k + 1]);
        } else if (op.kind == Op::Circle) {
            grow(op.pts[0] - op.size_mm, op.pts[1] - op.size_mm);
            grow(op.pts[0] + op.size_mm, op.pts[1] + op.size_mm);
        } else {
            const double len = (0.75 * op.size_mm * op.hscale + op.tracking_mm * (op.scale_advances ? op.hscale : 1.0)) *
                               double(op.text.size()) + op.size_mm;
            const double r = op.rot_deg * M_PI / 180.0;
            const double ex = std::cos(r), ey = std::sin(r);
            for (double a : {-0.3 * op.size_mm, len})
                for (double b : {-1.1 * op.size_mm, 0.4 * op.size_mm})
                    grow(op.s + a * ex - b * ey, op.t + a * ey + b * ex);
        }
    }
    const double pad = 3.0 + 3.0 * g.blur_mm / L.px;
    x0 = std::max(0, int(std::floor(xmin - pad)));
    y0 = std::max(0, int(std::floor(ymin - pad)));
    const int x1 = std::min(int(L.canvas_w), int(std::ceil(xmax + pad)));
    const int y1 = std::min(int(L.canvas_h), int(std::ceil(ymax + pad)));
    w = x1 - x0; h = y1 - y0;
}

}  // namespace

// --- macOS: the upstream CoreText/CoreGraphics surface, verbatim -------------
#ifdef __APPLE__

namespace {

CFAttributedStringRef make_text(const Op& op, CTFontRef font, double gain) {
    CFStringRef str = CFStringCreateWithCString(nullptr, op.text.c_str(), kCFStringEncodingUTF8);
    CGFloat white[] = {CGFloat(std::clamp(gain, 0.0, 1.0)), 1.0};
    CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
    CGColorRef col = CGColorCreate(gray, white);
    CFNumberRef kern = CFNumberCreate(nullptr, kCFNumberCGFloatType, &op.tracking_mm);
    const void* keys[] = {kCTFontAttributeName, kCTForegroundColorAttributeName, kCTKernAttributeName};
    const void* vals[] = {font, col, kern};
    CFDictionaryRef attrs = CFDictionaryCreate(nullptr, keys, vals, 3, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef as = CFAttributedStringCreate(nullptr, str, attrs);
    CFRelease(attrs); CFRelease(kern); CFRelease(col); CFRelease(gray); CFRelease(str);
    return as;
}

}  // namespace

double text_width(const char* font, double size_mm, const std::string& text, double tracking_mm,
                  double hscale) {
    Op op;
    op.font = font; op.size_mm = size_mm; op.text = text; op.tracking_mm = tracking_mm;
    CFStringRef fname = CFStringCreateWithCString(nullptr, font, kCFStringEncodingUTF8);
    CTFontRef f = CTFontCreateWithName(fname, size_mm, nullptr);
    CFRelease(fname);
    CFAttributedStringRef as = make_text(op, f, 1.0);
    CTLineRef line = CTLineCreateWithAttributedString(as);
    const double w = CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr);
    CFRelease(line); CFRelease(as); CFRelease(f);
    return w * hscale;
}

// Nothing to load: the Apple path reads the system's own faces.
void set_font_directory(const std::string& dir) {
    (void)dir;
}

bool rasterise(const OverscanLayout& L, const Group& g, std::vector<float>& cov, int& x0, int& y0,
               int& w, int& h) {
    group_box(L, g, x0, y0, w, h);
    if (w <= 0 || h <= 0) return false;

    std::vector<uint8_t> px(size_t(w) * size_t(h), 0);
    CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
    CGContextRef ctx = CGBitmapContextCreate(px.data(), size_t(w), size_t(h), 8, size_t(w), gray,
                                             kCGImageAlphaNone);
    CGColorSpaceRelease(gray);
    if (!ctx) return false;
    CGContextSetGrayFillColor(ctx, 1.0, 1.0);
    CGContextSetShouldAntialias(ctx, true);
    CGContextSetShouldSmoothFonts(ctx, false);
    // User space = film millimetres, t downward: flip to y-down bitmap pixels,
    // shift to this box, scale to mm, then canvas <- film.
    CGContextTranslateCTM(ctx, 0, h);
    CGContextScaleCTM(ctx, 1, -1);
    CGContextTranslateCTM(ctx, -x0, -y0);
    CGContextScaleCTM(ctx, 1.0 / L.px, 1.0 / L.px);
    CGContextTranslateCTM(ctx, L.cu, L.cv);
    {
        const double a = L.A[0], b = L.A[1], c = L.A[2], d = L.A[3], det = a * d - b * c;
        // (u, v) = Ainv (s, t): CG's (a, b, c, d) maps x' = a x + c y, y' = b x + d y.
        CGContextConcatCTM(ctx, CGAffineTransformMake(d / det, -c / det, -b / det, a / det, 0, 0));
    }
    CGContextTranslateCTM(ctx, -L.cs, -L.ct);

    for (const Op& op : g.ops) {
        CGContextSetGrayFillColor(ctx, std::clamp(op.gain, 0.0, 1.0), 1.0);
        if (op.kind == Op::Poly) {
            CGContextBeginPath(ctx);
            CGContextMoveToPoint(ctx, op.pts[0], op.pts[1]);
            for (size_t k = 2; k + 1 < op.pts.size(); k += 2) CGContextAddLineToPoint(ctx, op.pts[k], op.pts[k + 1]);
            CGContextClosePath(ctx);
            CGContextFillPath(ctx);
            continue;
        }
        if (op.kind == Op::Circle) {
            const double r = op.size_mm;
            CGContextFillEllipseInRect(ctx, CGRectMake(op.pts[0] - r, op.pts[1] - r, 2 * r, 2 * r));
            continue;
        }
        CFStringRef fname = CFStringCreateWithCString(nullptr, op.font, kCFStringEncodingUTF8);
        CTFontRef font = CTFontCreateWithName(fname, op.size_mm, nullptr);
        CFRelease(fname);
        CFAttributedStringRef as = make_text(op, font, op.gain);
        CTLineRef line = CTLineCreateWithAttributedString(as);
        CGContextSaveGState(ctx);
        CGContextTranslateCTM(ctx, op.s, op.t);
        CGContextRotateCTM(ctx, op.rot_deg * M_PI / 180.0);
        if (op.scale_advances) CGContextScaleCTM(ctx, op.hscale, 1.0);
        CGContextSetTextMatrix(ctx, CGAffineTransformMakeScale(op.scale_advances ? 1.0 : op.hscale, -1.0));   // t is down; glyphs are up
        CGContextSetTextPosition(ctx, 0, 0);
        CTLineDraw(line, ctx);
        CGContextRestoreGState(ctx);
        CFRelease(line); CFRelease(as); CFRelease(font);
    }
    CGContextRelease(ctx);

    cov.resize(px.size());
    for (size_t i = 0; i < px.size(); ++i) cov[i] = float(px[i]) * (1.0f / 255.0f);
    return apply_blur(cov, w, h, blur_kernel(L, g));
}

#else  // !__APPLE__ -- stb_truetype draws the same Ops ------------------------

namespace {

constexpr double kPi = 3.14159265358979323846;

// The bundled faces (resources/fonts/, TeX Gyre Heros, GUST Font License;
// license/GUST-FONT-LICENSE.txt). Helvetica Neue, Arial Narrow and Menlo Bold
// are Apple system fonts and cannot be redistributed; the substitutions are
// recorded in edge_raster.hpp.
struct Face {
    const char* name;       // the Op's font name
    const char* file;       // under <font dir>/fonts/
    double width_scale;     // x compression on advances and shapes (Arial Narrow)
};
constexpr Face kFaces[] = {
    {"HelveticaNeue",       "texgyreheros-regular.otf", 1.0},
    {"HelveticaNeue-Bold",  "texgyreheros-bold.otf",    1.0},
    // No Medium exists in TeX Gyre Heros; Bold stands in (PORT_PLAN §4).
    {"HelveticaNeue-Medium","texgyreheros-bold.otf",    1.0},
    // Menlo's uniform 0.6 em advances become Heros' proportional ones; every
    // Menlo op carries scale_advances and its own hscale, so the drawn
    // lengths hold without a second face.
    {"Menlo-Bold",          "texgyreheros-bold.otf",    1.0},
    // Arial Narrow is Arial (Heros' own metrics) compressed to 0.82 width.
    {"ArialNarrow",         "texgyreheros-regular.otf", 0.82},
};

std::string g_font_dir;

// The missing-glyph fallback advance: a word space's width, as CoreText
// falls back for unmapped characters the edge print may carry.
constexpr double kSpaceEm = 0.278;

struct LoadedFace {
    std::string key;        // the face file it came from
    double width_scale = 1.0;
    std::vector<unsigned char> data;    // owns the bytes stbtt points into
    stbtt_fontinfo info{};
    float upem = 1000.0f;
};

// stbtt's fontinfo holds pointers into `data`, so the loaded faces are
// address-stable and read-only after init (concurrent glyph reads are safe);
// loads happen on the render thread that first needs the face.
const LoadedFace* face_for(const char* name) {
    static std::vector<LoadedFace> loaded;
    const Face* sub = nullptr;
    for (const Face& f : kFaces) if (std::strcmp(f.name, name) == 0) { sub = &f; break; }
    if (!sub) sub = &kFaces[1];                       // an unknown name takes the default face's weight
    for (const LoadedFace& f : loaded)
        if (f.key == sub->file && f.width_scale == sub->width_scale) return &f;
    const std::string path = g_font_dir + "/fonts/" + sub->file;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return nullptr;
    const std::streamoff length = file.tellg();
    if (length <= 0) return nullptr;
    LoadedFace f;
    f.key = sub->file;
    f.width_scale = sub->width_scale;
    f.data.resize(size_t(length) + 1, 0);             // a guard byte: stbtt reads past the last table on rare fonts
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(f.data.data()), length)) return nullptr;
    const int offset = stbtt_GetFontOffsetForIndex(f.data.data(), 0);
    if (!stbtt_InitFont(&f.info, f.data.data(), offset)) return nullptr;
    f.upem = std::max(1.0f, 1.0f / stbtt_ScaleForMappingEmToPixels(&f.info, 1.0f));   // the EM square's font units
    loaded.push_back(std::move(f));
    return &loaded.back();
}

// UTF-8 -> one codepoint; `i` advances past it. Malformed bytes read as 0
// (which maps to no glyph and takes the fallback advance).
uint32_t utf8_next(const std::string& text, size_t& i) {
    const unsigned char c = text[i];
    size_t n = 1;
    uint32_t cp = 0;
    if (c < 0x80) cp = c;
    else if ((c & 0xE0) == 0xC0 && i + 1 < text.size()) { n = 2; cp = (uint32_t(c & 0x1F) << 6) | (text[i + 1] & 0x3F); }
    else if ((c & 0xF0) == 0xE0 && i + 2 < text.size()) { n = 3; cp = (uint32_t(c & 0x0F) << 12) | (uint32_t(text[i + 1] & 0x3F) << 6) | (text[i + 2] & 0x3F); }
    else if ((c & 0xF8) == 0xF0 && i + 3 < text.size()) { n = 4; cp = (uint32_t(c & 0x07) << 18) | (uint32_t(text[i + 1] & 0x3F) << 12) | (uint32_t(text[i + 2] & 0x3F) << 6) | (text[i + 3] & 0x3F); }
    i += n;
    return cp;
}

double glyph_advance_mm(const LoadedFace* f, int glyph, double upmm, bool& drawable) {
    drawable = glyph != 0;
    int advance = 0, lsb = 0;
    if (drawable) stbtt_GetGlyphHMetrics(&f->info, glyph, &advance, &lsb);
    else advance = int(kSpaceEm * f->upem);
    return advance * upmm * f->width_scale;
}

}  // namespace

void set_font_directory(const std::string& dir) {
    // Set once per process, before the first render thread exists; the faces
    // load lazily on first use and never reload.
    g_font_dir = dir;
}

double text_width(const char* font, double size_mm, const std::string& text, double tracking_mm,
                  double hscale) {
    const LoadedFace* f = face_for(font);
    if (!f || text.empty()) return 0.0;
    const double upmm = double(size_mm) / f->upem;
    size_t i = 0;
    size_t count = 0;
    double width = 0.0;
    while (i < text.size()) {
        const uint32_t cp = utf8_next(text, i);
        const int glyph = stbtt_FindGlyphIndex(&f->info, int(cp));
        bool drawable = false;
        width += glyph_advance_mm(f, glyph, upmm, drawable);
        ++count;
    }
    if (count > 1) width += tracking_mm * double(count - 1);   // the kern attribute's tracking, between characters
    return width * hscale;
}

namespace {

// A point in absolute bitmap px.
struct D2 { double x, y; };

// A path in absolute bitmap px (y down), flattened ready for a fill. Each
// fill makes its own stbtt_Rasterize call: stbtt_vertex carries int16
// coordinates, so the shape is drawn in its own clipped box with a fixed-
// point unit chosen to fit -- a glyph's small box gets sub-pixel placement
// (the date's dots, the print's letters), a band-wide rectangle still fits
// at a coarse unit that its straight edges round to exactly.
struct Path {
    std::vector<D2> pts;
    std::vector<uint8_t> kinds;    // 0 = move, 1 = line, 2 = quadratic (cx/cy in cs/sn)
    std::vector<D2> ctrl;
    void move(double x, double y) { pts.push_back({x, y}); kinds.push_back(0); ctrl.push_back({0, 0}); }
    void line(double x, double y) { pts.push_back({x, y}); kinds.push_back(1); ctrl.push_back({0, 0}); }
    void quad(double cx, double cy, double x, double y) {
        pts.push_back({x, y}); kinds.push_back(2); ctrl.push_back({cx, cy});
    }
};

struct Surface {
    std::vector<uint8_t>& px;
    int w, h;
    // Source-over composite of a coverage tile, as CG composites the grey
    // fill (1.0, gain): the fragment's alpha is gain * coverage, the source
    // is white, so out = alpha + dst*(1 - alpha). Keeping the dst retention
    // on the same alpha is what makes a half-covered edge pixel as bright as
    // the Apple path's.
    void composite(const std::vector<uint8_t>& tile, int tx, int ty, int tw, int th, double gain) {
        const int x_lo = std::max(0, tx), y_lo = std::max(0, ty);
        const int x_hi = std::min(w, tx + tw), y_hi = std::min(h, ty + th);
        const double g = std::clamp(gain, 0.0, 1.0);
        for (int y = y_lo; y < y_hi; ++y)
            for (int x = x_lo; x < x_hi; ++x) {
                const double alpha = g * (tile[size_t(y - ty) * size_t(tw) + size_t(x - tx)] * (1.0 / 255.0));
                const double dst = px[size_t(y) * size_t(w) + size_t(x)] * (1.0 / 255.0);
                const double out = alpha + dst * (1.0 - alpha);
                px[size_t(y) * size_t(w) + size_t(x)] = uint8_t(std::lround(out * 255.0));
            }
    }
};

// Fill one path through stbtt's scanline rasteriser -- the same one its
// glyph bitmaps go through, so text, polygons and dots share one
// antialiasing model.
void fill_path(Surface& surf, const Path& path, double gain) {
    if (path.pts.size() < 2) return;
    float xmin = 1e30f, ymin = 1e30f, xmax = -1e30f, ymax = -1e30f;
    for (const D2& v : path.pts) {
        xmin = std::min(xmin, float(v.x)); xmax = std::max(xmax, float(v.x));
        ymin = std::min(ymin, float(v.y)); ymax = std::max(ymax, float(v.y));
    }
    const int tx = std::max(0, int(std::floor(xmin)) - 1), ty = std::max(0, int(std::floor(ymin)) - 1);
    const int x1 = std::min(surf.w, int(std::ceil(xmax)) + 2), y1 = std::min(surf.h, int(std::ceil(ymax)) + 2);
    if (x1 <= tx || y1 <= ty) return;
    const int tw = x1 - tx, th = y1 - ty;
    // The fixed-point unit: as fine as the box's span allows inside int16,
    // capped at 1/64 px (finer is beyond the coverage resolution).
    const double units = std::clamp(std::floor(32000.0 / double(std::max(tw, th) + 4)), 1.0, 64.0);
    std::vector<stbtt_vertex> verts(path.pts.size());
    for (size_t i = 0; i < path.pts.size(); ++i) {
        stbtt_vertex& v = verts[i];
        const double lx = (path.pts[i].x - double(tx)) * units;
        const double ly = (path.pts[i].y - double(ty)) * units;
        v.x = int16_t(std::lround(lx));
        v.y = int16_t(std::lround(ly));
        if (path.kinds[i] == 2) {
            v.type = STBTT_vcurve;
            v.cx = int16_t(std::lround((path.ctrl[i].x - double(tx)) * units));
            v.cy = int16_t(std::lround((path.ctrl[i].y - double(ty)) * units));
        } else {
            v.type = path.kinds[i] == 0 ? STBTT_vmove : STBTT_vline;
        }
    }
    std::vector<uint8_t> tile(size_t(tw) * size_t(th), 0);
    stbtt__bitmap result{tw, th, tw, tile.data()};
    // Flatness a fifth of a pixel in output units; stbtt converts it into its
    // own coordinate space through the scale.
    stbtt_Rasterize(&result, 0.2f, verts.data(), int(verts.size()),
                    float(1.0 / units), float(1.0 / units), 0.0f, 0.0f, 0, 0, 0, nullptr);
    surf.composite(tile, tx, ty, tw, th, gain);
}

// Ellipse as a polygon: 64 segments, a chord error far under the coverage
// resolution at the dots' radii (<= 0.1 mm at the print's tiers).
void ellipse_path(const FilmToBitmap& m, double cs, double ct, double r, Path& path) {
    constexpr int kSeg = 64;
    for (int k = 0; k <= kSeg; ++k) {
        const double ang = 2.0 * kPi * double(k) / double(kSeg);
        const double dx = r * std::cos(ang), dy = r * std::sin(ang);
        const double x = m.x(cs + dx, ct + dy);
        const double y = m.y(cs + dx, ct + dy);
        if (k == 0) path.move(x, y); else path.line(x, y);
    }
}

// One text op: the line laid out in text space (x along the baseline, y up
// from it), each glyph's outline transformed into the path. The pen carries
// natural advances scaled by the face's width compression; the tracking
// stands between the characters; `hscale` widens the shapes only unless
// `scale_advances` scales the whole line (advances and tracking with it);
// the rotation is about (s, t), and t runs down while glyphs run up. Each
// glyph fills on its own (its small box keeps the fixed-point unit fine).
void draw_text(Surface& surf, const FilmToBitmap& m, const Op& op) {
    const LoadedFace* f = face_for(op.font);
    if (!f || op.text.empty()) return;
    const double upmm = op.size_mm / f->upem;
    const double pre = op.scale_advances ? op.hscale : 1.0;
    const double shape_x = op.scale_advances ? 1.0 : op.hscale;
    const double rot = op.rot_deg * kPi / 180.0;
    const double cos_r = std::cos(rot), sin_r = std::sin(rot);
    // text space -> film mm: the line's own scale along the baseline only
    // (CGContextScaleCTM(hscale, 1) never touches y), then the rotation about
    // (s, t), then the placement; y flips (t is down, glyphs are up).
    auto place = [&](double lx, double ly, double& fs, double& ft) {
        const double ax = lx * pre, ay = -ly;
        fs = op.s + ax * cos_r - ay * sin_r;
        ft = op.t + ax * sin_r + ay * cos_r;
    };
    double pen = 0.0;
    for (size_t i = 0; i < op.text.size();) {
        const uint32_t cp = utf8_next(op.text, i);
        const int glyph = stbtt_FindGlyphIndex(&f->info, int(cp));
        if (glyph) {
            stbtt_vertex* outline = nullptr;
            const int n = stbtt_GetGlyphShape(&f->info, glyph, &outline);
            Path path;
            bool opened = false;
            for (int k = 0; k < n; ++k) {
                const stbtt_vertex& v = outline[k];
                // glyph units -> text space (mm, y up): the width compression
                // squeezes x (never y -- a narrow face keeps its cap height);
                // the shape hscale widens x when the line's own advances are
                // not scaled.
                const double lx = pen + v.x * upmm * f->width_scale * shape_x;
                const double ly = v.y * upmm * f->width_scale;
                double fs, ft;
                place(lx, ly, fs, ft);
                const double bx = m.x(fs, ft), by = m.y(fs, ft);
                if (v.type == STBTT_vmove) {
                    path.move(bx, by);
                    opened = true;
                } else if (!opened) {
                    path.move(bx, by);   // a shape missing its leading move
                    opened = true;
                } else if (v.type == STBTT_vline) {
                    path.line(bx, by);
                } else if (v.type == STBTT_vcurve) {
                    const double cxs = pen + v.cx * upmm * f->width_scale * shape_x;
                    const double cys = v.cy * upmm * f->width_scale;
                    double cs, ct;
                    place(cxs, cys, cs, ct);
                    path.quad(m.x(cs, ct), m.y(cs, ct), bx, by);
                } else {   // STBTT_vcubic: the bundled faces carry quadratics only
                    path.line(bx, by);
                }
            }
            stbtt_FreeShape(&f->info, outline);
            fill_path(surf, path, op.gain);
        }
        bool drawable = false;
        pen += glyph_advance_mm(f, glyph, upmm, drawable);
        if (i < op.text.size()) pen += op.tracking_mm;   // the kern attribute's tracking
    }
}

}  // namespace

bool rasterise(const OverscanLayout& L, const Group& g, std::vector<float>& cov, int& x0, int& y0,
               int& w, int& h) {
    group_box(L, g, x0, y0, w, h);
    if (w <= 0 || h <= 0) return false;

    std::vector<uint8_t> px(size_t(w) * size_t(h), 0);
    Surface surf{px, w, h};
    const FilmToBitmap m = film_to_bitmap(L, x0, y0);
    for (const Op& op : g.ops) {
        if (op.kind == Op::Poly) {
            Path path;
            path.move(m.x(op.pts[0], op.pts[1]), m.y(op.pts[0], op.pts[1]));
            for (size_t k = 2; k + 1 < op.pts.size(); k += 2)
                path.line(m.x(op.pts[k], op.pts[k + 1]), m.y(op.pts[k], op.pts[k + 1]));
            path.line(m.x(op.pts[0], op.pts[1]), m.y(op.pts[0], op.pts[1]));   // ClosePath
            fill_path(surf, path, op.gain);
        } else if (op.kind == Op::Circle) {
            Path path;
            ellipse_path(m, op.pts[0], op.pts[1], op.size_mm, path);
            fill_path(surf, path, op.gain);
        } else {
            draw_text(surf, m, op);
        }
    }

    cov.resize(px.size());
    for (size_t i = 0; i < px.size(); ++i) cov[i] = float(px[i]) * (1.0f / 255.0f);
    return apply_blur(cov, w, h, blur_kernel(L, g));
}

#endif  // __APPLE__

}  // namespace edge_raster
}  // namespace spk
