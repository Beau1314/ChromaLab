#include "Layer2.hpp"

#include <QJsonArray>
#include <QJsonValue>
#include <cmath>

namespace spk::desktop::layer2 {
namespace {

// adjustments.ts num(): only a finite number survives, otherwise the default.
double number(const QJsonValue& value, double fallback) {
    if (!value.isDouble()) return fallback;
    const double parsed = value.toDouble();
    return std::isfinite(parsed) ? parsed : fallback;
}

QJsonObject encodeCurve(const Curve& curve) {
    QJsonArray points;
    for (const QPointF& point : curve.points)
        points.append(QJsonArray{point.x(), point.y()});
    return QJsonObject{{QLatin1String("points"), points}};
}

QJsonObject encodeZone(const ColorZone& zone) {
    return QJsonObject{{QLatin1String("hue"), zone.hue},
                       {QLatin1String("saturation"), zone.saturation},
                       {QLatin1String("luminance"), zone.luminance}};
}

Curve decodeCurve(const QJsonValue& value) {
    const QJsonArray raw = value.toObject().value(QLatin1String("points")).toArray();
    if (raw.size() < 2) return identityCurve();
    Curve curve;
    for (const QJsonValue& item : raw) {
        // Swift encodes CGPoint as [x, y]; accept {x, y} too. An array of the
        // wrong arity reads undefined x/y, exactly as the desktop does.
        if (item.isArray()) {
            const QJsonArray pair = item.toArray();
            if (pair.size() == 2)
                curve.points.emplace_back(number(pair.at(0), 0), number(pair.at(1), 0));
            else curve.points.emplace_back(0, 0);
        } else if (item.isObject()) {
            const QJsonObject point = item.toObject();
            curve.points.emplace_back(number(point.value(QLatin1String("x")), 0),
                                      number(point.value(QLatin1String("y")), 0));
        }
        // null, numbers, strings and booleans are not points: skipped.
    }
    return curve.points.size() >= 2 ? curve : identityCurve();
}

ColorZone decodeZone(const QJsonValue& value) {
    const QJsonObject zone = value.toObject();
    return ColorZone{number(zone.value(QLatin1String("hue")), 0),
                     number(zone.value(QLatin1String("saturation")), 0),
                     number(zone.value(QLatin1String("luminance")), 0)};
}

} // namespace

Curve identityCurve() {
    return Curve{std::vector<QPointF>{QPointF(0, 0), QPointF(1, 1)}};
}

CurveSet identityCurves() {
    return CurveSet{identityCurve(), identityCurve(), identityCurve(), identityCurve(),
                    identityCurve()};
}

bool isNeutral(const Adjustments& adjustments) {
    const auto curveIsIdentity = [](const Curve& curve) {
        return curve.points.size() == 2 && curve.points[0].x() == 0 && curve.points[0].y() == 0
               && curve.points[1].x() == 1 && curve.points[1].y() == 1;
    };
    const auto zoneIsNeutral = [](const ColorZone& zone) {
        return zone.saturation == 0 && zone.luminance == 0;
    };
    const CurveSet& curves = adjustments.curves;
    const ColorBalance& balance = adjustments.colorBalance;
    return adjustments.temperature == 0 && adjustments.tint == 0 && adjustments.exposure == 0
           && adjustments.contrast == 0 && adjustments.brightness == 0
           && adjustments.saturation == 0 && adjustments.highlights == 0 && adjustments.shadows == 0
           && adjustments.blackPoint == 0 && adjustments.whitePoint == 0
           && curveIsIdentity(curves.rgb) && curveIsIdentity(curves.luma)
           && curveIsIdentity(curves.red) && curveIsIdentity(curves.green)
           && curveIsIdentity(curves.blue) && zoneIsNeutral(balance.master)
           && zoneIsNeutral(balance.shadows) && zoneIsNeutral(balance.midtones)
           && zoneIsNeutral(balance.highlights) && adjustments.vignette.amount == 0;
}

QJsonObject encode(const Adjustments& adjustments) {
    QJsonObject encoded;
    encoded.insert(QLatin1String("enabled"), adjustments.enabled);
    encoded.insert(QLatin1String("temperature"), adjustments.temperature);
    encoded.insert(QLatin1String("tint"), adjustments.tint);
    encoded.insert(QLatin1String("exposure"), adjustments.exposure);
    encoded.insert(QLatin1String("contrast"), adjustments.contrast);
    encoded.insert(QLatin1String("brightness"), adjustments.brightness);
    encoded.insert(QLatin1String("saturation"), adjustments.saturation);
    encoded.insert(QLatin1String("highlights"), adjustments.highlights);
    encoded.insert(QLatin1String("shadows"), adjustments.shadows);
    encoded.insert(QLatin1String("blackPoint"), adjustments.blackPoint);
    encoded.insert(QLatin1String("whitePoint"), adjustments.whitePoint);
    QJsonObject curves;
    curves.insert(QLatin1String("rgb"), encodeCurve(adjustments.curves.rgb));
    curves.insert(QLatin1String("luma"), encodeCurve(adjustments.curves.luma));
    curves.insert(QLatin1String("red"), encodeCurve(adjustments.curves.red));
    curves.insert(QLatin1String("green"), encodeCurve(adjustments.curves.green));
    curves.insert(QLatin1String("blue"), encodeCurve(adjustments.curves.blue));
    encoded.insert(QLatin1String("curves"), curves);
    QJsonObject colorBalance;
    colorBalance.insert(QLatin1String("master"), encodeZone(adjustments.colorBalance.master));
    colorBalance.insert(QLatin1String("shadows"), encodeZone(adjustments.colorBalance.shadows));
    colorBalance.insert(QLatin1String("midtones"), encodeZone(adjustments.colorBalance.midtones));
    colorBalance.insert(QLatin1String("highlights"),
                        encodeZone(adjustments.colorBalance.highlights));
    encoded.insert(QLatin1String("colorBalance"), colorBalance);
    QJsonObject vignette;
    vignette.insert(QLatin1String("amount"), adjustments.vignette.amount);
    vignette.insert(QLatin1String("midpoint"), adjustments.vignette.midpoint);
    encoded.insert(QLatin1String("vignette"), vignette);
    return encoded;
}

Adjustments decode(const QJsonObject& raw) {
    const QJsonObject curves = raw.value(QLatin1String("curves")).toObject();
    const QJsonObject colorBalance = raw.value(QLatin1String("colorBalance")).toObject();
    const QJsonObject vignette = raw.value(QLatin1String("vignette")).toObject();
    const Adjustments defaults; // the desktop's ADJUSTMENTS_DEFAULT
    Adjustments decoded;
    const QJsonValue enabled = raw.value(QLatin1String("enabled"));
    decoded.enabled = enabled.isBool() ? enabled.toBool() : true;
    decoded.temperature = number(raw.value(QLatin1String("temperature")), defaults.temperature);
    decoded.tint = number(raw.value(QLatin1String("tint")), defaults.tint);
    decoded.exposure = number(raw.value(QLatin1String("exposure")), defaults.exposure);
    decoded.contrast = number(raw.value(QLatin1String("contrast")), defaults.contrast);
    decoded.brightness = number(raw.value(QLatin1String("brightness")), defaults.brightness);
    decoded.saturation = number(raw.value(QLatin1String("saturation")), defaults.saturation);
    decoded.highlights = number(raw.value(QLatin1String("highlights")), defaults.highlights);
    decoded.shadows = number(raw.value(QLatin1String("shadows")), defaults.shadows);
    decoded.blackPoint = number(raw.value(QLatin1String("blackPoint")), defaults.blackPoint);
    decoded.whitePoint = number(raw.value(QLatin1String("whitePoint")), defaults.whitePoint);
    decoded.curves = CurveSet{decodeCurve(curves.value(QLatin1String("rgb"))),
                              decodeCurve(curves.value(QLatin1String("luma"))),
                              decodeCurve(curves.value(QLatin1String("red"))),
                              decodeCurve(curves.value(QLatin1String("green"))),
                              decodeCurve(curves.value(QLatin1String("blue")))};
    decoded.colorBalance =
        ColorBalance{decodeZone(colorBalance.value(QLatin1String("master"))),
                     decodeZone(colorBalance.value(QLatin1String("shadows"))),
                     decodeZone(colorBalance.value(QLatin1String("midtones"))),
                     decodeZone(colorBalance.value(QLatin1String("highlights")))};
    decoded.vignette = Vignette{number(vignette.value(QLatin1String("amount")), 0),
                                number(vignette.value(QLatin1String("midpoint")), 50)};
    return decoded;
}

} // namespace spk::desktop::layer2
