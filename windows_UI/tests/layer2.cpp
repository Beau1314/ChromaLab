// Layer 2's data contract and math core: the JSON wire shape must match the
// desktop's shared/adjustments.ts exactly, so an edit saved here still opens on
// the Mac and desktop builds, and the arithmetic must match it anchor by
// anchor. No RAW decoder, no GPU.
#include "Layer2.hpp"
#include <QCoreApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointF>
#include <QStringList>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace layer2 = spk::desktop::layer2;
namespace {
int checks = 0;
void check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
void near(double actual, double expected, const char* message) {
    ++checks;
    if (!(std::abs(actual - expected) <= 1e-12)) throw std::runtime_error(message);
}
void nearT(double actual, double expected, double tolerance, const char* message) {
    ++checks;
    if (!(std::abs(actual - expected) <= tolerance)) throw std::runtime_error(message);
}
// The quantisation the two apply paths use: lround × depth, clamped.
int quantize8(double value) {
    const long rounded = std::lround(value * 255.0);
    return static_cast<int>(std::clamp(rounded, 0L, 255L));
}
std::uint16_t quantize16(double value) {
    const long rounded = std::lround(value * 65535.0);
    return static_cast<std::uint16_t>(std::clamp(rounded, 0L, 65535L));
}
// 16 → 8 bit: 65535/257 == 255 exactly, so /257 is the LSB-faithful reduction.
int reduce16To8(std::uint16_t value) {
    const long rounded = std::lround(value / 257.0);
    return static_cast<int>(std::clamp(rounded, 0L, 255L));
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        // JSON roundtrip: encode → decode preserves every field, and the wire
        // carries exactly the keys the desktop's decodeAdjustments reads.
        layer2::Adjustments adjustments;
        adjustments.enabled = false;
        adjustments.temperature = -12.5;
        adjustments.tint = 3.25;
        adjustments.exposure = 0.75;
        adjustments.contrast = -15;
        adjustments.brightness = 7.5;
        adjustments.saturation = -30;
        adjustments.highlights = 22;
        adjustments.shadows = -18;
        adjustments.blackPoint = 5;
        adjustments.whitePoint = 90;
        adjustments.curves.rgb = layer2::Curve{
            {QPointF(0, 0), QPointF(0.5, 0.25), QPointF(1, 1)}};
        adjustments.curves.green = layer2::Curve{{QPointF(0, 0), QPointF(1, 0.5)}};
        adjustments.colorBalance.master = layer2::ColorZone{35, 0.4, 0.1};
        adjustments.colorBalance.shadows = layer2::ColorZone{-90, 0.25, -0.05};
        adjustments.colorBalance.midtones = layer2::ColorZone{120, 0, 0.3};
        adjustments.colorBalance.highlights = layer2::ColorZone{-45, 0.1, 0};
        adjustments.vignette = layer2::Vignette{-60, 35};
        check(!layer2::isNeutral(adjustments), "a fully adjusted state tested neutral");

        const QJsonObject encoded = layer2::encode(adjustments);
        const QStringList keys = encoded.keys();
        const QStringList expectedKeys = {
            QStringLiteral("enabled"), QStringLiteral("temperature"), QStringLiteral("tint"),
            QStringLiteral("exposure"), QStringLiteral("contrast"), QStringLiteral("brightness"),
            QStringLiteral("saturation"), QStringLiteral("highlights"), QStringLiteral("shadows"),
            QStringLiteral("blackPoint"), QStringLiteral("whitePoint"), QStringLiteral("curves"),
            QStringLiteral("colorBalance"), QStringLiteral("vignette")};
        check(keys.size() == expectedKeys.size(),
              "encoded adjustments carried an unexpected key count");
        for (const QString& key : expectedKeys)
            check(encoded.contains(key), "encoded adjustments missed a desktop key");
        const QJsonArray rgbPoints = encoded.value(QLatin1String("curves")).toObject()
                                          .value(QLatin1String("rgb")).toObject()
                                          .value(QLatin1String("points")).toArray();
        check(rgbPoints.size() == 3 && rgbPoints.at(1).isArray()
                  && rgbPoints.at(1).toArray().at(0).toDouble() == 0.5
                  && rgbPoints.at(1).toArray().at(1).toDouble() == 0.25,
              "curve points must encode as [x, y] pairs like the desktop");
        const QJsonObject master = encoded.value(QLatin1String("colorBalance")).toObject()
                                       .value(QLatin1String("master")).toObject();
        check(master.contains(QLatin1String("hue"))
                  && master.contains(QLatin1String("saturation"))
                  && master.contains(QLatin1String("luminance")),
              "colour zones must carry hue/saturation/luminance like the desktop");
        const QJsonObject vignette = encoded.value(QLatin1String("vignette")).toObject();
        check(vignette.contains(QLatin1String("amount"))
                  && vignette.contains(QLatin1String("midpoint")),
              "the vignette must carry amount/midpoint like the desktop");
        check(layer2::decode(encoded) == adjustments, "JSON roundtrip changed the adjustments");

        // Swift encodes CGPoint as {x, y}; the desktop accepts both shapes.
        QJsonObject objectShaped = encoded;
        QJsonObject curves = objectShaped.value(QLatin1String("curves")).toObject();
        QJsonObject rgb = curves.value(QLatin1String("rgb")).toObject();
        QJsonArray points;
        points.append(QJsonArray{0.0, 0.0});
        points.append(QJsonObject{{QLatin1String("x"), 0.5}, {QLatin1String("y"), 0.25}});
        points.append(QJsonArray{1.0, 1.0});
        rgb.insert(QLatin1String("points"), points);
        curves.insert(QLatin1String("rgb"), rgb);
        objectShaped.insert(QLatin1String("curves"), curves);
        const layer2::Adjustments mixedDecoded = layer2::decode(objectShaped);
        check(mixedDecoded == adjustments, "{x, y} curve points decoded differently from [x, y]");
        near(mixedDecoded.curves.rgb.points[1].y(), 0.25, "the {x, y} point lost its value");

        // Missing keys decode to the desktop's defaults, field by field.
        const layer2::Adjustments fallback = layer2::decode(QJsonObject{});
        check(fallback == layer2::Adjustments{}, "an empty document did not decode to the defaults");
        check(fallback.enabled && layer2::isNeutral(fallback),
              "the defaults must be enabled and neutral");
        near(fallback.vignette.midpoint, 50, "the default vignette midpoint drifted");

        QJsonObject partial;
        partial.insert(QLatin1String("exposure"), 1.5);
        partial.insert(QLatin1String("vignette"),
                       QJsonObject{{QLatin1String("amount"), 42}});
        const layer2::Adjustments partialDecoded = layer2::decode(partial);
        check(partialDecoded.exposure == 1.5 && partialDecoded.vignette.amount == 42,
              "present keys were ignored while decoding");
        check(partialDecoded.temperature == 0 && partialDecoded.vignette.midpoint == 50
                  && partialDecoded.enabled
                  && partialDecoded.curves == layer2::identityCurves(),
              "missing keys did not fall back to the defaults");

        // Wrong shapes are tolerated the way the desktop's num() is: default.
        QJsonObject garbage;
        garbage.insert(QLatin1String("temperature"), QLatin1String("not a number"));
        garbage.insert(QLatin1String("enabled"), 1);
        garbage.insert(QLatin1String("colorBalance"), 7);
        garbage.insert(QLatin1String("curves"), QJsonArray{QLatin1String("rgb")});
        check(layer2::decode(garbage) == layer2::Adjustments{},
              "wrong-typed keys broke the defaults");
        QJsonObject shortCurve;
        shortCurve.insert(QLatin1String("curves"),
                          QJsonObject{{QLatin1String("red"),
                                       QJsonObject{{QLatin1String("points"),
                                                    QJsonArray{QJsonArray{0.0, 0.2}}}}}});
        check(layer2::decode(shortCurve).curves.red == layer2::identityCurve(),
              "a curve with fewer than two points did not fall back to identity");

        // Neutrality follows the desktop: hue alone or the vignette midpoint
        // alone are not adjustments.
        layer2::Adjustments hueOnly;
        hueOnly.colorBalance.master.hue = 90;
        hueOnly.vignette.midpoint = 80;
        check(layer2::isNeutral(hueOnly), "hue or midpoint alone counted as an adjustment");
        layer2::Adjustments curveOnly;
        curveOnly.curves.luma = layer2::Curve{
            {QPointF(0, 0), QPointF(0.5, 0.5), QPointF(1, 1)}};
        check(!layer2::isNeutral(curveOnly), "a non-identity curve tested neutral");
        layer2::Adjustments zoneOnly;
        zoneOnly.colorBalance.highlights.saturation = 0.1;
        check(!layer2::isNeutral(zoneOnly), "a colour balance shift tested neutral");

        // ------------------------------------------------------------------
        // Layer 2 math core — the brief's hand-verified anchors, ported from
        // desktop/src/shared/adjustments.ts.
        // ------------------------------------------------------------------

        // Neutral identity: pixel() returns the input untouched (1e-12).
        const layer2::Adjustments neutralAdjustments;
        const layer2::Uniforms neutralUniforms =
            layer2::uniforms(neutralAdjustments, layer2::kMidGreySrgb);
        check(neutralUniforms.enabled && !neutralUniforms.curvesActive,
              "neutral uniforms must be enabled with curves inert");
        near(neutralUniforms.midGrey, layer2::kMidGreySrgb, "uniforms lost midGrey");
        near(neutralUniforms.wbGain[0], 1, "neutral wbGain[0] drifted");
        near(neutralUniforms.exposureGain, 1, "neutral exposureGain drifted");
        near(neutralUniforms.vignetteAmount, 0, "neutral vignetteAmount drifted");
        {
            const std::array<double, 3> out =
                layer2::pixel({0.5, 0.35, 0.2}, neutralUniforms, nullptr, 0.5, 0.5);
            near(out[0], 0.5, "a neutral pixel moved red");
            near(out[1], 0.35, "a neutral pixel moved green");
            near(out[2], 0.2, "a neutral pixel moved blue");
        }
        // A disabled layer is a pass-through everywhere.
        {
            layer2::Adjustments off;
            off.exposure = 3;
            off.vignette.amount = 100;
            off.enabled = false;
            const layer2::Uniforms offUniforms = layer2::uniforms(off, layer2::kMidGreySrgb);
            check(!offUniforms.enabled, "disabled uniforms still reported enabled");
            const std::array<double, 3> out =
                layer2::pixel({0.5, 0.35, 0.2}, offUniforms, nullptr, 1, 1);
            near(out[0], 0.5, "a disabled pixel moved red");
            near(out[1], 0.35, "a disabled pixel moved green");
            near(out[2], 0.2, "a disabled pixel moved blue");
        }

        // White balance: temperature 50 warms to [1.09, 1, 0.91].
        layer2::Adjustments warm;
        warm.temperature = 50;
        const layer2::Uniforms warmUniforms = layer2::uniforms(warm, layer2::kMidGreySrgb);
        near(warmUniforms.wbGain[0], 1.09, "warm wbGain[0] drifted");
        near(warmUniforms.wbGain[1], 1.0, "warm wbGain[1] drifted");
        near(warmUniforms.wbGain[2], 0.91, "warm wbGain[2] drifted");
        {
            const std::array<double, 3> out =
                layer2::pixel({0.5, 0.5, 0.5}, warmUniforms, nullptr, 0.5, 0.5);
            near(out[0], 0.545, "the warm pixel's red drifted");
            near(out[1], 0.5, "the warm pixel's green drifted");
            near(out[2], 0.455, "the warm pixel's blue drifted");
        }

        // Exposure +1: 0.5 → 0.5 × 2^(1/2.2).
        layer2::Adjustments brighter;
        brighter.exposure = 1;
        const layer2::Uniforms exposureUniforms =
            layer2::uniforms(brighter, layer2::kMidGreySrgb);
        near(exposureUniforms.exposureGain, 2.0, "the exposure gain drifted");
        {
            const std::array<double, 3> out =
                layer2::pixel({0.5, 0.5, 0.5}, exposureUniforms, nullptr, 0.5, 0.5);
            const double expected = 0.5 * std::pow(2.0, 1.0 / 2.2);
            near(out[0], expected, "the exposure pixel's red drifted");
            near(out[1], expected, "the exposure pixel's green drifted");
            near(out[2], expected, "the exposure pixel's blue drifted");
        }

        // Contrast +25: (0.5 − 0.4614) × 1.3 + 0.4614 = 0.51158.
        layer2::Adjustments punchy;
        punchy.contrast = 25;
        const layer2::Uniforms contrastUniforms =
            layer2::uniforms(punchy, layer2::kMidGreySrgb);
        near(contrastUniforms.contrast, 0.25, "the contrast uniform drifted");
        {
            const std::array<double, 3> out =
                layer2::pixel({0.5, 0.5, 0.5}, contrastUniforms, nullptr, 0.5, 0.5);
            const double expected = (0.5 - layer2::kMidGreySrgb) * 1.3 + layer2::kMidGreySrgb;
            near(out[0], 0.51158, "the contrast pixel's red drifted");
            near(out[1], 0.51158, "the contrast pixel's green drifted");
            near(out[2], 0.51158, "the contrast pixel's blue drifted");
            near(expected, 0.51158, "the contrast anchor formula drifted from 0.51158");
        }

        // Saturation −100: every channel collapses to Rec.709 luma.
        layer2::Adjustments monochrome;
        monochrome.saturation = -100;
        const layer2::Uniforms monoUniforms =
            layer2::uniforms(monochrome, layer2::kMidGreySrgb);
        near(monoUniforms.saturation, 0.0, "the saturation uniform drifted");
        {
            const std::array<double, 3> out =
                layer2::pixel({0.6, 0.3, 0.3}, monoUniforms, nullptr, 0.5, 0.5);
            const double expected = 0.2126 * 0.6 + 0.7152 * 0.3 + 0.0722 * 0.3;
            near(out[0], expected, "the monochrome pixel's red drifted");
            near(out[1], expected, "the monochrome pixel's green drifted");
            near(out[2], expected, "the monochrome pixel's blue drifted");
        }

        // Black/white point 10/90: the span floor 0.05 clamps, 0.5 → 1.0.
        layer2::Adjustments crushed;
        crushed.blackPoint = 10;
        crushed.whitePoint = 90;
        const layer2::Uniforms crushedUniforms =
            layer2::uniforms(crushed, layer2::kMidGreySrgb);
        near(crushedUniforms.blackPoint, 0.1, "the blackPoint uniform drifted");
        near(crushedUniforms.whitePoint, 0.9, "the whitePoint uniform drifted");
        {
            const std::array<double, 3> out =
                layer2::pixel({0.5, 0.5, 0.5}, crushedUniforms, nullptr, 0.5, 0.5);
            near(out[0], 1.0, "the crushed pixel's red must clip to 1.0");
            near(out[1], 1.0, "the crushed pixel's green must clip to 1.0");
            near(out[2], 1.0, "the crushed pixel's blue must clip to 1.0");
        }

        // Fritsch–Carlson hand anchor: {(0,0),(0.5,0.25),(1,1)} at 0.25 → 0.09375.
        const layer2::Curve bent{{QPointF(0, 0), QPointF(0.5, 0.25), QPointF(1, 1)}};
        near(layer2::evaluateCurve(bent, 0.25), 0.09375, "the Fritsch-Carlson anchor drifted");
        near(layer2::evaluateCurve(bent, 0.0), 0.0, "the curve start drifted");
        near(layer2::evaluateCurve(bent, 1.0), 1.0, "the curve end drifted");
        near(layer2::evaluateCurve(bent, -0.5), 0.0, "a curve must clamp below its domain");
        near(layer2::evaluateCurve(bent, 1.5), 1.0, "a curve must clamp above its domain");
        {
            const layer2::Curve degenerate{{{QPointF(0.2, 0.8)}}};
            near(layer2::evaluateCurve(degenerate, 0.7), 0.7,
                 "a curve with fewer than two points must evaluate to x");
        }

        // Curve tables: 256×5 floats, identity in every row; the row order is
        // rgb/luma/red/green/blue (row 0 = rgb @ 64 → 64/255). float32 storage
        // gets a 1e-6 tolerance, the table quantum.
        {
            const std::vector<float> table = layer2::curveTables(layer2::identityCurves());
            check(table.size() == 256u * 5u, "a curve table must be 256×5 floats");
            bool identity = true;
            for (int row = 0; row < 5 && identity; ++row)
                for (int i = 0; i < 256; ++i)
                    if (std::abs(static_cast<double>(table[row * 256 + i]) - i / 255.0) > 1e-6) {
                        identity = false;
                        break;
                    }
            check(identity, "identity curves must produce an all-identity table");
            nearT(static_cast<double>(table[64]), 64 / 255.0, 1e-6,
                  "row 0 (rgb) at 64 must be 64/255");
        }
        {
            // Row order: only the red curve bends, and only row 2 follows it.
            layer2::CurveSet redBent = layer2::identityCurves();
            redBent.red = bent;
            const std::vector<float> table = layer2::curveTables(redBent);
            for (const int i : {0, 64, 128, 200, 255}) {
                nearT(static_cast<double>(table[2 * 256 + i]),
                      layer2::evaluateCurve(bent, i / 255.0), 1e-6,
                      "row 2 must follow the red curve");
                nearT(static_cast<double>(table[0 * 256 + i]), i / 255.0, 1e-6,
                      "row 0 (rgb) must stay identity when only red bends");
            }
        }

        // Point editing: insert reports the index; endpoints lock x; inner
        // points clamp inside their neighbours ±0.01; endpoints and 2-point
        // curves refuse removal; out-of-range moves do nothing.
        {
            layer2::Curve curve = layer2::identityCurve();
            const int inserted = layer2::insertPoint(curve, 0.5, 0.25);
            check(inserted == 1 && curve.points.size() == 3,
                  "insertPoint must report the new index");
            near(curve.points[1].x(), 0.5, "insertPoint lost x");
            near(curve.points[1].y(), 0.25, "insertPoint lost y");
            layer2::movePoint(curve, 0, 0.3, 0.2);
            near(curve.points[0].x(), 0.0, "the first point's x must stay locked at 0");
            near(curve.points[0].y(), 0.2, "the first point's y must move");
            layer2::movePoint(curve, 2, 0.4, 0.7);
            near(curve.points[2].x(), 1.0, "the last point's x must stay locked at 1");
            near(curve.points[2].y(), 0.7, "the last point's y must move");
            layer2::movePoint(curve, 1, 1.5, 2.0);
            near(curve.points[1].x(), 0.99, "an inner point must clamp below its right neighbour");
            near(curve.points[1].y(), 1.0, "an inner point's y must clamp to 1");
            layer2::movePoint(curve, 1, -0.5, -1.0);
            near(curve.points[1].x(), 0.01, "an inner point must clamp above its left neighbour");
            near(curve.points[1].y(), 0.0, "an inner point's y must clamp to 0");
            layer2::movePoint(curve, 7, 0.5, 0.5);
            near(curve.points[1].x(), 0.01, "an out-of-range move must not touch the curve");
            layer2::removePoint(curve, 1);
            check(curve.points.size() == 2 && curve.points[1].x() == 1,
                  "an inner point must be removable");
            layer2::Curve twoPoint = layer2::identityCurve();
            layer2::removePoint(twoPoint, 0);
            layer2::removePoint(twoPoint, 1);
            check(twoPoint == layer2::identityCurve(),
                  "the endpoints of a two-point curve must refuse removal");
            const layer2::Curve single{{{QPointF(0.5, 0.5)}}};
            layer2::Curve mutableSingle = single;
            layer2::removePoint(mutableSingle, 0);
            check(mutableSingle == single, "a curve with fewer than two points must refuse removal");
        }

        // Vignette amount 100 / midpoint 50 (print-space sx/sy): the centre is
        // untouched; the corner falls to
        // 0.5 × (1 + 0.9 × smoothstep(0.6, 1.5, √2)) = 0.93851389734130031.
        layer2::Adjustments darkened;
        darkened.vignette.amount = 100;
        const layer2::Uniforms vigUniforms = layer2::uniforms(darkened, layer2::kMidGreySrgb);
        near(vigUniforms.vignetteAmount, 1.0, "the vignette amount uniform drifted");
        near(vigUniforms.vignetteMidpoint, 0.5, "the vignette midpoint uniform drifted");
        {
            const std::array<double, 3> centre =
                layer2::pixel({0.5, 0.5, 0.5}, vigUniforms, nullptr, 0.5, 0.5);
            near(centre[0], 0.5, "the vignette must leave the centre untouched (red)");
            near(centre[1], 0.5, "the vignette must leave the centre untouched (green)");
            near(centre[2], 0.5, "the vignette must leave the centre untouched (blue)");
            const std::array<double, 3> corner =
                layer2::pixel({0.5, 0.5, 0.5}, vigUniforms, nullptr, 1.0, 1.0);
            nearT(corner[0], 0.93851389734130031, 1e-9, "the vignette corner drifted (red)");
            nearT(corner[1], 0.93851389734130031, 1e-9, "the vignette corner drifted (green)");
            nearT(corner[2], 0.93851389734130031, 1e-9, "the vignette corner drifted (blue)");
        }

        // applyImage on a non-power-of-two size (20 rows → the parallel row
        // blocks): every pixel must equal the pixel() formula within ±1 LSB,
        // and each pixel's alpha must pass through untouched.
        {
            layer2::Adjustments graded;
            graded.temperature = -20;
            graded.tint = 10;
            graded.exposure = 0.3;
            graded.contrast = -12;
            graded.brightness = 8;
            graded.shadows = -25;
            graded.highlights = 18;
            graded.blackPoint = 3;
            graded.whitePoint = 95;
            graded.saturation = 35;
            graded.curves.rgb = bent;
            graded.curves.blue = layer2::Curve{{QPointF(0, 0), QPointF(1, 0.5)}};
            graded.colorBalance.midtones = layer2::ColorZone{120, 0.3, 0.15};
            graded.vignette.amount = -45;
            graded.vignette.midpoint = 40;
            const int width = 5;
            const int height = 20;
            QImage image(width, height, QImage::Format_ARGB32);
            std::vector<std::array<double, 3>> input(static_cast<std::size_t>(width) * height);
            std::vector<std::array<int, 3>> expected(static_cast<std::size_t>(width) * height);
            std::vector<unsigned> alphas(static_cast<std::size_t>(width) * height);
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    const double r = 0.1 + 0.8 * ((x * 3 + y * 7) % 10) / 10.0;
                    const double g = 0.2 + 0.6 * ((x * 5 + y * 3) % 10) / 10.0;
                    const double b = 0.05 + 0.9 * ((x * 7 + y * 2) % 10) / 10.0;
                    // One translucent pixel documents alpha pass-through.
                    const unsigned alpha = (x == 2 && y == 7) ? 0x80u : 0xffu;
                    const std::size_t index = static_cast<std::size_t>(y) * width + x;
                    image.setPixel(x, y, qRgba(quantize8(r), quantize8(g), quantize8(b), int(alpha)));
                    input[index] = {quantize8(r) / 255.0, quantize8(g) / 255.0, quantize8(b) / 255.0};
                    alphas[index] = alpha;
                }
            }
            const layer2::Uniforms gradedUniforms =
                layer2::uniforms(graded, layer2::kMidGreySrgb);
            check(gradedUniforms.curvesActive, "the graded state must report active curves");
            const std::vector<float> gradedTables = layer2::curveTables(graded.curves);
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x) {
                    const std::size_t index = static_cast<std::size_t>(y) * width + x;
                    const std::array<double, 3> out = layer2::pixel(
                        input[index], gradedUniforms, gradedTables.data(),
                        (x + 0.5) / width, (y + 0.5) / height);
                    expected[index] = {quantize8(out[0]), quantize8(out[1]), quantize8(out[2])};
                }
            layer2::applyImage(image, graded);
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x) {
                    const std::size_t index = static_cast<std::size_t>(y) * width + x;
                    const QRgb got = image.pixel(x, y);
                    check(qAlpha(got) == alphas[index], "applyImage must preserve alpha");
                    check(std::abs(qRed(got) - expected[index][0]) <= 1,
                          "applyImage red drifted from the pixel() formula");
                    check(std::abs(qGreen(got) - expected[index][1]) <= 1,
                          "applyImage green drifted from the pixel() formula");
                    check(std::abs(qBlue(got) - expected[index][2]) <= 1,
                          "applyImage blue drifted from the pixel() formula");
                }
        }

        // The 16-bit path and the 8-bit path quantify the same signal within
        // 2 LSB @ 8 bit.
        {
            layer2::Adjustments mixed;
            mixed.temperature = 30;
            mixed.exposure = 0.5;
            mixed.contrast = 20;
            mixed.shadows = 15;
            mixed.saturation = -20;
            mixed.vignette.amount = 40;
            const int width = 4;
            const int height = 3;
            QImage image(width, height, QImage::Format_ARGB32);
            std::vector<std::uint16_t> rgba16(static_cast<std::size_t>(width) * height * 4,
                                              0xffffu);
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x) {
                    const std::array<double, 3> signal = {
                        std::clamp(0.12 + 0.22 * x + 0.09 * y, 0.0, 1.0),
                        std::clamp(0.55 - 0.14 * x + 0.06 * y, 0.0, 1.0),
                        std::clamp(0.31 + 0.17 * y - 0.08 * x, 0.0, 1.0)};
                    image.setPixel(x, y, qRgb(quantize8(signal[0]), quantize8(signal[1]),
                                              quantize8(signal[2])));
                    std::uint16_t* sample =
                        rgba16.data() + (static_cast<std::size_t>(y) * width + x) * 4;
                    sample[0] = quantize16(signal[0]);
                    sample[1] = quantize16(signal[1]);
                    sample[2] = quantize16(signal[2]);
                    sample[3] = 0xffffu;
                }
            layer2::applyImage(image, mixed);
            layer2::applyRgba16(rgba16, width, height, mixed);
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x) {
                    const QRgb got = image.pixel(x, y);
                    const std::uint16_t* sample =
                        rgba16.data() + (static_cast<std::size_t>(y) * width + x) * 4;
                    check(std::abs(qRed(got) - reduce16To8(sample[0])) <= 2,
                          "the 16-bit red drifted more than 2 LSB from the 8-bit path");
                    check(std::abs(qGreen(got) - reduce16To8(sample[1])) <= 2,
                          "the 16-bit green drifted more than 2 LSB from the 8-bit path");
                    check(std::abs(qBlue(got) - reduce16To8(sample[2])) <= 2,
                          "the 16-bit blue drifted more than 2 LSB from the 8-bit path");
                }
        }

        // A disabled layer leaves both buffers untouched.
        {
            layer2::Adjustments off;
            off.exposure = 2;
            off.vignette.amount = 80;
            off.enabled = false;
            QImage image(3, 2, QImage::Format_ARGB32);
            image.fill(qRgb(90, 120, 150));
            std::vector<std::uint16_t> rgba16(static_cast<std::size_t>(3) * 2 * 4, 0x0000u);
            layer2::applyImage(image, off);
            layer2::applyRgba16(rgba16, 3, 2, off);
            check(image.pixel(0, 0) == qRgb(90, 120, 150) && image.pixel(2, 1) == qRgb(90, 120, 150),
                  "applyImage must not touch a disabled layer");
            bool untouched = true;
            for (const std::uint16_t sample : rgba16)
                if (sample != 0) untouched = false;
            check(untouched, "applyRgba16 must not touch a disabled layer");
        }

        std::printf("Layer 2 model + math: %d checks passed; no GPU, no RAW input.\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Layer 2 model FAILED after %d checks: %s\n", checks, error.what());
        return 1;
    }
}
