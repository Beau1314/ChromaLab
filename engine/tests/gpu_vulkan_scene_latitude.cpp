// Independent headless check for the Windows Scene Latitude Mapping port
// (RFC-023, phase 2). The reference is scene_latitude.metal's `spk_scene_latitude`
// transcribed into C++ double, so the gate states the kernel's own numerics --
// the same arithmetic, in the same order, as core/latitude_fit.cpp's `departure`
// (the file the Metal comment names as its double-precision reference). Guard
// values exercise the rounded-up final workgroup, not just the visible plane.
//
// Tolerance: the kernel is pointwise but carries `log2`/`exp2`/`pow` (PORT_PLAN
// §0's looser class). 1e-6 relative is not enough here: the huge-value cases
// drive E onto the ±24 stop clamp, and the departure then works on D of about
// 22 stops, so the float32 chain's ULP error reaches ~3e-6 relative on the
// gain (measured worst case, the m = 2 form at the clamped end -- the error
// grows with the branch depth |d|). 5e-6 is the honest bound; a transcription
// mistake (wrong constant, wrong branch) is orders of magnitude larger. The
// signal itself is linear RGB multiplied by a scalar gain, so the relative
// floor also covers the huge-value cases; NaN pixels are asserted exactly
// (they must pass through unchanged, §11's rule).
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
    const double absolute_tolerance = 1e-6, relative_tolerance = 5e-6;
    for (size_t i = 0; i < expected.size(); ++i) {
        if (std::isnan(expected[i])) {
            // §11: a pathological pixel passes as it came -- exact NaN, not a
            // float-overflow artefact and not a 0.
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

// scene_latitude.metal's roll-off helpers (`:1-42`), in double: `g_m(D) - D`,
// the branch's departure from the identity, in the forms that do not cancel
// near the knee. Exactly 0 at D = 0 through the multiply, negative beyond it.
double slm_departure(double D, double H, double m) {
    if (!(D > 0.0)) return 0.0;
    if (m == 2.0) {
        const double r = std::sqrt(H * H + D * D);
        return -(D * D * D) / (r * (H + r));
    }
    if (m == 1.0) return -(D * D) / (H + D);
    return D * (H / std::pow(std::pow(H, m) + std::pow(D, m), 1.0 / m) - 1.0);
}

// scene_latitude.metal `spk_scene_latitude`, in double. p is the documented
// 19-float row: [0] K_h, [1] H_h (0 = off), [2] K_s, [3] H_s (0 = off), [4] m,
// [5] L_max, [6] norm (0 power, 1 Y, 2 max), [7..9] the working space's Y row,
// [10] n_ref, [11] width, [12] height, [13] split (0 = none), [14..17] the
// second frame's K_h H_h K_s H_s, [18] the image's first row in the frame.
void reference_scene_latitude(const float* rgb, const float* p, const uint32_t* n,
                              uint32_t index, double* out) {
    const double r = rgb[3u * index], g = rgb[3u * index + 1], b = rgb[3u * index + 2];
    const double cr = std::max(r, 0.0), cg = std::max(g, 0.0), cb = std::max(b, 0.0);

    double v;
    const uint32_t norm = uint32_t(p[6]);
    if (norm == 1u) {
        v = p[7] * cr + p[8] * cg + p[9] * cb;
    } else if (norm == 2u) {
        v = std::max(cr, std::max(cg, cb));
    } else {
        const double s2 = cr * cr + cg * cg + cb * cb;
        v = s2 > 0.0 ? (cr * cr * cr + cg * cg * cg + cb * cb * cb) / s2 : 0.0;
    }

    double k = 1.0;
    if (v > 0.0 && std::isfinite(v)) {
        const double n_ref = p[10];
        const double E = std::clamp(std::log2(std::max(v, n_ref * std::exp2(-24.0)) / n_ref),
                                    -24.0, 24.0);
        double d = 0.0;
        // A half-frame pair: from `split` along the long edge, the second
        // frame's own knees and rooms (p[14..17]).
        double Kh = p[0], Hh = p[1], Ks = p[2], Hs = p[3];
        if (p[13] > 0.0f) {
            const uint32_t w = uint32_t(p[11]), h = uint32_t(p[12]);
            const double along = w >= h
                ? (double(index % w) + 0.5) / double(w)
                : (double(index / w) + p[18] + 0.5) / double(h);
            if (along >= p[13]) { Kh = p[14]; Hh = p[15]; Ks = p[16]; Hs = p[17]; }
        }
        if (Hh > 0.0 && E > Kh) d = slm_departure(E - Kh, Hh, p[4]);
        else if (Hs > 0.0 && E < Ks) d = -slm_departure(Ks - E, Hs, p[4]);
        if (d > 0.0) { const double L = p[5]; d = d * L / std::sqrt(L * L + d * d); }
        k = std::clamp(std::exp2(d), std::exp2(-30.0), std::exp2(30.0));
    }
    out[0] = r * k;
    out[1] = g * k;
    out[2] = b * k;
}

struct Profile {
    const char* name;
    float p[19];
};

// The engine's own constants (core/latitude_fit.hpp): mid-grey 0.184 and the
// sRGB working space's Y row as the AE matrix carries it. The Y row's exact
// numbers do not matter to the gate (both sides read the same p row); a
// plausible one keeps the Y-norm profile honest.
constexpr float kMidgray = 0.184f;
constexpr float kYrow[3] = {0.2126f, 0.7152f, 0.0722f};

std::vector<Profile> profiles() {
    std::vector<Profile> out;
    auto base = [](float* p) {
        p[0] = 1.8f; p[1] = 0.0f; p[2] = -1.8f; p[3] = 0.0f;   // knees, both sides off
        p[4] = 2.0f; p[5] = 4.0f; p[6] = 0.0f;
        p[7] = kYrow[0]; p[8] = kYrow[1]; p[9] = kYrow[2];
        p[10] = kMidgray;
        p[11] = 8.0f; p[12] = 4.0f; p[13] = 0.0f;
        p[14] = 1.2f; p[15] = 0.0f; p[16] = -1.2f; p[17] = 0.0f;
        p[18] = 0.0f;
    };
    // A mild highlight-only curve at the engine's default roll-off: every
    // room-zero branch must be skipped outright.
    {
        Profile a{}; a.name = "highlight only, m 2";
        base(a.p);
        a.p[1] = 1.4f;
        out.push_back(a);
    }
    // Both sides on at m = 1 (the closed-form roll-off shape).
    {
        Profile b{}; b.name = "both sides, m 1";
        base(b.p);
        b.p[1] = 2.2f; b.p[3] = 2.6f; b.p[4] = 1.0f; b.p[6] = 1.0f;
        out.push_back(b);
    }
    // A general m (3.5) exercises the pow form of the departure, the Y norm
    // and a deep shadow lift against the smooth bound.
    {
        Profile c{}; c.name = "general m 3.5, Y norm";
        base(c.p);
        c.p[1] = 1.1f; c.p[3] = 3.0f; c.p[4] = 3.5f; c.p[5] = 1.5f; c.p[6] = 1.0f;
        out.push_back(c);
    }
    // Max norm with saturating rooms: E pins at the floor and the lift bound
    // (d * L / sqrt(L^2 + d^2)) is active rather than a limit case.
    {
        Profile d{}; d.name = "max norm, bound active";
        base(d.p);
        d.p[1] = 0.9f; d.p[3] = 0.8f; d.p[4] = 2.0f; d.p[5] = 0.5f; d.p[6] = 2.0f;
        out.push_back(d);
    }
    // A half-frame pair: split along the long edge, the second frame's own
    // knees and rooms (p[14..17]); the strip's first row (p[18]) is nonzero to
    // pin the RFC-020 executor's placement term.
    {
        Profile e{}; e.name = "split pair";
        base(e.p);
        e.p[1] = 1.6f; e.p[3] = 1.6f; e.p[13] = 0.55f; e.p[18] = 2.0f;
        e.p[14] = 0.8f; e.p[15] = 2.0f; e.p[16] = -1.0f; e.p[17] = 2.4f;
        out.push_back(e);
    }
    // The pair read by a tall plane (h > w): the long edge is the height and
    // `along` walks rows, so the second curve falls along the piece.
    {
        Profile f{}; f.name = "split pair, tall";
        base(f.p);
        f.p[11] = 3.0f; f.p[12] = 32.0f;
        f.p[1] = 1.6f; f.p[3] = 1.6f; f.p[13] = 0.4f; f.p[18] = 0.0f;
        f.p[14] = 2.4f; f.p[15] = 1.3f; f.p[16] = -2.4f; f.p[17] = 1.3f;
        out.push_back(f);
    }
    return out;
}

// Linear RGB spanning the node's whole usable range: 0, negatives, values
// parked on the stop-axis floor, the metered mid-grey, the medium's
// boundaries and huge ends; every seventh pixel NaN on all three channels
// (a mixed-NaN pixel's norm differs between max semantics, so §11's
// pass-through is only pinned exactly for all-channel NaN). The largest
// value is 1e9: huge enough that E pins at the +24 clamp (0.184 * 2^24 is
// about 3.1e6), but its cube and square stay inside float32's range, where
// the kernel's own norm runs -- anything larger would make the float32
// norm overflow to inf/NaN while the double reference stays finite, and the
// gate would be comparing two different domains.
bool check_latitude(Gpu* gpu, std::string& error) {
    const uint32_t counts[] = {0, 1, 2, 255, 256, 257, 1025};
    const float values[] = {-2.0f, -0.25f, 0.0f, 1e-9f, 0.01f, 0.05f, 0.1f, kMidgray,
                            0.25f, 0.5f, 1.0f, 2.0f, 8.0f, 64.0f, 1e6f, 1e9f};
    constexpr size_t value_count = sizeof values / sizeof values[0];
    for (const Profile& profile : profiles())
        for (uint32_t count : counts) {
            std::vector<float> rgb(std::max(count, 1u) * 3);
            std::vector<bool> nan_pixel(std::max(count, 1u));
            for (uint32_t i = 0; i < std::max(count, 1u); ++i) {
                const float r = values[i % value_count];
                const float g = values[(i + 5) % value_count];
                const float b = values[(i + 9) % value_count];
                if (i % 7 == 3) {
                    rgb[3 * i] = std::numeric_limits<float>::quiet_NaN();
                    rgb[3 * i + 1] = std::numeric_limits<float>::quiet_NaN();
                    rgb[3 * i + 2] = std::numeric_limits<float>::quiet_NaN();
                    nan_pixel[i] = true;
                } else {
                    rgb[3 * i] = r;
                    rgb[3 * i + 1] = g;
                    rgb[3 * i + 2] = b;
                }
            }
            auto input = gpu->upload(rgb.data(), rgb.size() * sizeof(float), error);
            auto parameters = gpu->upload(profile.p, sizeof profile.p, error);
            auto output = output_buffer(gpu, size_t(count) * 3, error);
            const uint32_t n[] = {count};
            std::vector<double> expected(size_t(count) * 3, std::numeric_limits<double>::quiet_NaN());
            for (uint32_t i = 0; i < count; ++i)
                reference_scene_latitude(rgb.data(), profile.p, n, i, expected.data() + 3 * i);
            if (!input || !parameters || !output ||
                !gpu->dispatch("spk_scene_latitude", {Arg::buf(input), Arg::buf(parameters),
                                   Arg::inline_bytes(n, 1), Arg::buf(output)},
                                   std::max(count, 1u), error) ||
                !check_values(gpu, output, expected, profile.name, error)) return false;
        }
    return true;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr, "usage: spk_vulkan_scene_latitude <resources>\n"); return 2; }
    std::string error;
    std::unique_ptr<Gpu> gpu(Gpu::create_vulkan(std::string(argv[1]) + "/vulkan", error));
    if (!gpu) { std::fprintf(stderr, "Vulkan device: %s\n", error.c_str()); return 1; }
    std::printf("Vulkan device: %s\n", gpu->device_name().c_str());
    if (!check_latitude(gpu.get(), error)) {
        std::fprintf(stderr, "scene_latitude: %s\n", error.c_str()); return 1;
    }
    std::printf("scene_latitude: %zu float64 cases, max abs error %.9g, "
                "max error/max(1,abs(reference)) %.9g, all guards intact\n",
                cases, maximum_error, maximum_scaled_error);
    return 0;
}
