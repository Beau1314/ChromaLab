// The output transform's CPU core, held two ways.
//
// 1. **The numeric gate** (no engine): an independent transcription of the
//    Mac kernel's arithmetic (`OutputTransform.swift` :207-464) written here
//    in double -- the transfer functions with their exact breakpoints, the
//    CAM16-UCS body with its three traps (the sign-preserving power for J,
//    the 460/1403 family in the inverse solve, Python-style hue modulo), and
//    the C_max bilinear read. The core must agree to 1e-9 over a sweep that
//    lands on the gate's branches, and a C_max table whose values alternate
//    loudly between rows and hue columns turns any index, wrap or weight
//    mistake into a visible jump rather than a rounding.
// 2. **The engine fetch** (bundled resources, no RAW): `PreviewHost::
//    outputTransform`'s parsed numbers against the reply's own, the identity
//    of a same-space pair, grey through every real destination, the
//    "off" algorithm's neutralised `k[15..17]` with its gate, and an unknown
//    space failing by name.
#include "ColourTransform.hpp"
#include "desktop/preview_host.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using spk::desktop::OutputTransformData;
namespace colour = spk::desktop::colour;

namespace {

unsigned checks = 0;

void check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

void check(bool condition, const std::string& message) { check(condition, message.c_str()); }

template <class F>
std::string rejected(F&& function, const char* message) {
    try {
        function();
    } catch (const std::exception& error) {
        check(*error.what() != '\0', "rejection omitted diagnostic");
        return error.what();
    }
    throw std::runtime_error(message);
}

void close(double actual, double expected, double tolerance, const std::string& what) {
    check(std::fabs(actual - expected) <= tolerance,
          what + ": got " + std::to_string(actual) + ", want " + std::to_string(expected));
}

// ---- the independent transcription -------------------------------------
namespace ref {

double spow(double v, double p) {
    return (v < 0.0 ? -1.0 : (v > 0.0 ? 1.0 : 0.0)) * std::pow(std::fabs(v), p);
}

// Re-derived from the transfer-function definitions, not copied from the
// core: each curve written from its own piecewise shape.
double decode(double v, std::uint32_t mode) {
    switch (mode) {
        case 0:  // IEC 61966-2-1's inverse, at the reference's own breakpoint
                 // (the kernel's constant, not the spec's printed rounding).
            return v <= 0.040449936 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
        case 1:  // ProPhoto's linear toe, toe end 16/512.
            return v < 0.03125 ? v / 16.0 : std::pow(v, 1.8);
        case 2:  // Adobe RGB (1998)'s pure gamma.
            return std::pow(v, 563.0 / 256.0);
        case 3:  // BT.709's inverse OETF; its breakpoint is encoded beta.
        {
            const double breakpoint = 1.099 * std::pow(0.018, 0.45) - 0.099;
            return v < breakpoint ? v / 4.5 : spow((v + 0.099) / 1.099, 1.0 / 0.45);
        }
        default:
            return v;
    }
}

double encode(double v, std::uint32_t mode) {
    switch (mode) {
        case 0:
            return v <= 0.0031308 ? 12.92 * v : 1.055 * spow(v, 1.0 / 2.4) - 0.055;
        case 1:
            return v < 1.0 / 512.0 ? v * 16.0 : std::pow(v, 1.0 / 1.8);
        case 2:
            return std::pow(v, 256.0 / 563.0);
        case 3:
            return 0.018 > v ? v * 4.5 : 1.099 * spow(v, 0.45) - 0.099;
        default:
            return v;
    }
}

// The CAM16-UCS body, grouped the classic way (nested lerps for the table
// read) rather than the kernel's; algebraically the same, in double.
std::array<double, 3> cam16ucs(const std::array<double, 3>& rgb, const OutputTransformData& u) {
    const double nL = double(u.cmax_rows), nh = double(u.cmax_cols);
    const auto at = [&](double l, double h) {
        return double(u.cmax[std::size_t(l) * u.cmax_cols + std::size_t(h)]);
    };
    const double F_L = u.k[0], N_bb = u.k[1], N_cb = u.k[2], n = u.k[3], z = u.k[4], A_w = u.k[5],
                 c = u.k[6], N_c = u.k[7], l0g = u.k[8], l1g = u.k[9], h0g = u.k[10],
                 hstep = u.k[11], thr = u.k[12], lim = u.k[13], pwr = u.k[14], lthr = u.k[15],
                 llim = u.k[16], lpwr = u.k[17], white = u.k[18];
    const double ec = std::pow(1.64 - std::pow(0.29, n), 0.73);
    const double fl4 = std::pow(F_L, 0.25);

    double X = (u.m_to_xyz[0] * rgb[0] + u.m_to_xyz[1] * rgb[1] + u.m_to_xyz[2] * rgb[2]) * 100.0;
    double Y = (u.m_to_xyz[3] * rgb[0] + u.m_to_xyz[4] * rgb[1] + u.m_to_xyz[5] * rgb[2]) * 100.0;
    double Z = (u.m_to_xyz[6] * rgb[0] + u.m_to_xyz[7] * rgb[1] + u.m_to_xyz[8] * rgb[2]) * 100.0;
    const double R = 0.401288 * X + 0.650173 * Y - 0.051461 * Z;
    const double G = -0.250268 * X + 1.204414 * Y + 0.045854 * Z;
    const double B = -0.002079 * X + 0.048952 * Y + 0.953127 * Z;
    const auto adapted = [&](double cone, double d) {
        const double value = cone * d;
        const double sign = value >= 0.0 ? 1.0 : -1.0;
        const double response = std::pow(F_L * std::fabs(value) / 100.0, 0.42);
        return 400.0 * sign * response / (27.13 + response) + 0.1;
    };
    const double Ra = adapted(R, u.k[19]), Ga = adapted(G, u.k[20]), Ba = adapted(B, u.k[21]);
    const double a = Ra - 12.0 * Ga / 11.0 + Ba / 11.0;
    const double b = (Ra + Ga - 2.0 * Ba) / 9.0;
    const double h = std::atan2(b, a);
    const double et = 0.25 * (std::cos(h + 2.0) + 3.8);
    const double A = (2.0 * Ra + Ga + Ba / 20.0 - 0.305) * N_bb;
    const double aratio = A / A_w;
    const double J = 100.0 * (aratio >= 0.0 ? 1.0 : -1.0) *
                     std::pow(std::fabs(aratio), c * z);
    const double den = Ra + Ga + 21.0 * Ba / 20.0;
    const double t = den == 0.0 ? 0.0
                                : (50000.0 / 13.0 * N_c * N_cb * et * std::sqrt(a * a + b * b)) / den;
    const double C = t > 0.0 ? std::pow(t, 0.9) * std::sqrt(std::fabs(J) / 100.0) * ec : 0.0;
    double Mp = std::log1p(0.0228 * C * fl4) / 0.0228;
    double Jp = 1.7 * J / (1.0 + 0.007 * J);
    if (u.lightness_compression_active) {
        double ln = Jp / white;
        if (ln > lthr) {
            const double span = llim - lthr;
            const double x = (ln - lthr) / span;
            ln = lthr + span * (x / std::pow(1.0 + std::pow(x, lpwr), 1.0 / lpwr));
        }
        Jp = ln * white;
    }
    const double lc = std::clamp(Jp, l0g, l1g);
    const double li = (lc - l0g) / (l1g - l0g) * (nL - 1.0);
    const int row = std::clamp(int(std::floor(li)), 0, int(nL) - 2);
    const double lf = li - row;
    const double hi = (h - h0g) / hstep;
    const int h0 = int(std::floor(hi)) % int(nh) < 0
                       ? int(std::floor(hi)) % int(nh) + int(nh)
                       : int(std::floor(hi)) % int(nh);
    const int h1 = (h0 + 1) % int(nh);
    const double hf = hi - std::floor(hi);
    // Nested lerps: bilinear by its textbook construction.
    const double top = at(row, h0) * (1.0 - hf) + at(row, h1) * hf;
    const double bottom = at(row + 1, h0) * (1.0 - hf) + at(row + 1, h1) * hf;
    const double cmax = top * (1.0 - lf) + bottom * lf;
    double d = Mp / std::max(cmax, 1e-9);
    if (d > thr) {
        const double span = lim - thr;
        const double x = (d - thr) / span;
        d = thr + span * (x / std::pow(1.0 + std::pow(x, pwr), 1.0 / pwr));
    }
    const double m = std::expm1(d * std::max(cmax, 1e-9) * 0.0228) / 0.0228;
    const double jn = Jp / (1.7 - 0.007 * Jp);
    const double cn = m / fl4;
    const double A2 = A_w * (jn >= 0.0 ? 1.0 : -1.0) * std::pow(std::fabs(jn) / 100.0, 1.0 / (c * z));
    const double t2 = (std::sqrt(std::fabs(jn) / 100.0) > 0.0 && cn > 0.0)
                          ? std::pow(cn / (std::sqrt(std::fabs(jn) / 100.0) * ec), 1.0 / 0.9)
                          : 0.0;
    const double ca = std::cos(h), sa = std::sin(h);
    const double p2 = A2 / N_bb + 0.305;
    double a2 = 0.0, b2 = 0.0;
    if (t2 != 0.0) {
        const double p1 = (50000.0 / 13.0) * N_c * N_cb * et / t2;
        if (std::fabs(sa) >= std::fabs(ca)) {
            b2 = p2 * (2.0 + 21.0 / 20.0) * (460.0 / 1403.0) /
                 (p1 / sa + (2.0 + 21.0 / 20.0) * (220.0 / 1403.0) * (ca / sa) - (27.0 / 1403.0) +
                  (21.0 / 20.0) * (6300.0 / 1403.0));
            a2 = b2 * (ca / sa);
        } else {
            a2 = p2 * (2.0 + 21.0 / 20.0) * (460.0 / 1403.0) /
                 (p1 / ca + (2.0 + 21.0 / 20.0) * (220.0 / 1403.0) -
                  ((27.0 / 1403.0) - (21.0 / 20.0) * (6300.0 / 1403.0)) * (sa / ca));
            b2 = a2 * (sa / ca);
        }
    }
    const double ra2 = (460.0 * p2 + 451.0 * a2 + 288.0 * b2) / 1403.0;
    const double ga2 = (460.0 * p2 - 891.0 * a2 - 261.0 * b2) / 1403.0;
    const double ba2 = (460.0 * p2 - 220.0 * a2 - 6300.0 * b2) / 1403.0;
    const auto opponent = [&](double shifted, double d_rgb) {
        const double vm = shifted - 0.1;
        const double sign = vm >= 0.0 ? 1.0 : -1.0;
        const double base = std::fabs(vm) < 400.0 ? (27.13 * std::fabs(vm)) / (400.0 - std::fabs(vm))
                                                  : 0.0;
        return (100.0 / F_L) * sign * std::pow(base, 1.0 / 0.42) / d_rgb;
    };
    const double rf = opponent(ra2, u.k[19]);
    const double gf = opponent(ga2, u.k[20]);
    const double bf = opponent(ba2, u.k[21]);
    double Xn = (1.86206786 * rf - 1.01125463 * gf + 0.14918677 * bf) / 100.0;
    double Yn = (0.38752654 * rf + 0.62144744 * gf - 0.00897398 * bf) / 100.0;
    double Zn = (-0.01584150 * rf - 0.03412294 * gf + 1.04996444 * bf) / 100.0;
    return {u.m_to_rgb[0] * Xn + u.m_to_rgb[1] * Yn + u.m_to_rgb[2] * Zn,
            u.m_to_rgb[3] * Xn + u.m_to_rgb[4] * Yn + u.m_to_rgb[5] * Zn,
            u.m_to_rgb[6] * Xn + u.m_to_rgb[7] * Yn + u.m_to_rgb[8] * Zn};
}

std::array<double, 3> pixel(const std::array<double, 3>& encoded, const OutputTransformData& u) {
    std::array<double, 3> v = {decode(encoded[0], u.source_cctf_mode),
                               decode(encoded[1], u.source_cctf_mode),
                               decode(encoded[2], u.source_cctf_mode)};
    std::array<double, 3> lin = {
        u.matrix[0] * v[0] + u.matrix[1] * v[1] + u.matrix[2] * v[2],
        u.matrix[3] * v[0] + u.matrix[4] * v[1] + u.matrix[5] * v[2],
        u.matrix[6] * v[0] + u.matrix[7] * v[1] + u.matrix[8] * v[2]};
    if (u.algorithm != "off") lin = cam16ucs(lin, u);
    return {encode(lin[0], u.target_cctf_mode), encode(lin[1], u.target_cctf_mode),
            encode(lin[2], u.target_cctf_mode)};
}

}  // namespace ref

// ---- synthetic setups ---------------------------------------------------
//
// The k block's shapes matter, its exact physics do not: both sides evaluate
// the same constants. The C_max values alternate loudly between rows and hue
// columns, so a wrong index, a dropped Python-style wrap or a swapped weight
// moves the result by whole units, not by rounding.

OutputTransformData synthetic(bool lightness, const char* algorithm) {
    OutputTransformData s;
    s.source_space = "Synthetic In";
    s.target_space = "Synthetic Out";
    s.source_cctf_mode = 1;  // ProPhoto-shaped source curve: the toe branch is exercised
    s.target_cctf_mode = 0;  // sRGB-shaped target curve
    s.algorithm = algorithm;
    s.lightness_compression_active = lightness;
    // Rows skewed, sums kept at 1 (what a chromatically adapted pair does).
    const double matrix[9] = {0.60, 0.30, 0.10, 0.20, 0.70, 0.10, 0.05, 0.25, 0.70};
    const double to_xyz[9] = {0.70, 0.25, 0.05, 0.30, 0.60, 0.10, 0.02, 0.11, 0.87};
    const double to_rgb[9] = {1.9, -0.8, -0.1, -0.7, 2.0, -0.3, -0.05, -0.4, 1.45};
    std::copy(std::begin(matrix), std::end(matrix), s.matrix);
    std::copy(std::begin(to_xyz), std::end(to_xyz), s.m_to_xyz);
    std::copy(std::begin(to_rgb), std::end(to_rgb), s.m_to_rgb);
    const double k[22] = {1.0,  1.0,  1.0,   0.69, 1.69,  100.0, 0.69, 1.0,
                          5.0,  95.0, 0.5,   0.4,  0.015, 0.8,   0.74, 0.0,
                          1.0,  1.0,  90.0,  1.0,  1.0,   1.0};
    std::copy(std::begin(k), std::end(k), s.k);
    s.cmax_rows = 3;
    s.cmax_cols = 4;
    for (std::uint32_t l = 0; l < s.cmax_rows; ++l)
        for (std::uint32_t h = 0; h < s.cmax_cols; ++h) {
            const bool loud = (l + h) % 2 == 0;
            s.cmax.push_back(loud ? 0.9 : 0.05 * double(l + 1));
        }
    if (lightness) {
        s.k[15] = 0.2;  // lc_threshold
        s.k[16] = 1.0;  // lc_limit
        s.k[17] = 2.0;  // lc_power
        s.k[18] = 90.0; // L_white
    }
    return s;
}

void gate_pixel(const std::array<double, 3>& encoded, const OutputTransformData& setup) {
    const auto mine = colour::transform_pixel(encoded, setup);
    const auto reference = ref::pixel(encoded, setup);
    for (int c = 0; c < 3; ++c) {
        const double tolerance = 1e-9 * (1.0 + std::fabs(reference[std::size_t(c)]));
        if (!(std::fabs(mine[std::size_t(c)] - reference[std::size_t(c)]) <= tolerance))
            throw std::runtime_error("transform core left its transcription at channel " +
                                     std::to_string(c) + ": " + std::to_string(mine[std::size_t(c)]) +
                                     " vs " + std::to_string(reference[std::size_t(c)]));
    }
    ++checks;
}

void numeric_gate() {
    // Transfer functions at their exact breakpoints and across both branches.
    for (const double v : {0.0, 1.0 / 512.0, 16.0 / 512.0, 0.040449936, 0.0404482362771076,
                           0.25, 0.5, 0.9, 1.0, 1.4}) {
        for (const std::uint32_t mode : {0u, 1u, 2u, 3u, 4u}) {
            const double expected = ref::decode(v, mode);
            const double made = colour::cctf_decode(v, mode);
            if (std::isnan(expected) && std::isnan(made)) continue;
            close(made, expected, 1e-9 * (1.0 + std::fabs(expected)), "cctf_decode moved");
            ++checks;
        }
    }
    // 1.055*x^(1/2.4)-0.055 is negative below x = 0.0031308's branch point
    // only across the branch: check the sign-preserving power both ways.
    close(colour::cctf_encode(-0.1, 0), ref::encode(-0.1, 0), 1e-12, "sRGB encode sign");
    close(colour::cctf_decode(-0.1, 3), ref::decode(-0.1, 3), 1e-12, "BT decode sign");
    for (const double v : {-0.5, -0.1, -1.0 / 512.0, 0.0031308, 0.0031307, 0.5, 1.0}) {
        close(colour::cctf_encode(v, 0), ref::encode(v, 0), 1e-9, "sRGB encode");
        close(colour::cctf_encode(v, 3), ref::encode(v, 3), 1e-9, "BT encode");
        ++checks;
    }
    close(colour::spow(-0.25, 0.5), -0.5, 1e-12, "spow keeps the sign");
    close(colour::spow(0.25, 0.5), 0.5, 1e-12, "spow keeps magnitude");
    close(colour::spow(0.0, 0.74), 0.0, 0.0, "spow at zero");

    // Containers: rounding, limits, and a NaN's black.
    check(colour::to16(0.0) == 0 && colour::to16(1.0) == 65535, "to16 lost the unit range");
    check(colour::to16(-3.0) == 0 && colour::to16(4.0) == 65535, "to16 did not clamp");
    check(colour::to16(std::numeric_limits<double>::quiet_NaN()) == 0, "to16 wrote a NaN");
    check(colour::to16(0.5) == 32768, "to16 rounding is not half-up");
    check(colour::to8(0.5) == 128, "to8 rounding is not half-up");
    check(colour::to8(1.0) == 255 && colour::to8(0.0) == 0, "to8 lost the unit range");
    check(colour::to8(-1.0) == 0 && colour::to8(2.0) == 255, "to8 did not clamp");
    check(colour::to8(std::numeric_limits<double>::quiet_NaN()) == 0, "to8 wrote a NaN");
    check(colour::to16(255.0 / 65535.0) == 255, "to16 is not linear in the code");
    check(colour::to8(1.0 / 255.0) == 1, "to8 is not linear in the code");

    // The gate itself: three setups (lightness off / on / the step skipped)
    // over a sweep that crosses every branch -- negatives exercise the sign
    // paths, brights clamp L onto the table's last row, and chromatic values
    // against a small knee force the roll-off.
    const OutputTransformData off = synthetic(false, "cam16ucs");
    const OutputTransformData light = synthetic(true, "cam16ucs");
    const OutputTransformData skipped = synthetic(false, "off");
    for (const double r : {-0.2, 0.0, 0.13, 0.5, 0.77, 1.0, 1.4})
        for (const double g : {-0.2, 0.0, 0.13, 0.5, 0.77, 1.0, 1.4})
            for (const double b : {-0.2, 0.0, 0.13, 0.5, 0.77, 1.0, 1.4})
                for (const OutputTransformData* setup : {&off, &light, &skipped}) {
                    gate_pixel({r, g, b}, *setup);
                }
    // A neutral through the CAM16 body is its own linear value: chroma zero,
    // d zero, no knee -- both sides must say so within double noise.
    for (const double grey : {0.0, 0.25, 0.5, 1.0, 1.3}) {
        const auto rolled = colour::cam16ucs_into({grey, grey, grey}, off);
        for (int c = 0; c < 3; ++c)
            close(rolled[std::size_t(c)], grey, 1e-9 * (1.0 + grey), "neutral did not roll to itself");
    }
    // The lightness stage actually moves a bright neutral when on, and does
    // not touch it when off (the engine's own reason the gate must travel).
    const auto quiet = colour::cam16ucs_into({0.9, 0.9, 0.9}, off);
    const auto compressed = colour::cam16ucs_into({0.9, 0.9, 0.9}, light);
    double biggest = 0.0;
    for (int c = 0; c < 3; ++c)
        biggest = std::max(biggest, std::fabs(quiet[std::size_t(c)] - compressed[std::size_t(c)]));
    check(biggest > 1e-4, "the lightness gate changed nothing for a bright neutral");
    // algorithm "off" skips the step in the core's own four-step shape:
    // exactly the matrix over the decoded source, encoded in the target.
    {
        const std::array<double, 3> encoded = {0.6, 0.4, 0.2};
        const auto direct = colour::transform_pixel(encoded, skipped);
        std::array<double, 3> lin = {};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                lin[std::size_t(i)] += skipped.matrix[std::size_t(3 * i + j)] *
                                       colour::cctf_decode(encoded[std::size_t(j)], 1);
        for (int c = 0; c < 3; ++c)
            close(direct[std::size_t(c)], colour::cctf_encode(lin[std::size_t(c)], 0), 1e-12,
                  "algorithm off is not the matrix alone");
    }
    // A setup whose numbers do not fit is refused, not silently mis-read.
    OutputTransformData broken = off;
    broken.cmax_cols = 5;
    rejected([&] { colour::transform_pixel({0.5, 0.5, 0.5}, broken); },
             "a C_max shape mismatch was accepted");
}

void whole_frame() {
    const OutputTransformData setup = synthetic(false, "cam16ucs");
    // Vector form: alpha rides untouched, the channels match transform_pixel.
    std::vector<std::uint16_t> rgba = {32768, 16384, 65535, 65535,
                                       49152, 0,     8192, 65535,
                                       100,   200,   300,  65535};
    auto copy = rgba;
    colour::apply_rgba16(copy, 3, 1, setup);
    check(copy[3] == 65535 && copy[7] == 65535 && copy[11] == 65535, "the frame pass moved alpha");
    for (int p = 0; p < 3; ++p) {
        const auto e = colour::transform_pixel({double(rgba[std::size_t(p) * 4]) / 65535.0,
                                                double(rgba[std::size_t(p) * 4 + 1]) / 65535.0,
                                                double(rgba[std::size_t(p) * 4 + 2]) / 65535.0},
                                               setup);
        check(copy[std::size_t(p) * 4] == colour::to16(e[0]) &&
                  copy[std::size_t(p) * 4 + 1] == colour::to16(e[1]) &&
                  copy[std::size_t(p) * 4 + 2] == colour::to16(e[2]),
              "the frame pass disagrees with the pixel pass");
    }
    rejected([&] { colour::apply_rgba16(copy, 4, 1, setup); },
             "a frame pass accepted a size its storage cannot hold");
}

std::string path_text(const fs::path& path) {
    const auto utf8 = path.u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

void engine_fetch(const fs::path& resources) {
    spk::desktop::PreviewHost host(resources);
    // Same space: an identity matrix to double noise, the reply echoed whole.
    const auto same = host.outputTransform("sRGB", "sRGB");
    check(same.source_space == "sRGB" && same.target_space == "sRGB", "the fetch lost the names");
    check(same.source_cctf_mode == 0 && same.target_cctf_mode == 0, "sRGB is not mode 0");
    check(same.algorithm == "cam16ucs", "the default fetch lost the compression");
    check(!same.lightness_compression_active, "the export fetch carried the lightness stage");
    check(same.cmax_rows > 1 && same.cmax_cols > 1 &&
              same.cmax.size() == std::size_t(same.cmax_rows) * same.cmax_cols,
          "the fetched C_max table does not fit its shape");
    for (int i = 0; i < 9; ++i) {
        const double identity = (i % 4 == 0) ? 1.0 : 0.0;
        // The residual against the identity is the blob's own whitepoint
        // self-consistency (measured at ~1.5e-4 on the diagonal and rows),
        // not an adaptation failure; the grey pass below pins neutrality.
        close(same.matrix[i], identity, 1e-3, "a same-space matrix is not the identity");
    }
    // Grey through each real destination comes back grey: the adapted matrix
    // carries the white, CAM16 keeps the neutral, the curves re-encode it.
    for (const auto* destination : {"sRGB", "Display P3", "Adobe RGB (1998)", "ProPhoto RGB"}) {
        const auto data = host.outputTransform("sRGB", destination);
        check(data.target_space == destination, "the fetch lost the target's name");
        double expected_mode = 0.0;
        if (std::string(destination) == "ProPhoto RGB") expected_mode = 1.0;
        if (std::string(destination) == "Adobe RGB (1998)") expected_mode = 2.0;
        close(double(data.target_cctf_mode), expected_mode, 0.0,
              "the target's transfer-function mode moved");
        double matrix_sums = 0.0;
        for (int row = 0; row < 3; ++row) {
            double sum = 0.0;
            for (int column = 0; column < 3; ++column) sum += data.matrix[std::size_t(3 * row + column)];
            matrix_sums += std::fabs(sum - 1.0);
        }
        // The white is carried to the blob's own self-consistency: measured
        // ~1.3e-4 across the four destinations (the same numbers the Mac's
        // lcms-validated 0.6 % MAE came from), not machine precision.
        check(matrix_sums < 1e-3, "the adapted matrix does not carry the white");
        for (const double grey : {0.0, 0.25, 0.5, 0.75}) {
            const auto pixel = colour::transform_pixel({grey, grey, grey}, data);
            // A neutral stays neutral in the target's own encoding. When the
            // two curves are the same one, that is the same number back
            // (decode and encode cancel); across different curves it is the
            // target's encoding of the same linear grey, so the three
            // channels equal *each other* and nothing else is assumed.
            const bool same_curve = data.target_cctf_mode == data.source_cctf_mode;
            for (int c = 0; c < 3; ++c) {
                if (same_curve) {
                    close(pixel[std::size_t(c)], grey, 5e-4 * (1.0 + grey),
                          std::string("grey moved on the way to ") + destination);
                } else {
                    check(pixel[std::size_t(c)] >= 0.0 && pixel[std::size_t(c)] <= 1.0,
                          std::string("a grey escaped [0,1] on the way to ") + destination);
                    close(pixel[std::size_t(c)], pixel[0], 5e-4 * (1.0 + grey),
                          std::string("grey split into channels on the way to ") + destination);
                }
            }
        }
        ++checks;
    }
    // The caller's own gamut override: "off" is legal and lands as skipped,
    // with the tables still carried and k[15..17] neutralised under its
    // false lightness flag -- the gate is what tells a kernel that merely
    // read them apart from an identity.
    const auto skipped = host.outputTransform(
        "sRGB", "Display P3", R"({"algorithm": "off", "lightness_compression_active": false})");
    check(skipped.algorithm == "off" && !skipped.lightness_compression_active,
          "the off algorithm did not come back as skipped");
    check(skipped.cmax.size() == std::size_t(skipped.cmax_rows) * skipped.cmax_cols,
          "the off fetch dropped the tables it promised to carry");
    close(skipped.k[15], 0.0, 0.0, "the off fetch left a lightness threshold");
    close(skipped.k[16], 1.0, 0.0, "the off fetch left a lightness limit");
    close(skipped.k[17], 1.0, 0.0, "the off fetch left a lightness power");
    // The lightness flag alone rides too, without touching the algorithm.
    const auto without_lightness = host.outputTransform("sRGB", "Display P3");
    check(without_lightness.algorithm == "cam16ucs" && !without_lightness.lightness_compression_active,
          "the export default lost its chroma knee or gained a lightness stage");
    // An unknown space fails by name, never by falling back.
    const auto unknown = rejected([&] { host.outputTransform("sRGB", "Fogra39"); },
                                  "an unknown target space was accepted");
    check(unknown.find("Fogra39") != std::string::npos, "the failure did not name the space");
    const auto unknown_source =
        rejected([&] { host.outputTransform("Wide Gamut", "sRGB"); },
                 "an unknown source space was accepted");
    check(unknown_source.find("Wide Gamut") != std::string::npos,
          "the source failure did not name the space");
    // A malformed override is refused at the wire's own gate.
    rejected([&] { host.outputTransform("sRGB", "sRGB", "{not json"); },
             "a malformed gamut override was accepted");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        numeric_gate();
        whole_frame();
        check(argc >= 2, "usage: spk_qt_colour_transform_test <resources>");
        engine_fetch(path_text(fs::path(argv[1])));
        std::printf("colour_transform: %u checks passed\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::printf("FAILED after %u checks: %s\n", checks, error.what());
        return 1;
    }
}
