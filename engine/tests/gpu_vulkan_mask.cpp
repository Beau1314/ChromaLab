// Independent headless check for the Windows contrast-mask port (RFC-024).
// The references are contrast_mask.metal's `spk_mask_reduce` (:80) and
// `spk_mask_epilogue` (:109) -- with their inline helpers `mask_image_exposure`
// (:34), `mask_x` (:47), `mask_g` (:58) and `mask_delta` (:62) -- transcribed
// into C++ double, so the gate states the kernels' own numerics, not a second
// GPU implementation. Guard values exercise the rounded-up final workgroup,
// not just the logical output count.
//
// Tolerance: the kernels carry `exp2`/`log2`/`log10`/`rsqrt` (PORT_PLAN §0's
// looser class), so the bound is 1e-6 relative with a 1e-6 absolute floor.
// The reduce additionally accumulates a cell's pixels in float32 -- the cell
// pixel counts here are kept small (<= 144) so that accumulation stays inside
// the same bound (production cells are of the same size: the grid is
// `kCellsPerSigma` cells per blur sigma on the long edge).
//
// The reduce's cells partition the frame (`[cx*W/gw, (cx+1)*W/gw)`), so one
// case uses an uneven frame (4096x256 over a 2048x33 grid) to pin the
// truncated partition and the workgroup folding (67584 > 65535 groups) in one
// case, and a single-cell case pins the whole-frame mean.
#include "gpu/gpu.hpp"
#include "gpu_vulkan_read.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {
using spk::gpu::Arg;
using spk::gpu::BufferRef;
using spk::gpu::Gpu;
constexpr float kGuard = -12345.25f;
constexpr size_t kGuardCount = 11;
size_t cases = 0;
double maximum_error = 0;
double maximum_scaled_error = 0;

BufferRef output_buffer(Gpu* gpu, size_t count, std::string& error) {
    const std::vector<float> guarded(count + kGuardCount, kGuard);
    return gpu->upload(guarded.data(), guarded.size() * sizeof(float), error);
}

bool check_values(Gpu* gpu, const BufferRef& output, const std::vector<double>& expected,
                  const char* name, std::string& error) {
    if (!gpu->flush(error)) return false;
    std::vector<float> actual_readback;
    if (!read_test_buffer(gpu, output, actual_readback, error)) return false;
    const float* actual = actual_readback.data();
    const double absolute_tolerance = 1e-6, relative_tolerance = 1e-6;
    for (size_t i = 0; i < expected.size(); ++i) {
        const double delta = std::fabs(double(actual[i]) - expected[i]);
        const double tolerance = absolute_tolerance + relative_tolerance * std::fabs(expected[i]);
        if (!(std::isfinite(actual[i]) && delta <= tolerance)) {
            char detail[320];
            std::snprintf(detail, sizeof detail,
                "%s case=%zu index=%zu got=%.9g expected=%.17g delta=%.9g tolerance=%.9g",
                name, cases, i, double(actual[i]), expected[i], delta, tolerance);
            error = detail;
            return false;
        }
        maximum_error = std::max(maximum_error, delta);
        maximum_scaled_error = std::max(maximum_scaled_error,
                                        delta / std::max(1.0, std::fabs(expected[i])));
    }
    for (size_t i = expected.size(); i < expected.size() + kGuardCount; ++i) {
        if (actual[i] != kGuard) {
            error = std::string(name) + " wrote past its logical output count";
            return false;
        }
    }
    ++cases;
    return true;
}

// --- the Metal inline helpers, in double -------------------------------------

