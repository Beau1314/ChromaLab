#include "ColourTransform.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace spk::desktop::colour {
namespace {

// Row-major 3x3 apply, `out[i] = sum_j m[3i + j] * x[j]` -- the engine writes
// its matrices row-major exactly so the two sides of the boundary cannot
// cross the convention wrongly once more (AGENTS.md trap 20).
std::array<double, 3> apply9(const double (&m)[9], const std::array<double, 3>& x) {
    return {m[0] * x[0] + m[1] * x[1] + m[2] * x[2],
            m[3] * x[0] + m[4] * x[1] + m[5] * x[2],
            m[6] * x[0] + m[7] * x[1] + m[8] * x[2]};
}

const OutputTransformData& require_setup(const OutputTransformData& s) {
    if (s.cmax.size() != std::size_t(s.cmax_rows) * s.cmax_cols)
        throw std::runtime_error("the output transform's numbers did not fit");
    return s;
}

}  // namespace

double spow(double v, double p) {
    const double s = v < 0.0 ? -1.0 : (v > 0.0 ? 1.0 : 0.0);
    return s * std::pow(std::fabs(v), p);
}

double cctf_decode(double v, std::uint32_t mode) {
    switch (mode) {
        case 0: return (0.040449936 >= v) ? v / 12.92 : spow((v + 0.055) / 1.055, 2.4);
        case 1: return (v < 16.0 * (1.0 / 512.0)) ? v / 16.0 : std::pow(v, 1.8);
        case 2: return std::pow(v, 563.0 / 256.0);
        case 3: {
            const double alpha = 1.099, beta = 0.018;
            const double bp = alpha * std::pow(beta, 0.45) - (alpha - 1.0);
            return (bp > v) ? v / 4.5 : spow((v + (alpha - 1.0)) / alpha, 1.0 / 0.45);
        }
        default: return v;
    }
}

double cctf_encode(double v, std::uint32_t mode) {
    switch (mode) {
        case 0: return (v <= 0.0031308) ? 12.92 * v : 1.055 * spow(v, 1.0 / 2.4) - 0.055;
        case 1: return (v < 1.0 / 512.0) ? v * 16.0 : std::pow(v, 1.0 / 1.8);
        case 2: return std::pow(v, 256.0 / 563.0);
        case 3: {
            const double alpha = 1.099, beta = 0.018;
            return (beta > v) ? v * 4.5 : alpha * spow(v, 0.45) - (alpha - 1.0);
        }
        default: return v;
    }
}

