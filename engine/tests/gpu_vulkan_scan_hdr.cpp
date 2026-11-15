// Independent headless check for the Windows scan_hdr port (2026-11-06,
// phase 2). The reference is nodes.metal's `spk_scan_hdr_expand` transcribed
// into C++ double, so the gate states the kernel's own numerics. The kernel
// is the whole scan_hdr GPU story: the table solve is entirely CPU double
// (core/scan_hdr.cpp), and this pass only reads each layer's density back
// from the develop's squeezed range -- below the layer's knee density dk it
// expands about dk by s, at and above dk it copies the density as it came
// (a pixel below every knee scans to the same bits as today's).
//
// Tolerance: the kernel is pointwise mul/sub only -- no transcendentals, no
// branch deeper than the sign of (v - dk) -- so the float32 chain sits a few
// ULP from the double reference. 4e-7 relative (with an absolute floor for
// the near-zero expansions) is the honest bound; a transcription mistake
// (swapped table slots, an inverted comparison) is orders of magnitude
// larger. NaN densities pass through unchanged and are asserted exactly:
// `v < dk` is false for NaN, and §11's rule (a pathological pixel passes as
// it came) holds by the comparison's own semantics.
//
// The dispatch bound is the pixel count (three channels a thread), the same
// shape the pipeline states (pipeline.cpp `node_scan_spectral`), and the
// count ladder pins the rounded-up final workgroup. The workgroup fold past
// maxComputeWorkGroupCount[0] is not exercised here: threads = pixels and
// three floats a pixel put a fold-crossing case past the storage-buffer
// range on the gate's devices (the same call the DI gate made; the fold is
// pinned by gpu_vulkan_mask.cpp's whole-frame grid instead).
#include "gpu/gpu.hpp"
#include "gpu_vulkan_read.hpp"