// contrast_mask.metal :34 `mask_image_exposure`: the enlarger's spectral
// integral over the CMY triple, then the setup gain. `p` is the documented
// 16-float row (contrast_mask.metal:20-26):
//   0..2 the enlarger's setup gain   3..5 1 / A_ref   6..8 the pre-flash offset
//   9 k_lo  10 h_lo  11 lo_on        12 k_hi 13 h_hi 14 hi_on
//   15 the gain limit, stops
void reference_image_exposure(const float* cmy, uint32_t i, const float* chd, const float* base,
                              const float* ixs, uint32_t nl, const float* p, double a[3]) {
    const double c0 = cmy[3 * size_t(i)], c1 = cmy[3 * size_t(i) + 1], c2 = cmy[3 * size_t(i) + 2];
    a[0] = a[1] = a[2] = 0.0;
    for (uint32_t l = 0; l < nl; ++l) {
        const double d = c0 * chd[3 * l] + c1 * chd[3 * l + 1] + c2 * chd[3 * l + 2] + base[l];
        const double t = std::exp2(-d * 3.321928094887362);
        a[0] += t * ixs[3 * l];
        a[1] += t * ixs[3 * l + 1];
        a[2] += t * ixs[3 * l + 2];
    }
    a[0] *= p[0]; a[1] *= p[1]; a[2] *= p[2];
}

// :47 `mask_x`: the mean of the per-channel log2 ratios against A_ref, floored
// at -20 (a numerical guard, not a signal). The double log2(0) is -inf, which
// the floor then clamps exactly as the kernel's comment states.
double reference_x(const double a[3], const float* p) {
    double s = 0.0;
    for (int c = 0; c < 3; ++c) {
        const double r = std::max(a[c] * double(p[3 + c]), 0.0);
        s += std::max(std::log2(r), -20.0);
    }
    return s * (1.0 / 3.0);
}

// :58 `mask_g`: RFC-023's m = 2 branch, `D * H / sqrt(H^2 + D^2)`; the kernel
// writes it through rsqrt.
double reference_g(double d, double h) { return d * h / std::sqrt(h * h + d * d); }

// :62 `mask_delta`: f(b) - b on the enabled branches, then the same smooth-min
// bound on the gain itself. Exactly 0.0 inside the core and on a disabled
// branch.
double reference_delta(double b, const float* p) {
    double delta = 0.0;
    if (p[11] != 0.0f && b < p[9]) {
        const double d = p[9] - b;
        delta = d - reference_g(d, p[10]);
    } else if (p[14] != 0.0f && b > p[12]) {
        const double d = b - p[12];
        delta = reference_g(d, p[13]) - d;
    }
    const double lim = p[15];
    return delta * lim / std::sqrt(lim * lim + delta * delta);
}

// --- kernel 1: spk_mask_reduce (:80) -----------------------------------------
//
// One thread per grid cell, the mean of `x` over the cell's pixels; the cells
// partition the frame, so every pixel is counted exactly once.
void reference_reduce(const std::vector<float>& cmy, const float* chd, const float* base,
                      const float* ixs, const float* p, uint32_t W, uint32_t H, uint32_t gw,
                      uint32_t gh, uint32_t nl, std::vector<double>& out) {
    out.assign(size_t(gw) * gh, 0.0);
    for (uint32_t cell = 0; cell < gw * gh; ++cell) {
        const uint32_t cy = cell / gw, cx = cell % gw;
        uint32_t x0 = (cx * W) / gw, x1 = std::max(((cx + 1u) * W) / gw, x0 + 1u);
        uint32_t y0 = (cy * H) / gh, y1 = std::max(((cy + 1u) * H) / gh, y0 + 1u);
        x1 = std::min(x1, W);
        y1 = std::min(y1, H);
        double acc = 0.0;
        for (uint32_t y = y0; y < y1; ++y)
            for (uint32_t x = x0; x < x1; ++x) {
                double a[3];
                reference_image_exposure(cmy.data(), y * W + x, chd, base, ixs, nl, p, a);
                acc += reference_x(a, p);
            }
        out[cell] = acc / double((x1 - x0) * (y1 - y0));
    }
}

