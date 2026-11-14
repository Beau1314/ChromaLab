// Independent headless check for the Windows Cineon Digital Intermediate
// port. The reference is nodes.metal's `spk_di_encode` transcribed into C++
// double, so the gate states the kernel's own numerics, not a second GPU
// implementation. Guard values exercise the rounded-up final workgroup, not
// just the visible image plane.
//
// Tolerance: the kernel is pointwise but carries `pow`/`log10`/`cos`/`atan2`
// (PORT_PLAN §0's looser class), so the bound is 1e-6 relative -- the output
// is clamped Cineon code in [0, 1], which gives the absolute floor meaning.
#include "gpu/gpu.hpp"
#include "gpu_vulkan_read.hpp"

#include <algorithm>
#include <cmath>
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

// Metal's fmod keeps the dividend's sign (C semantics); the hue wrap must
// not silently become GLSL mod's floor semantics.
double metal_fmod(double x, double y) { return x - y * std::trunc(x / y); }

// nodes.metal `spk_di_encode`, in double. p is the documented 42-float row:
// [0..8] film RGB -> working RGB (x @ M), [9..11] live log10 offsets,
// [12..20] working RGB -> Oklab LMS, [21..29] back, [30] compensation on,
// [31] hue rotation (deg), [32] chroma scale, [33..36] hue centre, half
// width, chroma lo/hi, [37..39] Cineon white, codes per decade, max code,
// [40] the positive of clear base, [41] Cineon's black offset.
void reference_di_encode(const float* y, const float* p, double* out) {
    double pos[3];
    for (int c = 0; c < 3; ++c) pos[c] = std::pow(10.0, double(y[c]) + double(p[9 + c]));
    // The base is black: the same floor on every channel, scaled with the
    // live gain so brightness and the filters never lift it.
    for (int c = 0; c < 3; ++c) pos[c] -= double(p[40]) * std::pow(10.0, double(p[9 + c]));
    double lin[3] = {pos[0] * p[0] + pos[1] * p[3] + pos[2] * p[6],
                     pos[0] * p[1] + pos[1] * p[4] + pos[2] * p[7],
                     pos[0] * p[2] + pos[1] * p[5] + pos[2] * p[8]};
    if (p[30] != 0.0f) {
        // Oklab (Ottosson 2020); the window sits on the output hue and fades
        // out below a chroma floor, so greys and every other hue are exact.
        double lms[3] = {p[12] * lin[0] + p[13] * lin[1] + p[14] * lin[2],
                         p[15] * lin[0] + p[16] * lin[1] + p[17] * lin[2],
                         p[18] * lin[0] + p[19] * lin[1] + p[20] * lin[2]};
        for (double& v : lms) v = std::pow(std::max(v, 0.0), 1.0 / 3.0);
        const double L = 0.2104542553 * lms[0] + 0.7936177850 * lms[1] - 0.0040720468 * lms[2];
        double a = 1.9779984951 * lms[0] - 2.4285922050 * lms[1] + 0.4505937099 * lms[2];
        double b = 0.0259040371 * lms[0] + 0.7827717662 * lms[1] - 0.8086757660 * lms[2];
        const double C = std::sqrt(a * a + b * b);
        const double h = std::atan2(b, a) * 57.29577951308232;
        const double d = metal_fmod(h - p[33] + 540.0, 360.0) - 180.0;
        double w = std::fabs(d) < p[34] ? 0.5 * (1.0 + std::cos(3.14159265358979 * d / p[34])) : 0.0;
        double g = std::clamp((C - p[35]) / (p[36] - p[35]), 0.0, 1.0);
        w *= g * g * (3.0 - 2.0 * g);
        if (w > 0.0) {
            const double h2 = (h + w * p[31]) * 0.017453292519943295;
            const double C2 = C * (1.0 + w * (p[32] - 1.0));
            a = C2 * std::cos(h2);
            b = C2 * std::sin(h2);
            double l_[3] = {L + 0.3963377774 * a + 0.2158037573 * b,
                            L - 0.1055613458 * a - 0.0638541728 * b,
                            L - 0.0894841775 * a - 1.2914855480 * b};
            for (double& v : l_) v = v * v * v;
            lin[0] = p[21] * l_[0] + p[22] * l_[1] + p[23] * l_[2];
            lin[1] = p[24] * l_[0] + p[25] * l_[1] + p[26] * l_[2];
            lin[2] = p[27] * l_[0] + p[28] * l_[1] + p[29] * l_[2];
        }
    }
    // Kodak's Cineon encode with the black offset (`log_encoding_Cineon`).
    for (int c = 0; c < 3; ++c) {
        const double code =
            (p[37] + p[38] * std::log10(std::max(lin[c], 0.0) * (1.0 - p[41]) + p[41])) / p[39];
        out[c] = std::clamp(code, 0.0, 1.0);
    }
}

struct Profile {
    const char* name;
    float p[42];
};

