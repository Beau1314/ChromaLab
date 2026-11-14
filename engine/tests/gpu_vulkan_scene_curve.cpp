// Independent headless check for the Windows Decode-curve port (API-SPEC §17,
// phase 2). The reference is scene_latitude.metal's `spk_scene_curve` and its
// `scene_curve_gain` transcribed into C++ double. Guard values exercise the
// rounded-up final workgroup, not just the visible image plane.
//
// Tolerance: the kernel is pointwise over `log2`/`exp2`/linear interpolation
// (PORT_PLAN §0's looser class), so the bound is 1e-6 relative. NaN pixels
// are asserted exactly: they pass as they came.
#include "gpu/gpu.hpp"
#include "gpu_vulkan_read.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
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
        if (std::isnan(expected[i])) {
            if (!std::isnan(double(actual[i]))) {
                char detail[320];
                std::snprintf(detail, sizeof detail,
                    "%s case=%zu index=%zu got=%.9g expected=NaN", name, cases, i, double(actual[i]));
                error = detail;
                return false;
            }
            continue;
        }
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

// scene_latitude.metal's `scene_curve_gain`, in double. `t` is five rows of n
// samples in 0..1 -- rgb, luma, r, g, b -- the value's stops about mid-grey
// read through the table, the gain 2^(f(E) - E) with the ends held.
double scene_curve_gain(double v, const float* t, uint32_t row, uint32_t n, double S,
                        double n_ref, double c) {
    if (!(v > 0.0) || !std::isfinite(v)) return 1.0;
    const double E = std::log2(v / n_ref) + c;
    const double u = std::clamp((E + S) / (2.0 * S), 0.0, 1.0);
    const double x = u * double(n - 1u);
    const uint32_t i = std::min(uint32_t(x), n - 2u);
    const double f = x - double(i);
    const float* r = t + size_t(row) * n;
    const double y = r[i] * (1.0 - f) + r[i + 1u] * f;
    // At the clamp, u is the end's: the gain there is held beyond it.
    const double d = 2.0 * S * (y - u);
    return std::clamp(std::exp2(d), std::exp2(-30.0), std::exp2(30.0));
}

// scene_latitude.metal `spk_scene_curve`, in double. p is the 7-float row:
// [0] n (samples a row), [1] S (stops either side), [2] n_ref, [3..5] the Y
// row of the working space's RGB -> XYZ, [6] Film Exposure in stops.
void reference_scene_curve(const float* rgb, const float* p, const float* t,
                           double* out) {
    const uint32_t n = uint32_t(p[0]);
    const double S = p[1], n_ref = p[2], ec = p[6];
    double c[3] = {rgb[0], rgb[1], rgb[2]};
    const double y = p[3] * std::max(c[0], 0.0) + p[4] * std::max(c[1], 0.0) + p[5] * std::max(c[2], 0.0);
    // Luma first, as a gain on the whole triple (the hue kept); then the rgb
    // row on each channel; then each channel's own row.
    const double luma_gain = scene_curve_gain(y, t, 1u, n, S, n_ref, ec);
    for (double& v : c) v *= luma_gain;
    for (int ch = 0; ch < 3; ++ch) c[ch] *= scene_curve_gain(c[ch], t, 0u, n, S, n_ref, ec);
    for (int ch = 0; ch < 3; ++ch) c[ch] *= scene_curve_gain(c[ch], t, uint32_t(2u + ch), n, S, n_ref, ec);
    out[0] = c[0];
    out[1] = c[1];
    out[2] = c[2];
}

constexpr float kMidgray = 0.184f;
constexpr float kYrow[3] = {0.2126f, 0.7152f, 0.0722f};

struct Profile {
    const char* name;
    uint32_t n;
    float S;
    float exposure_ev;
    // The five rows, each `n` samples in 0..1 (rgb, luma, r, g, b).
    std::vector<float> table;
};