// --- kernel 2: spk_mask_epilogue (:109) --------------------------------------
//
// Per pixel of a band: the same integral, the base `a * x + b` with (a, b)
// sampled bilinearly from the grid, the bounded delta as an exp2 gain on the
// image exposure *before* the pre-flash offset, and the log10 of the bounded
// exposure -- the same epilogue shape as spk_spectral_epilogue.
void reference_epilogue(const std::vector<float>& cmy, const float* chd, const float* base,
                        const float* ixs, const float* p, const float* coef, uint32_t n,
                        uint32_t nl, uint32_t W, uint32_t H, uint32_t row0, uint32_t gw,
                        uint32_t gh, std::vector<double>& out) {
    out.assign(size_t(n) * 3, 0.0);
    for (uint32_t i = 0; i < n; ++i) {
        double a[3];
        reference_image_exposure(cmy.data(), i, chd, base, ixs, nl, p, a);
        const uint32_t px = i % W, py = row0 + i / W;
        const double u = std::clamp((double(px) + 0.5) * double(gw) / double(W) - 0.5, 0.0,
                                    double(gw - 1u));
        const double v = std::clamp((double(py) + 0.5) * double(gh) / double(H) - 0.5, 0.0,
                                    double(gh - 1u));
        const uint32_t u0 = uint32_t(std::floor(u)), v0 = uint32_t(std::floor(v));
        const uint32_t u1 = std::min(u0 + 1u, gw - 1u), v1 = std::min(v0 + 1u, gh - 1u);
        const double tu = u - u0, tv = v - v0;
        // mix(mix(c00, c01, tu), mix(c10, c11, tu), tv) -- bilinear over the
        // grid's (a, b) pairs.
        const double at_u0 = 1.0 - tu, at_u1 = tu, at_v0 = 1.0 - tv, at_v1 = tv;
        const double ax =
            (coef[2 * (v0 * gw + u0)] * at_u0 + coef[2 * (v0 * gw + u1)] * at_u1) * at_v0 +
            (coef[2 * (v1 * gw + u0)] * at_u0 + coef[2 * (v1 * gw + u1)] * at_u1) * at_v1;
        const double bx =
            (coef[2 * (v0 * gw + u0) + 1] * at_u0 + coef[2 * (v0 * gw + u1) + 1] * at_u1) * at_v0 +
            (coef[2 * (v1 * gw + u0) + 1] * at_u0 + coef[2 * (v1 * gw + u1) + 1] * at_u1) * at_v1;
        const double b = ax * reference_x(a, p) + bx;
        const double k = std::exp2(reference_delta(b, p));
        for (int c = 0; c < 3; ++c)
            out[3 * size_t(i) + c] = std::log10(std::max(k * a[c] + double(p[6 + c]), 0.0) + 1e-10);
    }
}

// --- fixtures ----------------------------------------------------------------

// The branch's room `H` such that a base `kReach` stops past the knee moves by
// exactly `amount` stops after the gain bound (contrast_mask.cpp `room_for`):
// invert the bound, then solve. R = 4 stops of reach, L = 6 stops of limit.
double room_for(double amount) {
    const double L = 6.0, R = 4.0;
    const double delta = amount * L / std::sqrt(L * L - amount * amount);
    const double t = std::max(R - delta, 1e-3);
    return R * t / std::sqrt(R * R - t * t);
}

struct MaskProfile {
    const char* name;
    float p[16];
    uint32_t nl;  // wavelengths in this case's tables
};

