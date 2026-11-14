// The white-balance math behind the Qt editor's 温度/色调 row is pure
// (engine/host/white_balance.hpp): Adobe DNG Robertson isotemperature
// mapping, its inverse, and the as-shot illuminant solved through the
// camera's XYZ matrix. No RAW file, GPU or engine session is involved --
// LibRaw only supplies the structure the math reads, from a synthetic
// camera defined here.
//
// Tolerances: the Robertson mapping is piecewise-linear in mired along
// isotherms whose slopes change between table entries, so the inverse is
// not the algebraic inverse of the forward mapping (the DNG SDK's own
// dng_temperature pair has the same property -- this implementation mirrors
// it, see white_balance.hpp). The bounds below are this implementation's
// measured round-trip error over a K=[2000,50000] x tint=[-150,150] grid,
// with margin: at Robertson table points (K = 1e6/mired for a tabulated
// mired, e.g. 4000 K and 8000 K) the round trip is exact to ~5e-12 K, away
// from them within ~0.35 K and ~0.0015 tint, and the reconstructed
// chromaticity re-forwards onto itself within 1.6e-5. A regression in the
// table itself, or a re-derivation that "fixes" the asymmetry into a
// different scale, fails these bounds.
#include "white_balance.hpp"

#include <libraw/libraw.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(const bool passed, const std::string& name) {
    std::printf("%s %s\n", passed ? "PASS" : "FAIL", name.c_str());
    if (!passed) ++failures;
}

// Point-tagged names keep a failure attributable (the sweep runs the same
// checks 84 times).
std::string at(const char* what, const double kelvin, const double tint) {
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%s [%.0fK %+g]", what, kelvin, tint);
    return buffer;
}

constexpr double kKelvinTolerance = 0.5;       // away from table points
constexpr double kTintTolerance = 0.003;
constexpr double kChromaticityTolerance = 3e-5;
constexpr double kExactTolerance = 1e-9;       // at Robertson table points
constexpr double kMultiplierTolerance = 5e-4;  // as-shot multiplier recovery

// sRGB's D65 RGB->XYZ matrix, inverted: `cam_xyz` maps XYZ to camera RGB
// (LibRaw's convention -- the DNG ColorMatrix for D65), so the camera's
// neutral recording of an illuminant maps back onto that illuminant exactly
// and multipliers_for / as_shot_temperature_tint are exact inverses of each
// other except for the piecewise-linear xy round trip above.
constexpr double kRgbToXyz[9] = {0.4124, 0.3576, 0.1805,
                                 0.2126, 0.7152, 0.0722,
                                 0.0193, 0.1192, 0.9505};

void makeSyntheticCamera(LibRaw& raw) {
    raw.imgdata.idata.colors = 3;
    const auto& m = kRgbToXyz;
    const double det = m[0] * (m[4] * m[8] - m[5] * m[7]) -
                       m[1] * (m[3] * m[8] - m[5] * m[6]) +
                       m[2] * (m[3] * m[7] - m[4] * m[6]);
    double inv[9];
    inv[0] = (m[4] * m[8] - m[5] * m[7]) / det;
    inv[1] = (m[2] * m[7] - m[1] * m[8]) / det;
    inv[2] = (m[1] * m[5] - m[2] * m[4]) / det;
    inv[3] = (m[5] * m[6] - m[3] * m[8]) / det;
    inv[4] = (m[0] * m[8] - m[2] * m[6]) / det;
    inv[5] = (m[2] * m[3] - m[0] * m[5]) / det;
    inv[6] = (m[3] * m[7] - m[4] * m[6]) / det;
    inv[7] = (m[1] * m[6] - m[0] * m[7]) / det;
    inv[8] = (m[0] * m[4] - m[1] * m[3]) / det;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            raw.imgdata.color.cam_xyz[r][c] = static_cast<float>(inv[r * 3 + c]);
}

// The five points the Qt UI sits on: the preset temperatures (日光/阴天/阴影/
// 钨丝灯/荧光灯/闪光灯 collapse onto these five Kelvin values) and their
// tints. `exact` marks the Robertson table points, where the round trip
// must be exact rather than merely bounded.
struct WbPoint {
    double kelvin, tint;
    bool exact;
};
constexpr WbPoint kUiPoints[] = {
    {6500, 0, false},   // 日光
    {3200, 0, false},   // 钨丝灯
    {8000, 20, true},   // 阴影 (mired 125 -- a table point)
    {5500, 10, false},  // 闪光灯
    {4000, 20, true},   // 荧光灯 (mired 250 -- a table point)
};

