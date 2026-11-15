// Independent headless check for the Windows overscan port (RFC-032
// §26-§27). The references are overscan.metal's four kernels -- :144
// `spk_overscan_canvas` (with its inline helpers `canvas_to_film`, `hash_u`,
// `hash_f`, `value_noise`, `sd_quad`, `gate_sdf`, `penumbra` and
// `sample_frame`), :208 `spk_overscan_add_mask`, :305
// `spk_overscan_film_present` (with `perf_sdf`, `erf_as` and `film_alpha`)
// and :331 `spk_overscan_light` -- transcribed into C++ double, so the gate
// states the kernels' own numerics, not a second GPU implementation. Guard
// values exercise the rounded-up final workgroup, not just the logical
// output count.
//
// Tolerance: the kernels carry sin/cos/exp/asin/sqrt through the geometry
// and the scan-of-the-holes model (PORT_PLAN §0's looser class), so the
// bound is 1e-6 relative with a 1e-6 absolute floor. The parameter block is
// built as float32 exactly as the host uploads it, and the frame/coverage
// inputs are float32, so the references read the same values the device
// does; only the transcription's own arithmetic is double.
//
// The canvas cases cover: a small canvas (24x16), a perforated horizontal
// film with wobble, roughness, fog, leaks, flare, a cutting quad, an
// added triangle (a repeated vertex) and a cut ellipse (shouldered-gate
// shapes), the pair advance (a half-frame pair), both carrier modes, and a
// count ladder whose last case crosses the dispatch fold (> 65535 threads).
#include "gpu/gpu.hpp"
#include "gpu_vulkan_read.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
                  const char* name, bool canvas, std::string& error) {
    if (!gpu->flush(error)) return false;
    std::vector<float> actual_readback;
    if (!read_test_buffer(gpu, output, actual_readback, error)) return false;
    const float* actual = actual_readback.data();
    // The canvas kernel accumulates five exposures per pixel (the gate's
    // sample, the flare, the fog, the leaks), and two of its stages amplify
    // the gate SDF's own float32 noise: the penumbra's lit fraction rises at
    // up to 1/(2*r) per mm of signed distance (r ~ 0.03, so ~16x), and the
    // flare's and fog's exp(-d / sigma) carry 1/sigma (~20x at the flare's
    // width). The device's rounding of that composition cannot be reproduced
    // by the transcription's double, so the canvas cases state their own
    // class -- 1e-4 relative and absolute. film_present and light are
    // shallower but still run an erf and exps over the same noisy SDF (the
    // punched edge's roughness and burrs), so they take 5e-6 rather than the
    // scene kernels' 1e-6. What the canvas gate pins is the arithmetic as
    // written; the pixels themselves are overscan_checks.py's.
    const double absolute_tolerance = canvas ? 1e-4 : 5e-6;
    const double relative_tolerance = absolute_tolerance;
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

// --- the OsP parameter block, exactly as overscan.cpp :41 lays it out --------
enum OsP : int {
    P_CW = 0, P_CH, P_PX, P_FW, P_FH,
    P_A00, P_A01, P_A10, P_A11, P_CS, P_CT, P_CU, P_CV,
    P_VERTICAL,
    P_GS0, P_GT0, P_GW, P_GH, P_CORNER, P_PENUMBRA,
    P_WOB_A = 20, P_WOB_PH = 32,
    P_FILM_W = 44, P_FOG_AMP, P_FOG_WIDTH, P_FOG_SEED, P_FOG_PERIOD,
    P_FOG_R, P_FOG_G, P_FOG_B,
    P_N_LEAKS = 52,
    P_PERFORATED = 83, P_PERF_PITCH, P_PERF_W, P_PERF_H, P_PERF_EDGE, P_PERF_R, P_PERF_PHASE,
    P_HOLE_C = 90, P_HOLE_M, P_HOLE_Y,
    P_FLARE_AMP = 93, P_FLARE_WIDTH,
    P_GATE_R = 95, P_N_Q = 99, P_Q = 100,
    P_ROUGH_AMP = 180, P_ROUGH_PERIOD, P_ROUGH_SEED,
    P_HOLES_LIGHT = 183, P_LIGHT_R, P_LIGHT_G, P_LIGHT_B,
    P_PERF_SEED = 187, P_LIGHT_FALL, P_LIGHT_DIR,
    P_WALL_T = 190, P_WALL_VS, P_WALL_VT, P_WALL_PAR, P_BASE_R, P_BASE_G, P_BASE_B, P_WALL_GLOW, P_WALL_SCATTER,
    P_CARRIER = 199,
    P_LEAKS = 200,
    P_PAIR_ADV = 260,
    P_COUNT = 261
};
constexpr int kMaxLeaks = 12;
constexpr int kMaxQuads = 8;

// A small deterministic fill so the cases are reproducible without a stdlib
// dependency on the order of a host rng.
float pattern(uint32_t i, uint32_t salt) {
    uint32_t x = i * 0x9E3779B9u ^ salt;
    x ^= x >> 16u; x *= 0x7feb352du; x ^= x >> 15u; x *= 0x846ca68bu; x ^= x >> 16u;
    return float(double(x >> 8) * (1.0 / 16777216.0));   // [0, 1)
}

struct Block {
    std::vector<float> v{std::vector<float>(size_t(P_COUNT), 0.0f)};
    float& operator[](int i) { return v[size_t(i)]; }
    const float& operator[](int i) const { return v[size_t(i)]; }
};

struct Frame {
    uint32_t w, h;
    std::vector<float> rgb;
};

Frame frame_fixture(uint32_t w, uint32_t h, uint32_t salt) {
    Frame f{w, h, std::vector<float>(size_t(w) * h * 3)};
    for (uint32_t i = 0; i < w * h; ++i)
        for (int c = 0; c < 3; ++c)
            f.rgb[size_t(i) * 3 + size_t(c)] = 0.02f + pattern(i, salt + uint32_t(c)) * 1.8f;
    return f;
}

struct Case {
    const char* name;
    Block p;
    Frame frame;
};

// A perforated horizontal film (a 135-like strip): wobble on every side, gate
// burrs (an added triangle via a repeated vertex), a cut with rounding, a cut
// ellipse (the eared gate's sweep), machined-edge roughness, fog, three
// leaks, flare, and the penumbra's soft shadow.
Case horizontal(const char* name, float carrier, float holes_light, float penumbra) {
    Block b;
    b[P_CW] = 48; b[P_CH] = 36; b[P_PX] = 0.75f; b[P_FW] = 24; b[P_FH] = 18;
    const double rot = 0.031;
    b[P_A00] = float(std::cos(rot)); b[P_A01] = float(-std::sin(rot));
    b[P_A10] = float(std::sin(rot)); b[P_A11] = float(std::cos(rot));
    b[P_CS] = 17.1f; b[P_CT] = 12.3f; b[P_CU] = 18.0f; b[P_CV] = 13.5f;
    b[P_VERTICAL] = 0.0f;
    b[P_GS0] = 0.6f; b[P_GT0] = 4.9f; b[P_GW] = 27.5f; b[P_GH] = 19.6f;
    b[P_PENUMBRA] = penumbra;
    for (int side = 0; side < 4; ++side)
        for (int k = 0; k < 3; ++k) {
            b[P_WOB_A + side * 3 + k] = float(0.004 + 0.0025 * double(k) + 0.003 * double(side));
            b[P_WOB_PH + side * 3 + k] = float(pattern(uint32_t(side * 3 + k), 71u) * 6.2831853);
        }
    b[P_FILM_W] = 35.0f;
    b[P_FOG_AMP] = 0.018f; b[P_FOG_WIDTH] = 1.3f; b[P_FOG_SEED] = 911.0f; b[P_FOG_PERIOD] = 7.4f;
    b[P_FOG_R] = 0.9f; b[P_FOG_G] = 0.8f; b[P_FOG_B] = 0.7f;
    b[P_N_LEAKS] = 3.0f;
    for (int k = 0; k < 3; ++k) {
        const int s = P_LEAKS + 5 * k;
        b[s] = float(6.0 + 9.0 * double(k)); b[s + 1] = float(k % 2);
        b[s + 2] = 0.05f * float(1 + k); b[s + 3] = float(1.5 + 0.7 * double(k)); b[s + 4] = 0.8f;
    }
    b[P_PERFORATED] = 1.0f;
    b[P_PERF_PITCH] = 4.75f; b[P_PERF_W] = 1.98f; b[P_PERF_H] = 2.80f;
    b[P_PERF_EDGE] = 2.00f; b[P_PERF_R] = 0.50f; b[P_PERF_PHASE] = -3.2f;
    b[P_PERF_SEED] = 4177.0f;
    b[P_HOLE_C] = 0.62f; b[P_HOLE_M] = 0.58f; b[P_HOLE_Y] = 0.55f;
    b[P_FLARE_AMP] = 0.2f; b[P_FLARE_WIDTH] = 0.05f;
    b[P_GATE_R + 0] = 0.12f; b[P_GATE_R + 1] = 0.3f; b[P_GATE_R + 2] = 0.22f; b[P_GATE_R + 3] = 0.08f;
    // three quads: an added triangle (a repeated vertex), a rounded cut, and
    // a cut ellipse (an eared side's sweep) -- the gate_sdf shapes the Metal
    // comments name.
    b[P_N_Q] = 3.0f;
    {
        float* q = b.v.data() + P_Q;
        // triangle: (a0,b0) (a1,b1) apex, base repeated
        const float qs[10] = {8.0f, 4.4f, 10.0f, 4.4f, 9.0f, 3.9f, 8.0f, 4.4f, 0.005f, 1.0f};
        std::memcpy(q, qs, sizeof qs);
        const float qs2[10] = {20.0f, 3.6f, 26.0f, 3.6f, 26.6f, 4.9f, 20.6f, 4.9f, 0.02f, -1.0f};
        std::memcpy(q + 10, qs2, sizeof qs2);
        const float qs3[10] = {16.0f, 4.2f, 0.9f, 1.7f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, -2.0f};
        std::memcpy(q + 20, qs3, sizeof qs3);
    }
    b[P_ROUGH_AMP] = 0.006f; b[P_ROUGH_PERIOD] = 0.12f; b[P_ROUGH_SEED] = 77.0f;
    b[P_HOLES_LIGHT] = holes_light;
    b[P_LIGHT_R] = 1.0f; b[P_LIGHT_G] = 1.0f; b[P_LIGHT_B] = 1.0f;
    b[P_WALL_T] = 0.125f; b[P_WALL_VS] = 0.62f; b[P_WALL_VT] = -0.5f; b[P_WALL_PAR] = 0.0f;
    b[P_BASE_R] = 0.95f; b[P_BASE_G] = 0.7f; b[P_BASE_B] = 0.45f;
    b[P_WALL_GLOW] = 0.04f; b[P_WALL_SCATTER] = 0.03f;
    b[P_CARRIER] = carrier;
    b[P_PAIR_ADV] = 0.0f;
    return Case{name, b, frame_fixture(24, 18, 5u)};
}

// A 120 strip held with the film running down the picture (the layout's
// quarter turn), no perforations, no roughness, a pair advance (the same gate
// exposed again), fog off, a black carrier, and a soft penumbra.
Case vertical_pair(const char* name) {
    Block b;
    b[P_CW] = 30; b[P_CH] = 52; b[P_PX] = 1.1f; b[P_FW] = 20; b[P_FH] = 15;
    const double rot = -0.17;
    const double c = std::cos(rot), s = std::sin(rot);
    // the layout's vertical quarter turn: A = [sn, c, -c, sn]
    b[P_A00] = float(s); b[P_A01] = float(c); b[P_A10] = float(-c); b[P_A11] = float(s);
    b[P_CS] = 16.2f; b[P_CT] = 29.4f; b[P_CU] = 16.5f; b[P_CV] = 28.6f;
    b[P_VERTICAL] = 1.0f;
    b[P_GS0] = 0.0f; b[P_GT0] = 2.7f; b[P_GW] = 22.4f; b[P_GH] = 55.4f;
    b[P_PENUMBRA] = 0.045f;
    for (int side = 0; side < 4; ++side)
        for (int k = 0; k < 3; ++k) {
            b[P_WOB_A + side * 3 + k] = 0.010f;
            b[P_WOB_PH + side * 3 + k] = float(pattern(uint32_t(side * 3 + k), 21u) * 6.2831853);
        }
    b[P_FILM_W] = 61.0f;
    b[P_PERFORATED] = 0.0f;
    b[P_HOLE_C] = 0.5f; b[P_HOLE_M] = 0.5f; b[P_HOLE_Y] = 0.5f;
    b[P_GATE_R + 0] = 0.35f; b[P_GATE_R + 1] = 0.35f; b[P_GATE_R + 2] = 0.35f; b[P_GATE_R + 3] = 0.35f;
    // a shouldered gate's stepped cut, its two bands
    b[P_N_Q] = 2.0f;
    {
        float* q = b.v.data() + P_Q;
        const float qs[10] = {-1.0f, 2.0f, 8.5f, 2.0f, 8.2f, 3.1f, -0.7f, 3.1f, 0.03f, -1.0f};
        std::memcpy(q, qs, sizeof qs);
        const float qs2[10] = {-0.7f, 3.05f, 8.2f, 3.05f, 7.9f, 3.9f, -0.4f, 3.9f, 0.02f, -1.0f};
        std::memcpy(q + 10, qs2, sizeof qs2);
    }
    b[P_HOLES_LIGHT] = 0.0f;
    b[P_LIGHT_R] = 1.0f; b[P_LIGHT_G] = 0.95f; b[P_LIGHT_B] = 0.9f;
    b[P_WALL_T] = 0.125f; b[P_WALL_VS] = -0.3f; b[P_WALL_VT] = 0.7f; b[P_WALL_PAR] = 0.0f;
    b[P_BASE_R] = 0.9f; b[P_BASE_G] = 0.75f; b[P_BASE_B] = 0.5f;
    b[P_WALL_GLOW] = 0.02f; b[P_WALL_SCATTER] = 0.025f;
    b[P_CARRIER] = 0.0f;
    b[P_PAIR_ADV] = 19.0f;
    return Case{name, b, frame_fixture(20, 15, 9u)};
}

// The dispatch fold: more canvas pixels than one row of workgroups may hold.
Case fold_case(const char* name, float perforated) {
    Block b;
    b[P_CW] = 320; b[P_CH] = 220; b[P_PX] = 0.35f; b[P_FW] = 32; b[P_FH] = 24;
    b[P_A00] = 1.0f; b[P_A01] = 0.0f; b[P_A10] = 0.0f; b[P_A11] = 1.0f;
    b[P_CS] = 56.0f; b[P_CT] = 38.6f; b[P_CU] = 56.0f; b[P_CV] = 38.5f;
    b[P_VERTICAL] = 0.0f;
    b[P_GS0] = 0.2f; b[P_GT0] = 3.0f; b[P_GW] = 111.6f; b[P_GH] = 55.2f;
    b[P_PENUMBRA] = 0.03f;
    b[P_FILM_W] = 61.0f;
    b[P_PERFORATED] = perforated;
    b[P_PERF_PITCH] = 4.75f; b[P_PERF_W] = 1.98f; b[P_PERF_H] = 2.80f;
    b[P_PERF_EDGE] = 2.00f; b[P_PERF_R] = 0.50f; b[P_PERF_PHASE] = -3.0f;
    b[P_PERF_SEED] = 8123.0f;
    b[P_HOLE_C] = 0.6f; b[P_HOLE_M] = 0.56f; b[P_HOLE_Y] = 0.52f;
    b[P_HOLES_LIGHT] = 1.0f;
    b[P_LIGHT_R] = 1.0f; b[P_LIGHT_G] = 1.0f; b[P_LIGHT_B] = 1.0f;
    b[P_WALL_T] = 0.125f; b[P_WALL_VS] = 0.4f; b[P_WALL_VT] = 0.4f; b[P_WALL_PAR] = 0.0f;
    b[P_BASE_R] = 0.93f; b[P_BASE_G] = 0.72f; b[P_BASE_B] = 0.48f;
    b[P_WALL_GLOW] = 0.05f; b[P_WALL_SCATTER] = 0.03f;
    b[P_CARRIER] = 2.0f;
    return Case{name, b, frame_fixture(32, 24, 13u)};
}

// --- the canvas kernel, in double --------------------------------------------

struct D2 { double x, y; };

D2 canvas_to_film(double x, double y, const Block& p) {
    const double u = (x + 0.5) * double(p[P_PX]) - double(p[P_CU]);
    const double v = (y + 0.5) * double(p[P_PX]) - double(p[P_CV]);
    return {double(p[P_A00]) * u + double(p[P_A01]) * v + double(p[P_CS]),
            double(p[P_A10]) * u + double(p[P_A11]) * v + double(p[P_CT])};
}

uint32_t hash_u(uint32_t x) {
    x ^= x >> 16u; x *= 0x7feb352du; x ^= x >> 15u; x *= 0x846ca68bu; x ^= x >> 16u;
    return x;
}
double hash_f(int i, uint32_t seed) {
    return double(hash_u(uint32_t(i) * 0x9E3779B9u ^ seed)) * (1.0 / 4294967295.0);
}
double value_noise(double x, uint32_t seed) {
    const double i0 = std::floor(x), f = x - i0;
    const double w = f * f * (3.0 - 2.0 * f);
    return hash_f(int(i0), seed) * (1.0 - w) + hash_f(int(i0) + 1, seed) * w;
}

// `mix(a, b, w)` as the kernels mean it.
double mixd(double a, double b, double w) { return a * (1.0 - w) + b * w; }

double sd_quad(D2 st, const float* q) {
    D2 v[4] = {{q[0], q[1]}, {q[2], q[3]}, {q[4], q[5]}, {q[6], q[7]}};
    double d = (st.x - v[0].x) * (st.x - v[0].x) + (st.y - v[0].y) * (st.y - v[0].y);
    double sgn = 1.0;
    for (uint32_t i = 0, j = 3; i < 4; j = i, ++i) {
        const double ex = v[j].x - v[i].x, ey = v[j].y - v[i].y;
        const double wx = st.x - v[i].x, wy = st.y - v[i].y;
        const double ee = ex * ex + ey * ey;
        if (ee < 1e-12) continue;
        const double t = std::clamp((wx * ex + wy * ey) / ee, 0.0, 1.0);
        const double bx = wx - ex * t, by = wy - ey * t;
        d = std::min(d, bx * bx + by * by);
        const bool c0 = st.y >= v[i].y, c1 = st.y < v[j].y, c2 = ex * wy > ey * wx;
        if ((c0 && c1 && c2) || (!c0 && !c1 && !c2)) sgn = -sgn;
    }
    return sgn * std::sqrt(d);
}

double gate_sdf(D2 st, const Block& p) {
    const double hw = 0.5 * double(p[P_GW]), hh = 0.5 * double(p[P_GH]);
    const double ds = st.x - (double(p[P_GS0]) + hw), dt = st.y - (double(p[P_GT0]) + hh);
    const double r = ds > 0.0 ? (dt > 0.0 ? double(p[P_GATE_R + 2]) : double(p[P_GATE_R + 1]))
                              : (dt > 0.0 ? double(p[P_GATE_R + 3]) : double(p[P_GATE_R]));
    const double qs = std::fabs(ds) - (hw - r), qt = std::fabs(dt) - (hh - r);
    const double qx = std::max(qs, 0.0), qy = std::max(qt, 0.0);
    double d = std::sqrt(qx * qx + qy * qy) + std::min(std::max(qs, qt), 0.0) - r;
    uint32_t side; double along, len;
    if (qs > qt) { side = ds > 0.0 ? 1u : 3u; along = dt + hh; len = 2.0 * hh; }
    else         { side = dt > 0.0 ? 2u : 0u; along = ds + hw; len = 2.0 * hw; }
    const double u = std::clamp(along / len, 0.0, 1.0);
    double w = 0.0;
    for (uint32_t k = 0; k < 3u; ++k)
        w += double(p[int(P_WOB_A + side * 3u + int(k))]) *
             std::sin(double(k + 1u) * 3.14159265358979 * u + double(p[int(P_WOB_PH + side * 3u + int(k))]));
    d -= w * std::sin(3.14159265358979 * u);
    const uint32_t nq = uint32_t(double(p[P_N_Q]));
    for (uint32_t k = 0; k < nq; ++k) {
        const float* q = p.v.data() + P_Q + 10u * k;
        double dq;
        if (std::fabs(q[9]) > 1.5) {
            const double rx = std::max(double(q[2]), 1e-4), ry = std::max(double(q[3]), 1e-4);
            dq = (std::sqrt((st.x - double(q[0])) * (st.x - double(q[0])) / (rx * rx) +
                            (st.y - double(q[1])) * (st.y - double(q[1])) / (ry * ry)) - 1.0) * std::min(rx, ry);
        } else {
            dq = sd_quad(st, q) - double(q[8]);
        }
        d = q[9] > 0.0 ? std::min(d, dq) : std::max(d, -dq);
    }
    if (p[P_ROUGH_AMP] > 0.0f) {
        const uint32_t seed = uint32_t(double(p[P_ROUGH_SEED]));
        const double per = std::max(double(p[P_ROUGH_PERIOD]), 1e-3);
        d += double(p[P_ROUGH_AMP]) * (value_noise(st.x / per, seed) +
             value_noise(st.y / per, seed ^ 0x5bd1e995u) - 1.0);
    }
    return d;
}

double penumbra(double d, double r) {
    if (r <= 1e-6) return d < 0.0 ? 1.0 : 0.0;
    const double u = std::clamp(-d / r, -1.0, 1.0);
    return 0.5 + (u * std::sqrt(std::max(1.0 - u * u, 0.0)) + std::asin(u)) * (1.0 / 3.14159265358979);
}

// The transcription of `sample_frame`: bilinear per component, as the kernel
// writes it.
void sample_frame3(const Frame& f, double fx, double fy, double out[3]) {
    fx = std::clamp(fx, 0.0, double(f.w) - 1.0);
    fy = std::clamp(fy, 0.0, double(f.h) - 1.0);
    const uint32_t x0 = uint32_t(std::floor(fx)), y0 = uint32_t(std::floor(fy));
    const uint32_t x1 = std::min(x0 + 1u, f.w - 1u), y1 = std::min(y0 + 1u, f.h - 1u);
    const double ax = fx - double(x0), ay = fy - double(y0);
    auto at = [&](uint32_t x, uint32_t y, int c) {
        return double(f.rgb[(size_t(y) * f.w + x) * 3 + size_t(c)]);
    };
    for (int c = 0; c < 3; ++c) {
        const double a = at(x0, y0, c), b = at(x1, y0, c), cc = at(x0, y1, c), d = at(x1, y1, c);
        out[c] = (a + (b - a) * ax) * (1.0 - ay) + (cc + (d - cc) * ax) * ay;
    }
}

void reference_canvas(const Case& cs, std::vector<double>& out) {
    const Block& p = cs.p;
    const uint32_t cw = uint32_t(double(p[P_CW])), ch = uint32_t(double(p[P_CH]));
    const uint32_t n = cw * ch;
    out.assign(size_t(n) * 3, 0.0);
    for (uint32_t i = 0; i < n; ++i) {
        const D2 st = canvas_to_film(double(i % cw), double(i / cw), p);
        const double W = double(p[P_FILM_W]);
        double e[3] = {0.0, 0.0, 0.0};
        if (p[P_CARRIER] > 1.5f || (st.y >= 0.0 && st.y <= W)) {
            D2 sg = st;
            double shift = 0.0;
            if (p[P_PAIR_ADV] > 0.0f &&
                st.x > double(p[P_GS0]) + 0.5 * (double(p[P_GW]) + double(p[P_PAIR_ADV]))) {
                shift = double(p[P_PAIR_ADV]);
                sg.x -= shift;
            }
            const double cov = penumbra(gate_sdf(sg, p), double(p[P_PENUMBRA]));
            if (cov > 0.0) {
                const double px = double(p[P_PX]);
                double fx, fy;
                if (p[P_VERTICAL] < 0.5f) {
                    fx = (st.x - double(p[P_GS0])) / px - 0.5;
                    fy = (st.y - double(p[P_GT0])) / px - 0.5;
                } else {
                    fx = (double(p[P_GT0]) + double(p[P_GH]) - st.y) / px - 0.5;
                    fy = (st.x - double(p[P_GS0])) / px - 0.5;
                }
                double s[3];
                sample_frame3(cs.frame, fx, fy, s);
                for (int c = 0; c < 3; ++c) e[c] += cov * s[c];
            }
            const double d_gate = gate_sdf(sg, p);
            if (p[P_FLARE_AMP] > 0.0f && d_gate > -0.02 && d_gate < 6.0 * double(p[P_FLARE_WIDTH])) {
                const double px = double(p[P_PX]);
                const double in_s = std::clamp(sg.x, double(p[P_GS0]) + 0.12,
                                               double(p[P_GS0]) + double(p[P_GW]) - 0.12) + shift;
                const double in_t = std::clamp(st.y, double(p[P_GT0]) + 0.12,
                                               double(p[P_GT0]) + double(p[P_GH]) - 0.12);
                double fx, fy;
                if (p[P_VERTICAL] < 0.5f) {
                    fx = (in_s - double(p[P_GS0])) / px - 0.5;
                    fy = (in_t - double(p[P_GT0])) / px - 0.5;
                } else {
                    fx = (double(p[P_GT0]) + double(p[P_GH]) - in_t) / px - 0.5;
                    fy = (in_s - double(p[P_GS0])) / px - 0.5;
                }
                double edge_light[3];
                sample_frame3(cs.frame, fx, fy, edge_light);
                const double prof = std::exp(-std::max(d_gate, 0.0) / double(p[P_FLARE_WIDTH]));
                for (int c = 0; c < 3; ++c) e[c] += double(p[P_FLARE_AMP]) * prof * edge_light[c];
            }
            const double d_top = std::max(st.y, 0.0), d_bot = std::max(W - st.y, 0.0);
            const uint32_t seed = uint32_t(double(p[P_FOG_SEED]));
            const double period = std::max(double(p[P_FOG_PERIOD]), 0.1);
            const double nz = 0.6 * value_noise(st.x / period, seed) +
                              0.4 * value_noise(st.x / (0.37 * period), seed ^ 0xA5A5A5A5u);
            double fog = double(p[P_FOG_AMP]) *
                std::exp(-std::min(d_top, d_bot) / std::max(double(p[P_FOG_WIDTH]), 1e-3)) * (0.3 + 0.7 * nz);
            const uint32_t nl = uint32_t(double(p[P_N_LEAKS]));
            for (uint32_t k = 0; k < nl; ++k) {
                const int b = P_LEAKS + 5 * int(k);
                const double dd = p[b + 1] < 0.5f ? d_top : d_bot;
                const double ds = st.x - double(p[b]);
                fog += double(p[b + 2]) * std::exp(-ds * ds / (2.0 * double(p[b + 3]) * double(p[b + 3]))) *
                       std::exp(-dd / double(p[b + 4]));
            }
            for (int c = 0; c < 3; ++c) e[c] += fog * double(p[P_FOG_R + c]);
        }
        for (int c = 0; c < 3; ++c) out[size_t(i) * 3 + size_t(c)] = e[c];
    }
}

// --- film_present / light, in double -----------------------------------------

double perf_sdf(D2 st, const Block& p, uint32_t& hole) {
    const double W = double(p[P_FILM_W]);
    const double pitch = double(p[P_PERF_PITCH]), ph = double(p[P_PERF_PHASE]);
    const double k = std::floor((st.x - ph - 0.5 * double(p[P_PERF_W])) / pitch + 0.5);
    const bool top = st.y < 0.5 * W;
    const uint32_t seed = uint32_t(double(p[P_PERF_SEED]));
    hole = hash_u(uint32_t(int(k) * 2 + (top ? 0 : 1)) * 0x9E3779B9u ^ seed);
    const double j0 = hash_f(int(hole & 0xFFFFu), seed ^ 0x1u) - 0.5;
    const double j1 = hash_f(int(hole & 0xFFFFu), seed ^ 0x2u) - 0.5;
    const double j2 = hash_f(int(hole & 0xFFFFu), seed ^ 0x3u) - 0.5;
    const double j3 = hash_f(int(hole & 0xFFFFu), seed ^ 0x4u) - 0.5;
    const double j4 = hash_f(int(hole & 0xFFFFu), seed ^ 0x5u) - 0.5;
    const double wa = double(p[P_PERF_W]) + 0.016 * j2, hc = double(p[P_PERF_H]) + 0.016 * j3;
    const double r = std::clamp(double(p[P_PERF_R]) + 0.08 * j4, 0.3, 0.6);
    const double sc = ph + k * pitch + 0.5 * double(p[P_PERF_W]) + 0.020 * j0;
    const double tc = (top ? double(p[P_PERF_EDGE]) + 0.5 * double(p[P_PERF_H])
                           : W - double(p[P_PERF_EDGE]) - 0.5 * double(p[P_PERF_H])) + 0.024 * j1;
    const double qx = std::fabs(st.x - sc) - (0.5 * wa - r), qy = std::fabs(st.y - tc) - (0.5 * hc - r);
    const double mx = std::max(qx, 0.0), my = std::max(qy, 0.0);
    double d = std::sqrt(mx * mx + my * my) + std::min(std::max(qx, qy), 0.0) - r;
    const double rough = 0.001 + 0.003 * hash_f(int(hole & 0xFFFFu), seed ^ 0x6u);
    d += rough * (value_noise(st.x / 0.05, seed ^ 0x77u) + value_noise(st.y / 0.05, seed ^ 0x99u) - 1.0) * 2.0;
    if (hash_f(int(hole & 0xFFFFu), seed ^ 0x7u) < 0.25) {
        const double ang = 6.2831853 * hash_f(int(hole & 0xFFFFu), seed ^ 0x8u);
        const double pbx = sc + std::cos(ang) * 0.5 * wa, pby = tc + std::sin(ang) * 0.5 * hc;
        const double sig = 0.020 + 0.030 * hash_f(int(hole & 0xFFFFu), seed ^ 0x9u);
        const double amp = (hash_f(int(hole & 0xFFFFu), seed ^ 0xAu) < 0.5 ? -1.0 : 1.0) *
                           (0.006 + 0.010 * hash_f(int(hole & 0xFFFFu), seed ^ 0xBu));
        d += amp * std::exp(-((st.x - pbx) * (st.x - pbx) + (st.y - pby) * (st.y - pby)) / (sig * sig));
    }
    return d;
}

double erf_as(double x) {
    const double t = 1.0 / (1.0 + 0.3275911 * std::fabs(x));
    const double y = 1.0 - (((((1.061405429 * t - 1.453152027) * t) + 1.421413741) * t - 0.284496736) * t + 0.254829592) * t * std::exp(-x * x);
    return x < 0.0 ? -y : y;
}

double film_alpha(D2 st, const Block& p) {
    const double W = double(p[P_FILM_W]), px = double(p[P_PX]);
    double a = p[P_CARRIER] > 1.5f ? 1.0
        : std::clamp(std::min(st.y, W - st.y) / px + 0.5, 0.0, 1.0);
    if (p[P_PERFORATED] > 0.5f) {
        uint32_t hole;
        const double sigma = std::sqrt(0.017 * 0.017 + 0.0961 * px * px);
        a *= 0.5 + 0.5 * erf_as(perf_sdf(st, p, hole) / (1.41421356 * sigma));
    }
    return a;
}

void reference_film_present(const Case& cs, const std::vector<float>& cmy, std::vector<double>& out) {
    const Block& p = cs.p;
    const uint32_t cw = uint32_t(double(p[P_CW])), ch = uint32_t(double(p[P_CH]));
    const uint32_t n = cw * ch;
    out.assign(size_t(n) * 3, 0.0);
    for (uint32_t i = 0; i < n; ++i) {
        const double a = film_alpha(canvas_to_film(double(i % cw), double(i / cw), p), p);
        const double hole[3] = {double(p[P_HOLE_C]), double(p[P_HOLE_M]), double(p[P_HOLE_Y])};
        for (int c = 0; c < 3; ++c) {
            const double v = double(cmy[size_t(i) * 3 + size_t(c)]);
            out[size_t(i) * 3 + size_t(c)] = a * v + (1.0 - a) * hole[c];
        }
    }
}

double smoothstepd(double e0, double e1, double x) {
    const double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

void reference_light(const Case& cs, const std::vector<float>& rgb, std::vector<double>& out) {
    const Block& p = cs.p;
    const uint32_t cw = uint32_t(double(p[P_CW])), ch = uint32_t(double(p[P_CH]));
    const uint32_t n = cw * ch;
    out.assign(size_t(n) * 3, 0.0);
    for (uint32_t i = 0; i < n; ++i) {
        const D2 st = canvas_to_film(double(i % cw), double(i / cw), p);
        const double a = film_alpha(st, p);
        const bool white = p[P_HOLES_LIGHT] > 0.5f;
        double light[3] = {double(p[P_LIGHT_R]), double(p[P_LIGHT_G]), double(p[P_LIGHT_B])};
        double c[3] = {double(rgb[size_t(i) * 3]), double(rgb[size_t(i) * 3 + 1]),
                       double(rgb[size_t(i) * 3 + 2])};
        if (p[P_PERFORATED] > 0.5f) {
            uint32_t hole;
            const double d = perf_sdf(st, p, hole);
            const uint32_t seed = uint32_t(double(p[P_PERF_SEED]));
            const int h = int(hole & 0xFFFFu);
            if (d > -0.03 && d < 0.15) {
                const double e = 0.003;
                uint32_t hh;
                const double gx = perf_sdf({st.x + e, st.y}, p, hh) - perf_sdf({st.x - e, st.y}, p, hh);
                const double gy = perf_sdf({st.x, st.y + e}, p, hh) - perf_sdf({st.x, st.y - e}, p, hh);
                const double gl = std::sqrt(gx * gx + gy * gy);
                const double nx = gx / std::max(gl, 1e-6), ny = gy / std::max(gl, 1e-6);
                const double vx = double(p[P_WALL_VS]), vy = double(p[P_WALL_VT]);
                const double per_hole = 0.4 + 0.6 * hash_f(h, seed ^ 0xE1u);
                const double patch = smoothstepd(0.25, 0.75,
                    0.5 * (value_noise(st.x / 0.35, seed ^ uint32_t(h)) +
                           value_noise(st.y / 0.35, seed ^ uint32_t(h) ^ 0x3Cu)));
                const double side = 0.6 + 0.8 * std::max(0.0, nx * vx + ny * vy);
                const double k = per_hole * patch * side;
                const double tint[3] = {mixd(1.0, double(p[P_BASE_R]), 0.3),
                                        mixd(1.0, double(p[P_BASE_G]), 0.3),
                                        mixd(1.0, double(p[P_BASE_B]), 0.3)};
                const double lam = double(p[P_WALL_SCATTER]);
                if (d >= 0.0) {
                    const double lift = double(p[P_WALL_GLOW]) * k * std::exp(-d / lam);
                    for (int ch2 = 0; ch2 < 3; ++ch2)
                        c[ch2] += lift * tint[ch2] * (white ? light[ch2] : 1.0);
                } else if (white && hash_f(h, seed ^ 0xE4u) < 0.5) {
                    for (int ch2 = 0; ch2 < 3; ++ch2)
                        light[ch2] *= 1.0 - 0.12 * k * (1.0 - tint[ch2]) * std::exp(d / 0.012);
                }
            }
            if (white && d >= 0.0) {
                const double fl = 0.012 * hash_f(h, seed ^ 0xC4u) *
                                  (hash_f(h, seed ^ 0xC5u) >= 0.5 ? 1.0 : 0.0);
                for (int ch2 = 0; ch2 < 3; ++ch2) c[ch2] += fl * std::exp(-d / 0.025) * light[ch2];
            }
        }
        const double W = double(p[P_FILM_W]), px = double(p[P_PX]);
        const double on_strip = p[P_CARRIER] > 1.5f ? 1.0
            : std::clamp(std::min(st.y, W - st.y) / px + 0.5, 0.0, 1.0);
        const double in_film = on_strip > 0.0 ? a / on_strip : 0.0;
        const double carrier = double(p[P_CARRIER]);
        for (int ch2 = 0; ch2 < 3; ++ch2) {
            const double film = light[ch2] * (1.0 - in_film) + c[ch2] * in_film;
            out[size_t(i) * 3 + size_t(ch2)] = carrier * (1.0 - on_strip) + film * on_strip;
        }
    }
}

// --- node_headroom's kernel, in double ---------------------------------------
// `spk_highlight_headroom` (nodes.metal :58): reached whenever a film's
// halation model turns the base's back reflection on (CineStill 800T), and by
// the film-edge path, which lays the edge over this lifted exposure.
double smoothstep01(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

void reference_headroom(const std::vector<float>& x, const float* p, std::vector<double>& out) {
    out.assign(x.size(), 0.0);
    const size_t pixels = x.size() / 3;
    for (size_t i = 0; i < pixels; ++i) {
        const double vx = double(x[3 * i]), vy = double(x[3 * i + 1]), vz = double(x[3 * i + 2]);
        const double m = std::max(vx, std::max(vy, vz));
        double g = 1.0;
        if (m > 0.0) {
            const double lm = std::log2(m);
            const double near_clip = smoothstep01((lm + std::log2(double(p[0])) - double(p[1])) * double(p[2]));
            const double over_floor = smoothstep01((lm + std::log2(double(p[4])) - double(p[5])) * double(p[6]));
            g = std::exp2(double(p[3]) * near_clip * over_floor);
        }
        out[3 * i] = vx * g;
        out[3 * i + 1] = vy * g;
        out[3 * i + 2] = vz * g;
    }
}

bool check_headroom(Gpu* gpu, std::string& error) {
    // The engine's own numbers: the clip window a stop below the frame's
    // maximum, the floor 1.5..2.5 EV over mid-grey, and the edge cases
    // (m == 0 keeps the identity; shapes crossing the workgroup boundary and
    // the dispatch fold).
    const float params[7] = {0.25f, -1.0f, 1.0f, 4.0f, 1.0f / 0.184f, 1.5f, 1.0f};
    const uint32_t shapes[] = {1, 255, 256, 257, 1025, 70000};
    for (uint32_t s : shapes) {
        std::vector<float> x(size_t(s) * 3);
        for (size_t i = 0; i < x.size(); ++i) {
            x[i] = pattern(uint32_t(i), 41u) * 2.2f;
            if ((i / 3) % 17 == 0) x[i] = 0.0f;   // some black pixels
            if ((i / 3) % 29 == 0) x[i] = 2.2f;   // some clipped
        }
        std::vector<double> want;
        reference_headroom(x, params, want);
        auto input = gpu->upload(x.data(), x.size() * sizeof(float), error);
        auto parameters = gpu->upload(params, sizeof params, error);
        if (!input || !parameters) return false;
        auto output = output_buffer(gpu, x.size(), error);
        if (!output) return false;
        const uint32_t meta[1] = {s};
        if (!gpu->dispatch("spk_highlight_headroom",
                           {Arg::buf(input), Arg::buf(parameters), Arg::inline_bytes(meta, 1),
                            Arg::buf(output)},
                           s, error))
            return false;
        if (!check_values(gpu, output, want, "highlight headroom", false, error)) return false;
    }
    return true;
}

// --- the drivers -------------------------------------------------------------

bool check_canvas(Gpu* gpu, std::string& error) {
    Case small = horizontal("small 135-like, no penumbra", 2.0f, 1.0f, 0.0f);
    small.p[P_CW] = 24; small.p[P_CH] = 16;
    small.p[P_CU] = 9.0f; small.p[P_CV] = 6.0f;
    small.p[P_PAIR_ADV] = 19.0f;                     // the pair's second exposure
    small.frame = frame_fixture(24, 18, 31u);
    Case hard = horizontal("hard gate edge (penumbra 0), black carrier", 0.0f, 0.0f, 0.0f);
    Case soft = horizontal("soft penumbra, open carrier, black holes", 1.0f, 0.0f, 0.05f);
    Case pair120 = vertical_pair("vertical 120 pair, quarter turn");
    Case fold_off = fold_case("fold crossing (70400 threads), no perforations", 0.0f);
    Case fold_perf = fold_case("fold crossing with perforations", 1.0f);
    const Case* all[] = {&small, &hard, &soft, &pair120, &fold_off, &fold_perf};
    for (const Case* cs : all) {
        std::vector<double> want;
        reference_canvas(*cs, want);
        auto frame_buf = gpu->upload(cs->frame.rgb.data(), cs->frame.rgb.size() * sizeof(float), error);
        auto p_buf = gpu->upload(cs->p.v.data(), cs->p.v.size() * sizeof(float), error);
        if (!frame_buf || !p_buf) return false;
        const uint32_t cw = uint32_t(double(cs->p[P_CW])), ch = uint32_t(double(cs->p[P_CH]));
        const uint32_t n = cw * ch;
        auto output = output_buffer(gpu, size_t(n) * 3, error);
        if (!output) return false;
        const uint32_t meta[1] = {n};
        if (!gpu->dispatch("spk_overscan_canvas",
                           {Arg::buf(frame_buf), Arg::buf(p_buf), Arg::inline_bytes(meta, 1),
                            Arg::buf(output)},
                           n, error))
            return false;
        if (!check_values(gpu, output, want, cs->name, true, error)) return false;
    }
    return true;
}

bool check_add_mask(Gpu* gpu, std::string& error) {
    // A 64x48 canvas, three groups accumulated in sequence onto the same
    // canvas (the pipeline's loop), boxes deliberately clipped by the canvas
    // edge and a zero-coverage run inside.
    const uint32_t cw = 64, ch = 48;
    std::vector<float> canvas(size_t(cw) * ch * 3);
    for (size_t i = 0; i < canvas.size(); ++i) canvas[i] = 0.03f + pattern(uint32_t(i), 3u) * 0.1f;
    std::vector<double> want(canvas.size());
    for (size_t i = 0; i < canvas.size(); ++i) want[i] = double(canvas[i]);
    struct Group { int x0, y0, w, h; double e[3]; };
    const Group groups[] = {
        {4, 6, 56, 36, {0.42, 0.30, 0.11}},
        {-6, 40, 20, 20, {0.05, 0.04, 0.02}},      // clipped off the canvas's left/bottom
        {44, 30, 30, 30, {0.9, 0.7, 0.5}},         // clipped off the right/top
    };
    auto canvas_buf = gpu->upload(canvas.data(), canvas.size() * sizeof(float), error);
    if (!canvas_buf) return false;
    for (const Group& gr : groups) {
        std::vector<float> mask(size_t(gr.w) * size_t(gr.h));
        for (size_t i = 0; i < mask.size(); ++i)
            mask[i] = (i % 13 == 5) ? 0.0f : pattern(uint32_t(i), 17u);
        for (int y = 0; y < gr.h; ++y)
            for (int x = 0; x < gr.w; ++x) {
                const int px = gr.x0 + x, py = gr.y0 + y;
                if (px < 0 || py < 0 || px >= int(cw) || py >= int(ch)) continue;
                const float c = mask[size_t(y) * size_t(gr.w) + size_t(x)];
                if (c <= 0.0f) continue;
                for (int ch2 = 0; ch2 < 3; ++ch2)
                    want[(size_t(py) * cw + size_t(px)) * 3 + size_t(ch2)] +=
                        double(c) * gr.e[ch2];
            }
        auto mask_buf = gpu->upload(mask.data(), mask.size() * sizeof(float), error);
        if (!mask_buf) return false;
        const float m[9] = {float(gr.x0), float(gr.y0), float(gr.w), float(gr.h),
                            float(cw), float(ch), float(gr.e[0]), float(gr.e[1]), float(gr.e[2])};
        const uint32_t meta[1] = {uint32_t(size_t(gr.w) * size_t(gr.h))};
        if (!gpu->dispatch("spk_overscan_add_mask",
                           {Arg::buf(mask_buf), Arg::inline_bytes(m, 9),
                            Arg::inline_bytes(meta, 1), Arg::buf(canvas_buf)},
                           size_t(gr.w) * size_t(gr.h), error))
            return false;
    }
    if (!gpu->flush(error)) return false;
    std::vector<float> actual;
    if (!read_test_buffer(gpu, canvas_buf, actual, error)) return false;
    const double absolute_tolerance = 1e-6, relative_tolerance = 1e-6;
    for (size_t i = 0; i < want.size(); ++i) {
        const double delta = std::fabs(double(actual[i]) - want[i]);
        const double tolerance = absolute_tolerance + relative_tolerance * std::fabs(want[i]);
        if (!(std::isfinite(actual[i]) && delta <= tolerance)) {
            char detail[256];
            std::snprintf(detail, sizeof detail, "add_mask index=%zu got=%.9g want=%.17g",
                          i, double(actual[i]), want[i]);
            error = detail;
            return false;
        }
        maximum_error = std::max(maximum_error, delta);
    }
    ++cases;
    return true;
}

bool check_film_and_light(Gpu* gpu, std::string& error) {
    Case horiz = horizontal("film present + light on a perforated strip", 0.0f, 1.0f, 0.03f);
    Case black = horizontal("black holes, black carrier", 0.0f, 0.0f, 0.03f);
    Case pair120 = vertical_pair("film present + light on an unperforated pair");
    const Case* all[] = {&horiz, &black, &pair120};
    for (const Case* cs : all) {
        const uint32_t cw = uint32_t(double(cs->p[P_CW])), ch = uint32_t(double(cs->p[P_CH]));
        const uint32_t n = cw * ch;
        std::vector<float> cmy(size_t(n) * 3);
        for (size_t i = 0; i < cmy.size(); ++i) cmy[i] = -0.2f + pattern(uint32_t(i), 23u) * 1.4f;
        auto input = gpu->upload(cmy.data(), cmy.size() * sizeof(float), error);
        auto p_buf = gpu->upload(cs->p.v.data(), cs->p.v.size() * sizeof(float), error);
        if (!input || !p_buf) return false;
        {
            std::vector<double> want;
            reference_film_present(*cs, cmy, want);
            auto output = output_buffer(gpu, size_t(n) * 3, error);
            if (!output) return false;
            const uint32_t meta[1] = {n};
            if (!gpu->dispatch("spk_overscan_film_present",
                               {Arg::buf(input), Arg::buf(p_buf), Arg::inline_bytes(meta, 1),
                                Arg::buf(output)},
                               n, error))
                return false;
            if (!check_values(gpu, output, want, cs->name, false, error)) return false;
        }
        {
            std::vector<double> want;
            reference_light(*cs, cmy, want);
            auto output = output_buffer(gpu, size_t(n) * 3, error);
            if (!output) return false;
            const uint32_t meta[1] = {n};
            if (!gpu->dispatch("spk_overscan_light",
                               {Arg::buf(input), Arg::buf(p_buf), Arg::inline_bytes(meta, 1),
                                Arg::buf(output)},
                               n, error))
                return false;
            if (!check_values(gpu, output, want, cs->name, false, error)) return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr, "usage: spk_vulkan_overscan <resources>\n"); return 2; }
    std::string error;
    std::unique_ptr<Gpu> gpu(Gpu::create_vulkan(std::string(argv[1]) + "/vulkan", error));
    if (!gpu) { std::fprintf(stderr, "Vulkan device: %s\n", error.c_str()); return 1; }
    std::printf("Vulkan device: %s\n", gpu->device_name().c_str());
    if (!check_canvas(gpu.get(), error)) {
        std::fprintf(stderr, "overscan canvas: %s\n", error.c_str()); return 1;
    }
    if (!check_add_mask(gpu.get(), error)) {
        std::fprintf(stderr, "overscan add_mask: %s\n", error.c_str()); return 1;
    }
    if (!check_film_and_light(gpu.get(), error)) {
        std::fprintf(stderr, "overscan film_present/light: %s\n", error.c_str()); return 1;
    }
    if (!check_headroom(gpu.get(), error)) {
        std::fprintf(stderr, "highlight headroom: %s\n", error.c_str()); return 1;
    }
    std::printf("overscan: %zu float64 cases, max abs error %.9g, "
                "max error/max(1,abs(reference)) %.9g, all guards intact\n",
                cases, maximum_error, maximum_scaled_error);
    return 0;
}
