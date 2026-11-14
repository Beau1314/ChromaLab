// Layer 2's data contract only: the JSON wire shape must match the desktop's
// shared/adjustments.ts exactly, so an edit saved here still opens on the Mac
// and desktop builds. No RAW decoder, no GPU, no layer math.
#include "Layer2.hpp"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointF>
#include <QStringList>
#include <cmath>
#include <cstdio>
#include <stdexcept>

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

        std::printf("Layer 2 model: %d checks passed; no GPU, no RAW input.\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Layer 2 model FAILED after %d checks: %s\n", checks, error.what());
        return 1;
    }
}