std::vector<MaskProfile> profiles() {
    std::vector<MaskProfile> out;
    auto push = [&](const char* name, float gain0, float inv_ref, float flash, float core,
                    double highlights, double shadows, float limit, uint32_t nl) {
        MaskProfile m{};
        m.name = name;
        m.nl = nl;
        m.p[0] = gain0; m.p[1] = gain0 * 1.02f; m.p[2] = gain0 * 0.97f;
        m.p[3] = inv_ref; m.p[4] = inv_ref * 1.01f; m.p[5] = inv_ref * 0.99f;
        m.p[6] = flash; m.p[7] = flash * 0.8f; m.p[8] = -flash * 0.6f;
        m.p[9] = -core;
        m.p[10] = float(highlights > 0.0 ? room_for(highlights) : 1.0);
        m.p[11] = highlights > 0.0 ? 1.0f : 0.0f;
        m.p[12] = core;
        m.p[13] = float(shadows > 0.0 ? room_for(shadows) : 1.0);
        m.p[14] = shadows > 0.0 ? 1.0f : 0.0f;
        m.p[15] = limit;
        out.push_back(m);
    };
    // Both branches on, engine-shaped rooms (room_for on both amounts).
    push("both branches", 0.9f, 2.5f, 0.01f, 1.0f, 1.2, 0.8, 6.0f, 3);
    // Print-highlight branch only (a dense negative, raised print highlights).
    push("highlight only", 1.1f, 2.2f, 0.02f, 0.8f, 2.0, 0.0, 6.0f, 3);
    // Print-shadow branch only, and a tighter gain limit so the smooth bound
    // saturates on deep-shadow pixels.
    push("shadow bound", 1.0f, 2.0f, 0.0f, 0.5f, 0.0, 2.5, 2.0f, 3);
    // Neither branch: delta == 0 exactly, so the kernel must be the spectral
    // epilogue's own epilogue -- the structural-bypass shape the pipeline
    // relies on when the user's amounts are zero.
    push("bypass", 0.85f, 2.4f, 0.015f, 1.0f, 0.0, 0.0, 6.0f, 3);
    // Deep floor: densities that underflow the transmission to zero push a_c
    // through the log2(0) = -inf path onto the -20 floor. The full 81
    // wavelengths the engine itself uses.
    push("log2 floor", 0.9f, 2.5f, 0.01f, 1.0f, 1.0, 1.0, 6.0f, 81);
    return out;
}

// Printing-density-like CMY spanning the negative's usable range: the bulk
// around mid-grey, an every-9th specular white and an every-11th deep shadow,
// with an every-23rd sub-channel below base so negatives see both signs.
// `chd_scale` 12 pushes the deep-floor profile's densities past transmission
// underflow; 1.0 keeps the others on the curve's useful range.
std::vector<float> cmy_fixture(uint32_t pixels, float chd_scale) {
    std::vector<float> cmy(size_t(pixels) * 3);
    for (uint32_t i = 0; i < pixels; ++i) {
        float c[3] = {0.35f + float(i % 17) * 0.055f, 0.42f + float(i % 13) * 0.05f,
                      0.30f + float(i % 19) * 0.045f};
        if (i % 9 == 0)
            for (float& v : c) v = 0.0f;   // specular: no density
        if (i % 11 == 0)
            for (float& v : c) v = 2.6f;   // deep shadow
        if (i % 23 == 0) c[i % 3] = -0.25f;
        for (int ch = 0; ch < 3; ++ch) cmy[3 * size_t(i) + ch] = c[ch] * chd_scale;
    }
    return cmy;
}

struct Tables {
    std::vector<float> packed;  // chd | base | ixs, one upload each below
    const float* chd;
    const float* base;
    const float* ixs;
};

Tables wavelength_tables(uint32_t nl, float chd_scale) {
    Tables t;
    t.packed.resize(size_t(nl) * 7);
    t.chd = t.packed.data();
    t.base = t.chd + size_t(nl) * 3;
    t.ixs = t.base + nl;
    for (uint32_t l = 0; l < nl; ++l) {
        for (int c = 0; c < 3; ++c)
            t.packed[3 * l + c] = (0.55f + 0.08f * float(l % 5) + 0.03f * float(c)) * chd_scale;
        t.packed[size_t(nl) * 3 + l] = 0.10f + 0.02f * float(l % 4);
        t.packed[size_t(nl) * 4 + 3 * l] = 0.9f + 0.01f * float(l % 7);
        t.packed[size_t(nl) * 4 + 3 * l + 1] = 1.0f + 0.01f * float(l % 5);
        t.packed[size_t(nl) * 4 + 3 * l + 2] = 1.1f - 0.01f * float(l % 6);
    }
    return t;
}