// Kodak's Cineon convention (core/digital_intermediate.hpp): reference white
// 685, 300 codes per decade, max code 1023, black offset 10^((95-685)/300).
std::vector<Profile> profiles() {
    const float black_offset = float(std::pow(10.0, (95.0 - 685.0) / 300.0));
    const float cineon[5] = {685.0f, 300.0f, 1023.0f, 0.004f, black_offset};
    // A mild film->working cross-talk, as the fitted matrices have.
    const float matrix[9] = {1.05f, -0.02f, -0.03f, -0.04f, 0.98f, 0.06f, 0.01f, 0.05f, 1.02f};
    const float identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    auto fill = [=](float* p) {
        for (int i = 0; i < 9; ++i) p[i] = matrix[i];
        for (int i = 0; i < 9; ++i) p[12 + i] = identity[i];
        for (int i = 0; i < 9; ++i) p[21 + i] = identity[i];
        for (int i = 0; i < 5; ++i) p[37 + i] = cineon[i];
    };
    std::vector<Profile> out;
    // Compensation off: the pure matrix + Cineon path.
    Profile a{};
    a.name = "comp off";
    fill(a.p);
    a.p[9] = 0.02f; a.p[10] = -0.015f; a.p[11] = 0.01f;
    out.push_back(a);
    // Compensation on with the engine's own blue window: identity Oklab
    // bases (the reference is free to choose any consistent pair), a hue
    // rotation and a chroma lift that the spread of inputs must exercise.
    Profile b{};
    b.name = "blue window";
    fill(b.p);
    b.p[9] = 0.02f; b.p[10] = -0.015f; b.p[11] = 0.01f;
    b.p[30] = 1.0f;
    b.p[31] = -14.0f; b.p[32] = 1.12f;
    b.p[33] = 280.0f; b.p[34] = 55.0f; b.p[35] = 0.02f; b.p[36] = 0.08f;
    out.push_back(b);
    // Saturation: large offsets and a deep floor push codes against both
    // clamps; the half-width spans the whole hue circle, so the window is on
    // wherever chroma clears its floor.
    Profile c{};
    c.name = "saturated";
    fill(c.p);
    c.p[9] = 0.35f; c.p[10] = -0.3f; c.p[11] = 0.4f;
    c.p[30] = 1.0f;
    c.p[31] = 40.0f; c.p[32] = 0.6f;
    c.p[33] = 280.0f; c.p[34] = 180.0f; c.p[35] = 0.02f; c.p[36] = 0.08f;
    c.p[40] = 0.02f;
    out.push_back(c);
    return out;
}

// Printing-density log10 values spanning the DI's usable range, including 0,
// negatives below the base, the Cineon anchor densities and saturating ends.
bool check_encode(Gpu* gpu, std::string& error) {
    const uint32_t counts[] = {0, 1, 2, 255, 256, 257, 1025};
    const float densities[] = {-3.0f, -2.0f, -1.0f, -0.5f, -0.25f, -0.125f,
                               -0.0625f, 0.0f, 0.0625f, 0.125f, 0.25f, 0.5f,
                               0.30103f, 1.0f, 2.0f, 3.0f};
    constexpr size_t density_count = sizeof densities / sizeof densities[0];
    for (const Profile& profile : profiles())
        for (uint32_t count : counts) {
            std::vector<float> y(std::max(count, 1u) * 3);
            for (uint32_t i = 0; i < std::max(count, 1u); ++i) {
                // Every fifth pixel is exact grey (all channels equal), so
                // the compensation's chroma floor sees both branches.
                const bool grey = i % 5 == 0;
                const float r = densities[i % density_count];
                const float g = densities[(i + 5) % density_count];
                const float b = densities[(i + 9) % density_count];
                y[3 * i] = r;
                y[3 * i + 1] = grey ? r : g;
                y[3 * i + 2] = grey ? r : b;
            }
            auto input = gpu->upload(y.data(), y.size() * sizeof(float), error);
            auto parameters = gpu->upload(profile.p, sizeof profile.p, error);
            auto output = output_buffer(gpu, size_t(count) * 3, error);
            const uint32_t n[] = {count};
            std::vector<double> expected(size_t(count) * 3);
            for (uint32_t i = 0; i < count; ++i)
                reference_di_encode(y.data() + 3 * i, profile.p, expected.data() + 3 * i);
            if (!input || !parameters || !output ||
                !gpu->dispatch("spk_di_encode", {Arg::buf(input), Arg::buf(parameters),
                                   Arg::inline_bytes(n, 1), Arg::buf(output)},
                                   std::max(count, 1u), error) ||
                !check_values(gpu, output, expected, profile.name, error)) return false;
        }
    return true;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr, "usage: spk_vulkan_di_encode <resources>\n"); return 2; }
    std::string error;
    std::unique_ptr<Gpu> gpu(Gpu::create_vulkan(std::string(argv[1]) + "/vulkan", error));
    if (!gpu) { std::fprintf(stderr, "Vulkan device: %s\n", error.c_str()); return 1; }
    std::printf("Vulkan device: %s\n", gpu->device_name().c_str());
    if (!check_encode(gpu.get(), error)) {
        std::fprintf(stderr, "di_encode: %s\n", error.c_str()); return 1;
    }
    std::printf("di_encode: %zu float64 cases, max abs error %.9g, "
                "max error/max(1,abs(reference)) %.9g, all guards intact\n",
                cases, maximum_error, maximum_scaled_error);
    return 0;
}