std::array<double, 3> cam16ucs_into(const std::array<double, 3>& rgb_linear,
                                    const OutputTransformData& setup) {
    const OutputTransformData& s = require_setup(setup);
    const std::uint32_t nL = s.cmax_rows, nh = s.cmax_cols;
    const bool lc_active = s.lightness_compression_active;
    const double F_L = s.k[0], N_bb = s.k[1], N_cb = s.k[2], n_ = s.k[3], z = s.k[4],
                 A_w = s.k[5], c_ = s.k[6], N_c = s.k[7];
    const double L_grid0 = s.k[8], L_grid1 = s.k[9], h_grid0 = s.k[10], h_step = s.k[11];
    const double threshold = s.k[12], limit = s.k[13], power_ = s.k[14];
    const double lc_threshold = s.k[15], lc_limit = s.k[16], lc_power = s.k[17], L_white = s.k[18];
    const double D0 = s.k[19], D1 = s.k[20], D2 = s.k[21];
    const double e_c = std::pow(1.64 - std::pow(0.29, n_), 0.73);
    const double inv_FL4 = std::pow(F_L, 0.25);

    std::array<double, 3> xyz = apply9(s.m_to_xyz, rgb_linear);
    double X = xyz[0], Y = xyz[1], Z = xyz[2];
    X *= 100.0; Y *= 100.0; Z *= 100.0;

    double R = 0.401288 * X + 0.650173 * Y - 0.051461 * Z;
    double G = -0.250268 * X + 1.204414 * Y + 0.045854 * Z;
    double B = -0.002079 * X + 0.048952 * Y + 0.953127 * Z;

    double Rc = D0 * R, Gc = D1 * G, Bc = D2 * B;
    double sR = Rc >= 0.0 ? 1.0 : -1.0, sG = Gc >= 0.0 ? 1.0 : -1.0, sB = Bc >= 0.0 ? 1.0 : -1.0;
    double fR = std::pow(F_L * std::fabs(Rc) / 100.0, 0.42);
    double fG = std::pow(F_L * std::fabs(Gc) / 100.0, 0.42);
    double fB = std::pow(F_L * std::fabs(Bc) / 100.0, 0.42);
    double Ra = 400.0 * sR * fR / (27.13 + fR) + 0.1;
    double Ga = 400.0 * sG * fG / (27.13 + fG) + 0.1;
    double Ba = 400.0 * sB * fB / (27.13 + fB) + 0.1;

    double a = Ra - 12.0 * Ga / 11.0 + Ba / 11.0;
    double bb = (Ra + Ga - 2.0 * Ba) / 9.0;
    double hrad = std::atan2(bb, a);
    double e_t = 0.25 * (std::cos(hrad + 2.0) + 3.8);

    double A = (2.0 * Ra + Ga + Ba / 20.0 - 0.305) * N_bb;
    double Aratio = A / A_w;
    double sJ = Aratio >= 0.0 ? 1.0 : -1.0;
    double J = 100.0 * sJ * std::pow(std::fabs(Aratio), c_ * z);

    double den = Ra + Ga + 21.0 * Ba / 20.0;
    double t = 0.0;
    if (den != 0.0) t = (50000.0 / 13.0 * N_c * N_cb * e_t * std::sqrt(a * a + bb * bb)) / den;
    double sq = std::sqrt(std::fabs(J) / 100.0);
    double C = (t > 0.0) ? std::pow(t, 0.9) * sq * e_c : 0.0;
    double M = C * inv_FL4;

    double Mp = (1.0 / 0.0228) * std::log(1.0 + 0.0228 * M);
    double Jp = 1.7 * J / (1.0 + 0.007 * J);

    if (lc_active) {
        double Ln = Jp / L_white;
        if (Ln > lc_threshold) {
            double lsc = lc_limit - lc_threshold;
            double lx = (Ln - lc_threshold) / lsc;
            double ly = lx / std::pow(1.0 + std::pow(lx, lc_power), 1.0 / lc_power);
            Ln = lc_threshold + lsc * ly;
        }
        Jp = Ln * L_white;
    }

    double Lc = std::min(std::max(Jp, L_grid0), L_grid1);
    double Li = (Lc - L_grid0) / (L_grid1 - L_grid0) * double(nL - 1);
    int l0 = int(std::floor(Li));
    l0 = std::min(std::max(l0, 0), int(nL) - 2);
    double lf = Li - double(l0);
    double hi_ = (hrad - h_grid0) / h_step;
    double hfl = std::floor(hi_);
    int nhi = int(nh);
    int h0 = (int(hfl) % nhi + nhi) % nhi;  // Python-style modulo
    int h1 = (h0 + 1) % nhi;
    double hf = hi_ - hfl;
    double c00 = s.cmax[std::size_t(l0) * nhi + h0], c01 = s.cmax[std::size_t(l0) * nhi + h1];
    double c10 = s.cmax[std::size_t(l0 + 1) * nhi + h0], c11 = s.cmax[std::size_t(l0 + 1) * nhi + h1];
    double Cmax = (1.0 - lf) * ((1.0 - hf) * c00 + hf * c01) + lf * ((1.0 - hf) * c10 + hf * c11);
    double safe = Cmax > 1e-9 ? Cmax : 1e-9;

    double d = Mp / safe;
    if (d > threshold) {
        double sc = limit - threshold;
        double xk = (d - threshold) / sc;
        double yk = xk / std::pow(1.0 + std::pow(xk, power_), 1.0 / power_);
        d = threshold + sc * yk;
    }
    double Mp_new = d * safe;

    double M_new = (std::exp(Mp_new * 0.0228) - 1.0) / 0.0228;
    double J_new = Jp / (1.7 - 0.007 * Jp);
    double C_new = M_new / inv_FL4;

    double sJ2 = J_new >= 0.0 ? 1.0 : -1.0;
    double A2 = A_w * sJ2 * std::pow(std::fabs(J_new) / 100.0, 1.0 / (c_ * z));
    double sq2 = std::sqrt(std::fabs(J_new) / 100.0);
    double t2 = (sq2 > 0.0 && C_new > 0.0) ? std::pow(C_new / (sq2 * e_c), 1.0 / 0.9) : 0.0;
    double ca = std::cos(hrad), sa = std::sin(hrad);
    double p2 = A2 / N_bb + 0.305;
    const double p3 = 21.0 / 20.0;
    double a2, b2;
    if (t2 == 0.0) {
        a2 = 0.0;
        b2 = 0.0;
    } else {
        double p1 = ((50000.0 / 13.0) * N_c * N_cb * e_t) / t2;
        if (std::fabs(sa) >= std::fabs(ca)) {
            double p4 = p1 / sa;
            b2 = (p2 * (2.0 + p3) * (460.0 / 1403.0)) /
                 (p4 + (2.0 + p3) * (220.0 / 1403.0) * (ca / sa) - (27.0 / 1403.0) + p3 * (6300.0 / 1403.0));
            a2 = b2 * (ca / sa);
        } else {
            double p5 = p1 / ca;
            a2 = (p2 * (2.0 + p3) * (460.0 / 1403.0)) /
                 (p5 + (2.0 + p3) * (220.0 / 1403.0) - ((27.0 / 1403.0) - p3 * (6300.0 / 1403.0)) * (sa / ca));
            b2 = a2 * (sa / ca);
        }
    }
    double Ra2 = (460.0 * p2 + 451.0 * a2 + 288.0 * b2) / 1403.0;
    double Ga2 = (460.0 * p2 - 891.0 * a2 - 261.0 * b2) / 1403.0;
    double Ba2 = (460.0 * p2 - 220.0 * a2 - 6300.0 * b2) / 1403.0;

    double vm, sv, base_;
    vm = Ra2 - 0.1;
    sv = vm >= 0.0 ? 1.0 : -1.0;
    base_ = (std::fabs(vm) < 400.0) ? (27.13 * std::fabs(vm)) / (400.0 - std::fabs(vm)) : 0.0;
    double Rf = (100.0 / F_L) * sv * std::pow(base_, 1.0 / 0.42) / D0;
    vm = Ga2 - 0.1;
    sv = vm >= 0.0 ? 1.0 : -1.0;
    base_ = (std::fabs(vm) < 400.0) ? (27.13 * std::fabs(vm)) / (400.0 - std::fabs(vm)) : 0.0;
    double Gf = (100.0 / F_L) * sv * std::pow(base_, 1.0 / 0.42) / D1;
    vm = Ba2 - 0.1;
    sv = vm >= 0.0 ? 1.0 : -1.0;
    base_ = (std::fabs(vm) < 400.0) ? (27.13 * std::fabs(vm)) / (400.0 - std::fabs(vm)) : 0.0;
    double Bf = (100.0 / F_L) * sv * std::pow(base_, 1.0 / 0.42) / D2;

    double Xn = 1.86206786 * Rf - 1.01125463 * Gf + 0.14918677 * Bf;
    double Yn = 0.38752654 * Rf + 0.62144744 * Gf - 0.00897398 * Bf;
    double Zn = -0.01584150 * Rf - 0.03412294 * Gf + 1.04996444 * Bf;
    Xn /= 100.0; Yn /= 100.0; Zn /= 100.0;

    return apply9(s.m_to_rgb, {Xn, Yn, Zn});
}