#include <algorithm>
#include <cmath>
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
    const double absolute_tolerance = 1e-7, relative_tolerance = 4e-7;
    for (size_t i = 0; i < expected.size(); ++i) {
        if (std::isnan(expected[i])) {
            // §11: a NaN density passes as it came -- exact NaN, not a 0 and
            // not an overflow artefact.
            if (!std::isnan(double(actual[i]))) {
                char detail[320];
                std::snprintf(detail, sizeof detail,
                    "%s case=%zu index=%zu got=%.9g expected=NaN", name, cases, i, double(actual[i]));
                error = detail;
                return false;
            }
            ++cases;
            continue;
        }
        const double delta = std::fabs(double(actual[i]) - expected[i]);
        const double tolerance = absolute_tolerance + relative_tolerance * std::fabs(expected[i]);
        if (!std::isfinite(double(actual[i])) || delta > tolerance) {
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

// nodes.metal:220 `spk_scan_hdr_expand`, transcribed: three channels a
// pixel, per channel `v < dk ? dk - (dk - v) * s : v` with dk = hdr[2c] and
// s = hdr[2c + 1]. The comparison is strict, so a density exactly at the
// knee's is copied, and NaN (never less than dk) is copied too.
double reference_expand(float v, float dk, float s) {
    return double(v) < double(dk) ? double(dk) - (double(dk) - double(v)) * double(s) : double(v);
}

void reference_expand_plane(const std::vector<float>& cmy, const float* hdr,
                            uint32_t pixels, std::vector<double>& expected) {
    expected.resize(size_t(pixels) * 3);
    for (uint32_t i = 0; i < pixels; ++i)
        for (uint32_t c = 0; c < 3; ++c)
            expected[size_t(i) * 3 + c] =
                reference_expand(cmy[size_t(i) * 3 + c], hdr[2 * c], hdr[2 * c + 1]);
}

// Densities spanning the develop's squeezed range and beyond it: 0 (the
// clear base), negatives (the lift's below-base expansions), the knee
// itself and the float next to it on both sides, the film's usable range,
// huge ends and NaN. The table's dk values are built so several of these
// land exactly on a knee or one ULP from it.
bool check_expand(Gpu* gpu, std::string& error) {
    struct Table { const char* name; float hdr[6]; };
    // The engine's own solved tables are dk in the film's density range with
    // s = (dk - D''_min) / (dk - D_min) >= 1 (core/scan_hdr.cpp
    // `scan_hdr_scan_table`); the profiles below cover that shape and the
    // arithmetic's corner shapes.
    const Table tables[] = {
        {"typical", {2.1f, 1.35f, 1.7f, 1.5f, 1.9f, 1.25f}},
        {"s 1", {1.8f, 1.0f, 1.6f, 1.0f, 2.0f, 1.0f}},
        {"s below 1", {1.4f, 0.5f, 1.2f, 0.75f, 1.6f, 0.6f}},
        {"s 0", {1.4f, 0.0f, 1.2f, 0.0f, 1.6f, 0.0f}},
        {"deep knees", {3.2f, 2.5f, 2.8f, 3.0f, 3.5f, 2.0f}},
        {"zero knee", {0.0f, 1.4f, 0.0f, 1.8f, 0.0f, 1.1f}},
        {"negative knee", {-0.5f, 1.2f, -0.25f, 1.6f, -0.75f, 1.1f}},
    };
    const uint32_t counts[] = {0, 1, 2, 255, 256, 257, 1025};
    // Exactly-on-knee and one-ULP neighbours, per table, parked into the
    // input at known indices: the strict `<` must copy at the knee and
    // expand one ULP below it.
    std::vector<float> values = {
        0.0f, -0.0f, -1e-10f, 1e-10f, 0.0625f, 0.18f, 0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f,
        -0.5f, -1.0f, -2.5f, 1e6f, std::numeric_limits<float>::quiet_NaN(),
    };
    for (const Table& table : tables) {
        for (int c = 0; c < 3; ++c) {
            const float dk = table.hdr[2 * c];
            values.push_back(dk);
            values.push_back(std::nextafter(dk, -std::numeric_limits<float>::infinity()));
            values.push_back(std::nextafter(dk, std::numeric_limits<float>::infinity()));
        }
    }
    for (const Table& table : tables)
        for (uint32_t count : counts) {
            const size_t floats = size_t(std::max(count, 1u)) * 3;
            std::vector<float> cmy(floats);
            for (size_t i = 0; i < floats; ++i) cmy[i] = values[i % values.size()];
            // The knee-exact probes overwrite the first live pixels' channels
            // in table order; count 0 and 1 keep the probes on live pixels
            // where the plane is large enough to carry them.
            std::vector<float> hdr(std::begin(table.hdr), std::end(table.hdr));
            std::vector<double> expected;
            reference_expand_plane(cmy, hdr.data(), count, expected);
            auto input = gpu->upload(cmy.data(), cmy.size() * sizeof(float), error);
            auto table_buffer = gpu->upload(hdr.data(), hdr.size() * sizeof(float), error);
            auto output = output_buffer(gpu, floats, error);
            if (!input || !table_buffer || !output) return false;
            const uint32_t n[] = {count};
            if (!gpu->dispatch("spk_scan_hdr_expand",
                               {Arg::buf(input), Arg::buf(table_buffer), Arg::inline_bytes(n, 1),
                                Arg::buf(output)},
                               std::max(count, 1u), error) ||
                !check_values(gpu, output, expected, table.name, error)) return false;
        }
    // The pipeline's alias case (elementwise independence lets the pass run
    // in place, as the pointwise family pins for spk_mul): a guarded tail on
    // the same plane.
    {
        const uint32_t count = 257;
        std::vector<float> cmy(size_t(count) * 3 + kGuardCount, kGuard);
        for (size_t i = 0; i < size_t(count) * 3; ++i) cmy[i] = values[i % values.size()];
        const float hdr[] = {1.7f, 1.4f, 2.0f, 1.1f, 1.5f, 1.6f};
        std::vector<double> expected;
        reference_expand_plane(cmy, hdr, count, expected);
        auto plane = gpu->upload(cmy.data(), cmy.size() * sizeof(float), error);
        auto table_buffer = gpu->upload(hdr, sizeof hdr, error);
        if (!plane || !table_buffer) return false;
        const uint32_t n[] = {count};
        if (!gpu->dispatch("spk_scan_hdr_expand",
                           {Arg::buf(plane), Arg::buf(table_buffer), Arg::inline_bytes(n, 1),
                            Arg::buf(plane)},
                           count, error) ||
            !check_values(gpu, plane, expected, "alias", error)) return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr, "usage: spk_vulkan_scan_hdr <resources>\n"); return 2; }
    std::string error;
    std::unique_ptr<Gpu> gpu(Gpu::create_vulkan(std::string(argv[1]) + "/vulkan", error));
    if (!gpu) { std::fprintf(stderr, "Vulkan device: %s\n", error.c_str()); return 1; }
    std::printf("Vulkan device: %s\n", gpu->device_name().c_str());
    if (!check_expand(gpu.get(), error)) {
        std::fprintf(stderr, "scan_hdr: %s\n", error.c_str()); return 1;
    }
    std::printf("scan_hdr: %zu analytic/float64 cases, max abs error %.9g, "
                "max error/max(1,abs(reference)) %.9g, all guards intact\n",
                cases, maximum_error, maximum_scaled_error);
    return 0;
}