bool check_reduce(Gpu* gpu, std::string& error) {
    struct Grid { uint32_t W, H, gw, gh; };
    // The first two partition evenly (4x4-pixel cells); the third is uneven
    // (truncated cells of differing widths); the fourth also folds past
    // maxComputeWorkGroupCount[0]; the fifth is the whole-frame single cell;
    // the sixth is one pixel per cell; the last makes the grid finer than the
    // frame, where the max(x0+1) clamp widens every cell back to at least one
    // pixel (the engine never builds such a grid; the transcription pins the
    // arithmetic as written).
    const Grid grids[] = {
        {32, 16, 8, 4},
        {64, 48, 16, 12},
        {33, 17, 11, 6},
        {4096, 256, 2048, 33},
        {24, 18, 1, 1},
        {32, 16, 32, 16},
        {8, 8, 16, 16},
    };
    for (const MaskProfile& profile : profiles()) {
        // The log2-floor profile needs the deep densities; the others stay
        // on the curve's useful range.
        const float chd_scale = std::string(profile.name) == "log2 floor" ? 12.0f : 1.0f;
        const Tables tables = wavelength_tables(profile.nl, chd_scale);
        auto chd_buf = gpu->upload(tables.chd, size_t(profile.nl) * 3 * sizeof(float), error);
        auto base_buf = gpu->upload(tables.base, size_t(profile.nl) * sizeof(float), error);
        auto ixs_buf = gpu->upload(tables.ixs, size_t(profile.nl) * 3 * sizeof(float), error);
        auto parameters = gpu->upload(profile.p, sizeof profile.p, error);
        if (!chd_buf || !base_buf || !ixs_buf || !parameters) return false;
        for (const Grid& g : grids) {
            const std::vector<float> cmy = cmy_fixture(g.W * g.H, chd_scale);
            auto input = gpu->upload(cmy.data(), cmy.size() * sizeof(float), error);
            auto output = output_buffer(gpu, size_t(g.gw) * g.gh, error);
            if (!input || !output) return false;
            const uint32_t meta[5] = {g.W, g.H, g.gw, g.gh, profile.nl};
            std::vector<double> expected;
            reference_reduce(cmy, tables.chd, tables.base, tables.ixs, profile.p, g.W, g.H,
                             g.gw, g.gh, profile.nl, expected);
            if (!gpu->dispatch("spk_mask_reduce",
                               {Arg::buf(input), Arg::buf(chd_buf), Arg::buf(base_buf),
                                Arg::buf(ixs_buf), Arg::buf(parameters),
                                Arg::inline_bytes(meta, 5), Arg::buf(output)},
                               size_t(g.gw) * g.gh, error))
                return false;
            if (!check_values(gpu, output, expected, profile.name, error)) return false;
        }
    }
    return true;
}