std::array<double, 3> transform_pixel(const std::array<double, 3>& encoded,
                                      const OutputTransformData& setup) {
    const OutputTransformData& s = require_setup(setup);
    // 1. the working space's own curve, off
    std::array<double, 3> v = {cctf_decode(encoded[0], s.source_cctf_mode),
                               cctf_decode(encoded[1], s.source_cctf_mode),
                               cctf_decode(encoded[2], s.source_cctf_mode)};
    // 2. into the target, linearly, chromatically adapted
    std::array<double, 3> lin = apply9(s.matrix, v);
    // 3. CAM16-UCS into the target's gamut, rolling rather than cutting
    if (s.algorithm != "off") lin = cam16ucs_into(lin, s);
    // 4. the target's curve, on
    return {cctf_encode(lin[0], s.target_cctf_mode),
            cctf_encode(lin[1], s.target_cctf_mode),
            cctf_encode(lin[2], s.target_cctf_mode)};
}

std::uint16_t to16(double v) {
    if (!(v == v)) return 0;
    return std::uint16_t(std::clamp(v, 0.0, 1.0) * 65535.0 + 0.5);
}

std::uint8_t to8(double v) {
    if (!(v == v)) return 0;
    return std::uint8_t(std::clamp(v, 0.0, 1.0) * 255.0 + 0.5);
}