void roundTripKelvinTint(const double kelvin, const double tint, const bool exact) {
    // The mapping clamps out-of-range Kelvin to the table's span rather than
    // extrapolating (DNG SDK behaviour), so the round trip is compared
    // against the clamped value a caller effectively asked for.
    const double asked = std::clamp(kelvin, 1700.0, 50000.0);
    double x = 0, y = 0, back_k = 0, back_tint = 0, again_x = 0, again_y = 0;
    spkhost::temperature_tint_to_xy(kelvin, tint, x, y);
    check(std::isfinite(x) && std::isfinite(y) && x > 0 && x < 1 && y > 0 && y < 1,
          at("Forward Temperature/Tint stays inside the chromaticity diagram", asked, tint));
    spkhost::xy_to_temperature_tint(x, y, back_k, back_tint);
    check(std::isfinite(back_k) && std::isfinite(back_tint),
          at("Inverse chromaticity stays finite", asked, tint));
    const bool kelvin_ok = std::abs(back_k - asked) <= (exact ? kExactTolerance : kKelvinTolerance);
    const bool tint_ok = std::abs(back_tint - tint) <= (exact ? kExactTolerance : kTintTolerance);
    check(kelvin_ok && tint_ok, at("Temperature/Tint round trip reproduces its input", asked, tint));
    spkhost::temperature_tint_to_xy(back_k, back_tint, again_x, again_y);
    check(std::abs(again_x - x) <= kChromaticityTolerance &&
              std::abs(again_y - y) <= kChromaticityTolerance,
          at("Re-forwarded chromaticity lands on the original", asked, tint));
}

void asShotRoundTrip(const double kelvin, const double tint) {
    LibRaw raw;
    makeSyntheticCamera(raw);
    std::array<float, 4> mul{};
    std::string error;
    if (!spkhost::multipliers_for(raw, kelvin, tint, mul, error)) {
        check(false, at("Synthetic camera multiplies an in-gamut illuminant", kelvin, tint));
        return;
    }
    // The computed multipliers, treated as the camera's own as-shot record.
    for (int c = 0; c < 4; ++c) raw.imgdata.color.cam_mul[c] = mul[c];
    double as_shot_k = 0, as_shot_tint = 0;
    if (!spkhost::as_shot_temperature_tint(raw, as_shot_k, as_shot_tint)) {
        check(false, at("As-shot illuminant solves from synthetic multipliers", kelvin, tint));
        return;
    }
    // The same piecewise-linear reconstruction error as the xy round trip,
    // plus LibRaw's float32 cam_mul storage, which alone moves the recovered
    // Kelvin by ~3e-4 K at the table points (measured) -- so the exactness
    // the pure xy round trip keeps at 1e-9 does not survive the float
    // record, and the ordinary bounds apply everywhere.
    check(std::abs(as_shot_k - kelvin) <= kKelvinTolerance &&
              std::abs(as_shot_tint - tint) <= kTintTolerance,
          at("As Shot -> custom with the same numbers stays the same illuminant", kelvin, tint));
    // multipliers_for normalises to green 1, as does the as-shot record it
    // solved from, so the recovered set is compared channel for channel.
    std::array<float, 4> recovered{};
    std::string recovered_error;
    if (!spkhost::multipliers_for(raw, as_shot_k, as_shot_tint, recovered, recovered_error)) {
        check(false, at("Recovered as-shot illuminant re-multiplies", kelvin, tint));
        return;
    }
    check(std::abs(double(recovered[0]) - double(mul[0])) <= kMultiplierTolerance &&
              std::abs(double(recovered[2]) - double(mul[2])) <= kMultiplierTolerance,
          at("Backfilled As Shot numbers decode to the as-shot multipliers", kelvin, tint));
}

}  // namespace

int main() {
    for (const auto& point : kUiPoints) roundTripKelvinTint(point.kelvin, point.tint, point.exact);
    for (const auto& point : kUiPoints) asShotRoundTrip(point.kelvin, point.tint);

    // The scale the sliders sweep, sampled coarsely: the same bounds must
    // hold everywhere the UI can reach, not only at the preset points.
    for (const double kelvin : {2000., 2350., 2850., 3200., 4000., 5000.,
                                6500., 8000., 12000., 20000., 35000., 50000.})
        for (const double tint : {-150., -75., -20., 0., 20., 75., 150.})
            roundTripKelvinTint(kelvin, tint, false);

    // The mapping clamps out-of-range Kelvin to the table's span rather than
    // extrapolating (DNG SDK behaviour): the clamped extremes must still
    // round trip onto themselves.
    roundTripKelvinTint(1700, 0, false);
    roundTripKelvinTint(100000, 0, false);

    // A camera without a three-colour XYZ matrix cannot define Temperature/
    // Tint at all: both directions must refuse explicitly, not produce
    // numbers. A fresh LibRaw carries no matrix and no multipliers.
    {
        LibRaw blank;
        std::array<float, 4> mul{};
        std::string error;
        check(!spkhost::multipliers_for(blank, 6500, 0, mul, error) && !error.empty(),
              "A matrix-less camera refuses Temperature/Tint with a reason");
        double kelvin = 0, tint = 0;
        check(!spkhost::as_shot_temperature_tint(blank, kelvin, tint),
              "A matrix-less camera refuses an as-shot illuminant");
    }

    if (failures != 0) {
        std::printf("wb_math FAIL (%d failed checks)\n", failures);
        return 1;
    }
    std::printf("wb_math PASS\n");
    return 0;
}