bool check_epilogue(Gpu* gpu, std::string& error) {
    // The band shape: a frame of W x H, bands of `row0`'s offset. meta is
    // {n, nl, W, H, row0, gw, gh}; i indexes the band's pixels, py the frame's
    // rows, so a striped render must sample exactly what the whole-frame one
    // does (contrast_mask.metal:102-104).
    struct Band { uint32_t n, W, H, row0, gw, gh; };
    const Band bands[] = {
        {1025, 64, 64, 16, 16, 16},   // the analysis grid, a mid-frame band
        {63, 64, 64, 0, 16, 16},      // short band from the frame's top
        {0, 64, 64, 16, 16, 16},      // empty band: nothing may be written
        {256, 32, 32, 8, 8, 8},       // aligned count and grid
        {257, 32, 32, 8, 8, 8},       // rounded-up final workgroup
        {255, 32, 32, 8, 8, 8},       // one short of the final workgroup
        {1, 32, 32, 31, 8, 8},        // the frame's last row, off-grid centre
        {97, 33, 40, 11, 33, 40},     // grid == frame: the degenerate 1:1 map
    };
    uint32_t shape_index = 0;
    for (const MaskProfile& profile : profiles()) {
        const Tables tables = wavelength_tables(profile.nl, 1.0f);
        auto chd_buf = gpu->upload(tables.chd, size_t(profile.nl) * 3 * sizeof(float), error);
        auto base_buf = gpu->upload(tables.base, size_t(profile.nl) * sizeof(float), error);
        auto ixs_buf = gpu->upload(tables.ixs, size_t(profile.nl) * 3 * sizeof(float), error);
        auto parameters = gpu->upload(profile.p, sizeof profile.p, error);
        if (!chd_buf || !base_buf || !ixs_buf || !parameters) return false;
        for (const Band& b : bands) {
            // Three coefficient shapes, cycled per band: a = 0 everywhere (the
            // Gaussian base, the product scheme -- the base is purely the
            // stored b), a = 1 (RFC-024's pointwise comparator -- the base
            // follows this pixel's own x), and per-cell-varying coefficients
            // that exercise the bilinear including the clamped edge cells.
            const uint32_t shape = shape_index++ % 3;
            std::vector<float> coef;
            if (shape == 0) coef.assign(size_t(b.gw) * b.gh * 2, 0.0f);
            else if (shape == 1)
                for (size_t i = 0; i < size_t(b.gw) * b.gh; ++i) {
                    coef.push_back(1.0f);
                    coef.push_back(-0.05f);
                }
            else
                for (uint32_t y = 0; y < b.gh; ++y)
                    for (uint32_t x = 0; x < b.gw; ++x) {
                        coef.push_back(0.4f + 0.5f * float(x) / float(b.gw));    // a
                        coef.push_back(-0.10f + 0.2f * float(y) / float(b.gh));  // b
                    }
            const std::vector<float> cmy = cmy_fixture(b.n, 1.0f);
            auto coefficients = gpu->upload(coef.data(), coef.size() * sizeof(float), error);
            // The empty band carries no pixels: upload a placeholder (the
            // kernel's `i >= n` guard reads nothing of it).
            auto input = b.n > 0 ? gpu->upload(cmy.data(), cmy.size() * sizeof(float), error)
                                 : gpu->upload(&kGuard, sizeof(float), error);
            auto output = output_buffer(gpu, size_t(b.n) * 3, error);
            if (!coefficients || !input || !output) return false;
            const uint32_t meta[7] = {b.n, profile.nl, b.W, b.H, b.row0, b.gw, b.gh};
            std::vector<double> expected;
            reference_epilogue(cmy, tables.chd, tables.base, tables.ixs, profile.p, coef.data(),
                               b.n, profile.nl, b.W, b.H, b.row0, b.gw, b.gh, expected);
            if (!gpu->dispatch("spk_mask_epilogue",
                               {Arg::buf(input), Arg::buf(chd_buf), Arg::buf(base_buf),
                                Arg::buf(ixs_buf), Arg::buf(parameters), Arg::buf(coefficients),
                                Arg::inline_bytes(meta, 7), Arg::buf(output)},
                               std::max(b.n, 1u), error))
                return false;
            if (!check_values(gpu, output, expected, profile.name, error)) return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr, "usage: spk_vulkan_mask <resources>\n"); return 2; }
    std::string error;
    std::unique_ptr<Gpu> gpu(Gpu::create_vulkan(std::string(argv[1]) + "/vulkan", error));
    if (!gpu) { std::fprintf(stderr, "Vulkan device: %s\n", error.c_str()); return 1; }
    std::printf("Vulkan device: %s\n", gpu->device_name().c_str());
    if (!check_reduce(gpu.get(), error)) {
        std::fprintf(stderr, "mask reduce: %s\n", error.c_str()); return 1;
    }
    if (!check_epilogue(gpu.get(), error)) {
        std::fprintf(stderr, "mask epilogue: %s\n", error.c_str()); return 1;
    }
    std::printf("mask: %zu float64 cases, max abs error %.9g, "
                "max error/max(1,abs(reference)) %.9g, all guards intact\n",
                cases, maximum_error, maximum_scaled_error);
    return 0;
}