void apply_rgba16(std::vector<std::uint16_t>& rgba, int width, int height,
                  const OutputTransformData& setup) {
    if (width <= 0 || height <= 0 || rgba.size() != std::size_t(width) * std::size_t(height) * 4)
        throw std::runtime_error("the output transform's picture has invalid storage");
    for (std::size_t i = 0; i < rgba.size(); i += 4) {
        const std::array<double, 3> e = transform_pixel({double(rgba[i]) / 65535.0,
                                                         double(rgba[i + 1]) / 65535.0,
                                                         double(rgba[i + 2]) / 65535.0},
                                                        setup);
        rgba[i] = to16(e[0]);
        rgba[i + 1] = to16(e[1]);
        rgba[i + 2] = to16(e[2]);
    }
}

QImage apply_rgba64(const QImage& frame, const OutputTransformData& setup) {
    if (frame.format() != QImage::Format_RGBA64 || frame.isNull())
        throw std::runtime_error("the output transform needs an RGBA16 picture");
    const int width = frame.width(), height = frame.height();
    std::vector<std::uint16_t> rgba(std::size_t(width) * std::size_t(height) * 4);
    for (int y = 0; y < height; ++y)
        std::memcpy(rgba.data() + std::size_t(y) * width * 4, frame.constScanLine(y),
                    std::size_t(width) * 8);
    apply_rgba16(rgba, width, height, setup);
    QImage out(width, height, QImage::Format_RGBA64);
    if (out.isNull()) throw std::runtime_error("Cannot allocate the transformed picture");
    for (int y = 0; y < height; ++y)
        std::memcpy(out.scanLine(y), rgba.data() + std::size_t(y) * width * 4, std::size_t(width) * 8);
    return out;
}

QImage apply_rgb888(const QImage& frame, const OutputTransformData& setup) {
    if (frame.format() != QImage::Format_RGB888 || frame.isNull())
        throw std::runtime_error("the output transform needs RGB888 pixels");
    const int width = frame.width(), height = frame.height();
    QImage out(width, height, QImage::Format_RGB888);
    if (out.isNull()) throw std::runtime_error("Cannot allocate the transformed picture");
    for (int y = 0; y < height; ++y) {
        const auto* src = frame.constScanLine(y);
        auto* dst = out.scanLine(y);
        for (int x = 0; x < width; ++x) {
            const std::array<double, 3> e = transform_pixel({src[x * 3] / 255.0,
                                                             src[x * 3 + 1] / 255.0,
                                                             src[x * 3 + 2] / 255.0},
                                                            setup);
            dst[x * 3] = char(to8(e[0]));
            dst[x * 3 + 1] = char(to8(e[1]));
            dst[x * 3 + 2] = char(to8(e[2]));
        }
    }
    return out;
}

}  // namespace spk::desktop::colour
