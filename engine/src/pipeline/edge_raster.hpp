// edge_raster.hpp -- the imprint raster surface the overscan draws through.
//
// overscan.cpp lays every film-edge mark out as `Op`s in film millimetres and
// hands each group here: the surface rasterises them into a canvas-aligned
// 8-bit grey coverage bitmap (antialiased polygons, ellipses and text), blurs
// it separably (the projection's softness), and hands back a float coverage
// plane. On macOS the implementation is the upstream CoreText/CoreGraphics/
// vImage code, moved here verbatim; everywhere else stb_truetype (v1.26,
// which parses the CFF outlines of the bundled OTF) draws the same `Op`s
// through one scanline rasteriser, and the Gaussian is a threaded separable
// convolution. The two paths share the bounding-box, transform and blur-
// kernel arithmetic, so the only intentional differences are the glyph
// rasteriser and the convolution's summation order (documented at the
// convolution, ~1e-7 against vImage; bit-identity was never promised).
//
// Faces: the Apple code loads Helvetica Neue, Arial Narrow and Menlo Bold
// from the system. Those cannot be redistributed, so every other platform
// maps them onto the bundled TeX Gyre Heros (GUST Font License, a Helvetica
// clone; recorded in licenses/GUST-FONT-LICENSE.txt and
// resources/fonts/): HelveticaNeue -> Heros Regular, HelveticaNeue-Bold and
// Menlo-Bold -> Heros Bold (no Medium exists; -Medium takes Bold), and
// ArialNarrow -> Heros Regular compressed to Arial Narrow's 0.82 width
// (Heros and Arial share advance widths, so the compression is the
// substitution's only metric difference). The layout -- positions, sizes,
// tracking -- stays the engine's own numbers.
#pragma once

#include <string>
#include <vector>

namespace spk {

struct OverscanLayout;

namespace edge_raster {

// One imprint drawing op, in film millimetres (s along, t across, t down).
// The names follow the Apple surface they were written against.
struct Op {
    enum Kind { Text, Poly, Circle } kind = Text;   // Circle: pts = {s, t}, radius = size_mm
    std::string text;
    const char* font = "HelveticaNeue-Bold";
    double size_mm = 1.0;       // font size (em), mm
    double s = 0, t = 0;        // baseline start (Text) in film mm
    double rot_deg = 0;         // about (s, t)
    double tracking_mm = 0;
    double skew = 0;            // horizontal shear (seven-segment slant)
    double hscale = 1.0;        // Text: glyphs widened along the baseline (Kodak's wide numerals)
    // Text: `hscale` widens the glyphs' shapes only -- CoreText places them at
    // their own advances, so widened letters close up on each other. With this
    // set the whole line is scaled, advances and tracking with it, and the
    // string is `hscale` times as long (Kodak 120's extended face).
    bool scale_advances = false;
    std::vector<double> pts;    // Poly: s0,t0,s1,t1,... in film mm
    double gain = 1.0;          // this mark's share of the group's exposure (printing variance)
};

struct Group {
    bool factory = true;
    std::vector<Op> ops;
    double exposure[3] = {0, 0, 0};   // raw exposure at full coverage, per layer
    double blur_mm = 0.0;
};

// A Helvetica-family string's advance in film mm, as CoreText lays it out
// with the kern attribute set to `tracking_mm` (the font's own pair kerning
// is off; the tracking stands between the characters) and the line's width
// multiplied by `hscale`.
double text_width(const char* font, double size_mm, const std::string& text, double tracking_mm,
                  double hscale);

// Rasterise one group into a canvas-aligned coverage bitmap (its bounding
// box), blurred by the group's softness. Returns false when the group falls
// outside the canvas.
bool rasterise(const OverscanLayout& L, const Group& g, std::vector<float>& cov, int& x0, int& y0,
               int& w, int& h);

// Where the bundled font files live (a `<dir>/fonts/` with the two TeX Gyre
// Heros faces). Set once per process at engine creation, before the first
// text op and before any render thread exists: every face the Op table names
// loads here, so the face table is read-only while renders run concurrently,
// and a missing or unreadable face file is refused here -- `error` names the
// file -- instead of silencing the edge print at render time. Always true on
// macOS, which reads the system's own faces.
bool set_font_directory(const std::string& dir, std::string& error);

}  // namespace edge_raster

}  // namespace spk