std::vector<Profile> profiles() {
    std::vector<Profile> out;
    auto fill_identity = [](std::vector<float>& table, uint32_t n) {
        for (uint32_t row = 0; row < 5; ++row)
            for (uint32_t i = 0; i < n; ++i) table.push_back(float(i) / float(n - 1u));
    };
    // The identity table: every row the diagonal, every gain exactly 1 (well,
    // 2^0 to the transcribe's precision) -- the whole-image no-op that pins
    // the three-pass order and the axis placement without any curve shape.
    {
        Profile a{}; a.name = "identity, n 9, S 8, c 0";
        a.n = 9; a.S = 8.0f; a.exposure_ev = 0.0f;
        fill_identity(a.table, a.n);
        out.push_back(a);
    }
    // A pull shadow end and a shoulder: gains below 1 at both ends of the
    // axis, Exercise every row through the three passes.
    {
        Profile b{}; b.name = "S curve, n 33, S 4, c +1";
        b.n = 33; b.S = 4.0f; b.exposure_ev = 1.0f;
        for (uint32_t row = 0; row < 5; ++row)
            for (uint32_t i = 0; i < b.n; ++i) {
                const double u = double(i) / double(b.n - 1u);
                // A soft S: 0.5 + 0.5 * tanh((u - 0.5) * 4), clipped to 0..1.
                const double v = 0.5 * (1.0 + std::tanh((u - 0.5) * 4.0));
                b.table.push_back(float(std::clamp(v, 0.0, 1.0)));
            }
        out.push_back(b);
    }
    // The smallest legal table (n = 2: one linear segment end to end) with a
    // pure lift, and a Film Exposure that shifts the axis under it.
    {
        Profile c{}; c.name = "n 2 lift, S 8, c -1.5";
        c.n = 2; c.S = 8.0f; c.exposure_ev = -1.5f;
        for (uint32_t row = 0; row < 5; ++row) {
            c.table.push_back(row == 4 ? 0.02f : 0.05f);   // low end
            c.table.push_back(row == 2 ? 0.95f : 0.98f);   // high end
        }
        out.push_back(c);
    }
    // Extreme ends: the table pins 0 at u = 0 and 1 at u = 1, so the held end
    // gains hit the exp2(-30)/exp2(30) clamps for far-off-axis values.
    {
        Profile d{}; d.name = "pinned ends, n 65, S 3, c 0";
        d.n = 65; d.S = 3.0f; d.exposure_ev = 0.0f;
        for (uint32_t row = 0; row < 5; ++row)
            for (uint32_t i = 0; i < d.n; ++i)
                d.table.push_back(i == 0 ? 0.0f : i == d.n - 1 ? 1.0f
                                  : float(0.4 + 0.2 * (double(i) / double(d.n - 1u))));
        out.push_back(d);
    }
    return out;
}

// Linear RGB spanning the curve's usable range, including 0, negatives, the
// metered mid-grey and huge-but-float-finite ends; every seventh pixel NaN on
// all three channels (a mixed-NaN pixel's luma differs between max
// semantics, so the pass-through is only pinned exactly for all-channel NaN).
bool check_curve(Gpu* gpu, std::string& error) {
    const uint32_t counts[] = {0, 1, 2, 255, 256, 257, 1025};
    const float values[] = {-2.0f, -0.25f, 0.0f, 1e-9f, 0.01f, 0.05f, 0.1f, kMidgray,
                            0.25f, 0.5f, 1.0f, 2.0f, 8.0f, 64.0f, 1e9f, 1e30f};
    constexpr size_t value_count = sizeof values / sizeof values[0];
    for (const Profile& profile : profiles())
        for (uint32_t count : counts) {
            float p[7] = {float(profile.n), profile.S, kMidgray,
                          kYrow[0], kYrow[1], kYrow[2], profile.exposure_ev};
            std::vector<float> rgb(std::max(count, 1u) * 3);
            for (uint32_t i = 0; i < std::max(count, 1u); ++i) {
                if (i % 7 == 3) {
                    const float nan = std::numeric_limits<float>::quiet_NaN();
                    rgb[3 * i] = nan; rgb[3 * i + 1] = nan; rgb[3 * i + 2] = nan;
                } else {
                    rgb[3 * i] = values[i % value_count];
                    rgb[3 * i + 1] = values[(i + 5) % value_count];
                    rgb[3 * i + 2] = values[(i + 9) % value_count];
                }
            }
            auto input = gpu->upload(rgb.data(), rgb.size() * sizeof(float), error);
            auto parameters = gpu->upload(p, sizeof p, error);
            auto table = gpu->upload(profile.table.data(), profile.table.size() * sizeof(float), error);
            auto output = output_buffer(gpu, size_t(count) * 3, error);
            const uint32_t n[] = {count};
            std::vector<double> expected(size_t(count) * 3, std::numeric_limits<double>::quiet_NaN());
            for (uint32_t i = 0; i < count; ++i)
                reference_scene_curve(rgb.data() + 3 * i, p, profile.table.data(), expected.data() + 3 * i);
            if (!input || !parameters || !table || !output ||
                !gpu->dispatch("spk_scene_curve", {Arg::buf(input), Arg::buf(parameters),
                                   Arg::buf(table), Arg::inline_bytes(n, 1), Arg::buf(output)},
                                   std::max(count, 1u), error) ||
                !check_values(gpu, output, expected, profile.name, error)) return false;
        }
    return true;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr, "usage: spk_vulkan_scene_curve <resources>\n"); return 2; }
    std::string error;
    std::unique_ptr<Gpu> gpu(Gpu::create_vulkan(std::string(argv[1]) + "/vulkan", error));
    if (!gpu) { std::fprintf(stderr, "Vulkan device: %s\n", error.c_str()); return 1; }
    std::printf("Vulkan device: %s\n", gpu->device_name().c_str());
    if (!check_curve(gpu.get(), error)) {
        std::fprintf(stderr, "scene_curve: %s\n", error.c_str()); return 1;
    }
    std::printf("scene_curve: %zu float64 cases, max abs error %.9g, "
                "max error/max(1,abs(reference)) %.9g, all guards intact\n",
                cases, maximum_error, maximum_scaled_error);
    return 0;
}
