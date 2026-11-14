#include "Layer2.hpp"

#include <QColor>
#include <QJsonArray>
#include <QJsonValue>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

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

// --- the layer arithmetic; a line-by-line port of adjustments.ts -------------

namespace {

constexpr double kPi = 3.141592653589793; // adjustments.ts Math.PI

// adjustments.ts sat(): its [0, 1] clamp.
double clamp01(double value) {
    return value < 0 ? 0 : value > 1 ? 1 : value;
}

double luma(double r, double g, double b) {
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

bool curveIsIdentity(const Curve& curve) {
    return curve.points.size() == 2 && curve.points[0].x() == 0 && curve.points[0].y() == 0
           && curve.points[1].x() == 1 && curve.points[1].y() == 1;
}

bool curvesIdentity(const CurveSet& curves) {
    return curveIsIdentity(curves.rgb) && curveIsIdentity(curves.luma)
           && curveIsIdentity(curves.red) && curveIsIdentity(curves.green)
           && curveIsIdentity(curves.blue);
}

// adjustments.ts zoneOffset(): a colour zone's per-channel offset vector.
std::array<double, 3> zoneOffset(const ColorZone& zone) {
    const double h = (zone.hue * kPi) / 180;
    const double s = zone.saturation * 0.25;
    return {std::cos(h) * s, std::cos(h - (2 * kPi) / 3) * s, std::cos(h + (2 * kPi) / 3) * s};
}

// adjustments.ts tonePosition(): a luma's 0…1 position on the tone scale.
double tonePosition(double l, double midGrey) {
    double m = clamp01(midGrey);
    if (m <= 0 || m >= 1) m = 0.5;
    return l <= m ? (0.5 * l) / m : 0.5 + (0.5 * (l - m)) / (1 - m);
}

// adjustments.ts layer2Tone() — steps 1–6. In/out: one pixel, encoded 0…1.
std::array<double, 3> layer2Tone(const std::array<double, 3>& rgb, const Uniforms& u) {
    double r = rgb[0] * u.wbGain[0];
    double g = rgb[1] * u.wbGain[1];
    double b = rgb[2] * u.wbGain[2];
    if (u.exposureGain != 1) {
        r = std::pow(std::pow(std::max(r, 0.0), 2.2) * u.exposureGain, 1 / 2.2);
        g = std::pow(std::pow(std::max(g, 0.0), 2.2) * u.exposureGain, 1 / 2.2);
        b = std::pow(std::pow(std::max(b, 0.0), 2.2) * u.exposureGain, 1 / 2.2);
    }
    if (u.contrast != 0) {
        const double k = 1 + u.contrast * 1.2;
        r = (r - u.midGrey) * k + u.midGrey;
        g = (g - u.midGrey) * k + u.midGrey;
        b = (b - u.midGrey) * k + u.midGrey;
    }
    if (u.brightness != 0) {
        const double e = 1 / (1 + u.brightness * 0.8);
        r = std::pow(std::max(r, 0.0), e);
        g = std::pow(std::max(g, 0.0), e);
        b = std::pow(std::max(b, 0.0), e);
    }
    double l = tonePosition(luma(r, g, b), u.midGrey);
    if (u.shadows != 0) {
        double w = 1 - l;
        w = w * w;
        r += u.shadows * 0.25 * w * (1 - r);
        g += u.shadows * 0.25 * w * (1 - g);
        b += u.shadows * 0.25 * w * (1 - b);
    }
    if (u.highlights != 0) {
        const double w = l * l;
        const double k = u.highlights * 0.25 * w;
        if (u.highlights > 0) {
            r += k * (1 - r);
            g += k * (1 - g);
            b += k * (1 - b);
        } else {
            r += k * r;
            g += k * g;
            b += k * b;
        }
    }
    const double span = std::max(1 - u.blackPoint - u.whitePoint, 0.05);
    r = (r - u.blackPoint) / span;
    g = (g - u.blackPoint) / span;
    b = (b - u.blackPoint) / span;
    l = luma(r, g, b);
    r = l + (r - l) * u.saturation;
    g = l + (g - l) * u.saturation;
    b = l + (b - l) * u.saturation;
    const double ls = clamp01(tonePosition(l, u.midGrey));
    const double wS = (1 - ls) * (1 - ls);
    const double wH = ls * ls;
    const double wM = std::max(1 - wS - wH, 0.0);
    const auto off = [&](std::size_t i) {
        return u.cbMaster[i] + u.cbShadows[i] * wS + u.cbMidtones[i] * wM
               + u.cbHighlights[i] * wH;
    };
    const double lum = 1 + u.cbLum[0] + u.cbLum[1] * wS + u.cbLum[2] * wM + u.cbLum[3] * wH;
    return {(r + off(0)) * lum, (g + off(1)) * lum, (b + off(2)) * lum};
}

// adjustments.ts lookup(): linear between table entries, clamped — the shader's
// linear sampler. f is never negative, so the int cast is Math.floor's.
double lookup(const float* tables, double x, int row) {
    constexpr int kTableSize = 256;
    const double f = clamp01(x) * (kTableSize - 1);
    const int i = static_cast<int>(f);
    const int j = std::min(i + 1, kTableSize - 1);
    const double t = f - i;
    return tables[row * kTableSize + i] * (1 - t) + tables[row * kTableSize + j] * t;
}

double smoothstep(double e0, double e1, double x) {
    const double t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3 - 2 * t);
}

// The quantisation both apply paths share: lround × depth, clamped. The pixel
// arithmetic is pure and finite, so the row work below cannot throw.
int toByte(double value) {
    const long rounded = std::lround(value * 255.0);
    return static_cast<int>(std::clamp(rounded, 0L, 255L));
}

std::uint16_t toShort(double value) {
    const long rounded = std::lround(value * 65535.0);
    return static_cast<std::uint16_t>(std::clamp(rounded, 0L, 65535L));
}

// Rows are graded in contiguous blocks on worker threads; fewer than 16 rows
// run on this thread. No exception may escape a thread (that would terminate
// the process): workers stash the first one and the joining thread rethrows it.
template <typename Fn>
void parallelRows(int rows, Fn&& fn) {
    if (rows <= 0) return;
    int threads = QThread::idealThreadCount();
    threads = std::clamp(threads, 1, 16);
    if (rows < 16 || threads <= 1) {
        for (int y = 0; y < rows; ++y) fn(y);
        return;
    }
    const int block = (rows + threads - 1) / threads;
    std::vector<std::thread> workers;
    std::mutex guard;
    std::exception_ptr failure;
    const auto spawn = [&](int begin, int end) {
        workers.emplace_back([&, begin, end] {
            try {
                for (int y = begin; y < end; ++y) fn(y);
            } catch (...) {
                const std::lock_guard<std::mutex> lock(guard);
                if (!failure) failure = std::current_exception();
            }
        });
    };
    try {
        for (int t = 0; t < threads; ++t) {
            const int begin = t * block;
            if (begin >= rows) break;
            spawn(begin, std::min(begin + block, rows));
        }
    } catch (...) {
        for (std::thread& worker : workers)
            if (worker.joinable()) worker.join();
        throw;
    }
    for (std::thread& worker : workers) worker.join();
    if (failure) std::rethrow_exception(failure);
}

} // namespace

double evaluateCurve(const Curve& curve, double x) {
    const std::vector<QPointF>& p = curve.points;
    const std::size_t n = p.size();
    if (n < 2) return x;
    if (x <= p[0].x()) return p[0].y();
    if (x >= p[n - 1].x()) return p[n - 1].y();
    std::vector<double> d;
    d.reserve(n - 1);
    for (std::size_t i = 0; i + 1 < n; ++i) {
        const double dx = std::max(p[i + 1].x() - p[i].x(), 1e-6);
        d.push_back((p[i + 1].y() - p[i].y()) / dx);
    }
    std::vector<double> m(n, 0.0);
    m[0] = d[0];
    m[n - 1] = d[n - 2];
    for (std::size_t i = 1; i + 1 < n; ++i)
        m[i] = d[i - 1] * d[i] <= 0 ? 0.0 : (d[i - 1] + d[i]) / 2;
    for (std::size_t i = 0; i + 1 < n; ++i) {
        if (d[i] == 0) continue;
        const double a = m[i] / d[i];
        const double b = m[i + 1] / d[i];
        const double s = a * a + b * b;
        if (s > 9) {
            const double t = 3 / std::sqrt(s);
            m[i] = t * a * d[i];
            m[i + 1] = t * b * d[i];
        }
    }
    std::size_t i = 0;
    while (i + 2 < n && x > p[i + 1].x()) ++i;
    const double h = std::max(p[i + 1].x() - p[i].x(), 1e-6);
    const double t = (x - p[i].x()) / h;
    const double t2 = t * t;
    const double t3 = t2 * t;
    const double y = (2 * t3 - 3 * t2 + 1) * p[i].y() + (t3 - 2 * t2 + t) * h * m[i]
                     + (-2 * t3 + 3 * t2) * p[i + 1].y() + (t3 - t2) * h * m[i + 1];
    return std::clamp(y, 0.0, 1.0);
}

std::vector<float> curveTables(const CurveSet& curves) {
    // Five rows of 256: rgb, luma, red, green, blue (the shader's layout).
    constexpr int kTableSize = 256;
    const Curve* const rows[5] = {&curves.rgb, &curves.luma, &curves.red, &curves.green,
                                  &curves.blue};
    std::vector<float> out(static_cast<std::size_t>(kTableSize) * 5);
    for (int row = 0; row < 5; ++row)
        for (int i = 0; i < kTableSize; ++i)
            out[static_cast<std::size_t>(row) * kTableSize + i] =
                static_cast<float>(evaluateCurve(*rows[row], i / (kTableSize - 1.0)));
    return out;
}

int insertPoint(Curve& curve, double x, double y) {
    const QPointF point(std::clamp(x, 0.0, 1.0), std::clamp(y, 0.0, 1.0));
    std::size_t i = 0;
    while (i < curve.points.size() && !(curve.points[i].x() > point.x())) ++i;
    curve.points.insert(curve.points.begin() + static_cast<std::ptrdiff_t>(i), point);
    return static_cast<int>(i);
}

void movePoint(Curve& curve, int index, double x, double y) {
    if (index < 0 || static_cast<std::size_t>(index) >= curve.points.size()) return;
    std::vector<QPointF> points = curve.points;
    double qx = std::clamp(x, 0.0, 1.0);
    const double qy = std::clamp(y, 0.0, 1.0);
    if (index == 0) qx = 0;
    else if (index == static_cast<int>(points.size()) - 1) qx = 1;
    else {
        const double lo = points[static_cast<std::size_t>(index) - 1].x() + 0.01;
        const double hi = points[static_cast<std::size_t>(index) + 1].x() - 0.01;
        qx = std::clamp(qx, std::min(lo, hi), std::max(lo, hi));
    }
    points[static_cast<std::size_t>(index)] = QPointF(qx, qy);
    curve.points = std::move(points);
}

void removePoint(Curve& curve, int index) {
    if (curve.points.size() <= 2 || index <= 0
            || static_cast<std::size_t>(index) >= curve.points.size() - 1) return;
    curve.points.erase(curve.points.begin() + index);
}

Uniforms uniforms(const Adjustments& adjustments, double midGrey) {
    const double t = adjustments.temperature / 100;
    const double g = adjustments.tint / 100;
    const ColorBalance& cb = adjustments.colorBalance;
    Uniforms u{};
    u.wbGain = {1 + 0.18 * t, 1 - 0.12 * g, 1 - 0.18 * t};
    u.exposureGain = std::pow(2.0, adjustments.exposure);
    u.contrast = adjustments.contrast / 100;
    u.brightness = adjustments.brightness / 100;
    u.saturation = 1 + adjustments.saturation / 100;
    u.highlights = adjustments.highlights / 100;
    u.shadows = adjustments.shadows / 100;
    u.blackPoint = adjustments.blackPoint / 100;
    u.whitePoint = adjustments.whitePoint / 100;
    u.cbMaster = zoneOffset(cb.master);
    u.cbShadows = zoneOffset(cb.shadows);
    u.cbMidtones = zoneOffset(cb.midtones);
    u.cbHighlights = zoneOffset(cb.highlights);
    u.cbLum = {cb.master.luminance * 0.25, cb.shadows.luminance * 0.25,
               cb.midtones.luminance * 0.25, cb.highlights.luminance * 0.25};
    u.vignetteAmount = adjustments.vignette.amount / 100;
    u.vignetteMidpoint = adjustments.vignette.midpoint / 100;
    u.midGrey = midGrey;
    u.curvesActive = !curvesIdentity(adjustments.curves);
    u.enabled = adjustments.enabled;
    return u;
}

std::array<double, 3> pixel(const std::array<double, 3>& rgb, const Uniforms& u,
                            const float* tables, double x, double y) {
    if (!u.enabled) return rgb;
    std::array<double, 3> out = layer2Tone(rgb, u);
    if (tables) {
        for (double& channel : out) channel = clamp01(channel);
        const double ly = luma(out[0], out[1], out[2]);
        const double ly2 = lookup(tables, ly, 1);
        const double k = ly > 1e-4 ? ly2 / ly : 1;
        for (double& channel : out) channel = clamp01(channel * k);
        out[0] = lookup(tables, out[0], 0);
        out[1] = lookup(tables, out[1], 0);
        out[2] = lookup(tables, out[2], 0);
        out[0] = lookup(tables, out[0], 2);
        out[1] = lookup(tables, out[1], 3);
        out[2] = lookup(tables, out[2], 4);
    }
    if (u.vignetteAmount != 0) {
        const double d = std::hypot((x - 0.5) * 2, (y - 0.5) * 2);
        const double fall = smoothstep(u.vignetteMidpoint * 1.2, 1.5, d);
        const double k = 1 + u.vignetteAmount * fall * 0.9;
        for (double& channel : out) channel *= k;
    }
    for (double& channel : out) channel = clamp01(channel);
    return out;
}

void applyImage(QImage& image, const Adjustments& adjustments, double midGrey) {
    if (image.format() != QImage::Format_ARGB32)
        image = image.convertToFormat(QImage::Format_ARGB32);
    const Uniforms u = uniforms(adjustments, midGrey);
    if (!u.enabled) return;
    std::vector<float> tableStorage;
    if (u.curvesActive) tableStorage = curveTables(adjustments.curves);
    const float* const tables = tableStorage.empty() ? nullptr : tableStorage.data();
    const int width = image.width();
    const int height = image.height();
    if (width <= 0 || height <= 0) return;
    uchar* const base = image.bits();
    const qsizetype bytesPerLine = image.bytesPerLine();
    const auto gradeRow = [&](int y) {
        QRgb* line = reinterpret_cast<QRgb*>(base + y * bytesPerLine);
        for (int x = 0; x < width; ++x) {
            const QRgb original = line[x];
            const std::array<double, 3> in = {qRed(original) / 255.0, qGreen(original) / 255.0,
                                              qBlue(original) / 255.0};
            const std::array<double, 3> out =
                pixel(in, u, tables, (x + 0.5) / width, (y + 0.5) / height);
            // The layer grades RGB; the pixel's alpha passes through untouched.
            line[x] = (original & 0xff000000u)
                      | (qRgb(toByte(out[0]), toByte(out[1]), toByte(out[2])) & 0x00ffffffu);
        }
    };
    parallelRows(height, gradeRow);
}

void applyRgba16(std::vector<std::uint16_t>& rgba, int width, int height,
                 const Adjustments& adjustments, double midGrey) {
    const Uniforms u = uniforms(adjustments, midGrey);
    if (!u.enabled) return;
    std::vector<float> tableStorage;
    if (u.curvesActive) tableStorage = curveTables(adjustments.curves);
    const float* const tables = tableStorage.empty() ? nullptr : tableStorage.data();
    if (width <= 0 || height <= 0
            || rgba.size()
                   < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4)
        return;
    const auto gradeRow = [&](int y) {
        std::uint16_t* line = rgba.data() + static_cast<std::size_t>(y) * width * 4;
        for (int x = 0; x < width; ++x) {
            std::uint16_t* sample = line + static_cast<std::size_t>(x) * 4;
            const std::array<double, 3> in = {sample[0] / 65535.0, sample[1] / 65535.0,
                                              sample[2] / 65535.0};
            const std::array<double, 3> out =
                pixel(in, u, tables, (x + 0.5) / width, (y + 0.5) / height);
            // RGBA order; the alpha sample passes through untouched.
            sample[0] = toShort(out[0]);
            sample[1] = toShort(out[1]);
            sample[2] = toShort(out[2]);
        }
    };
    parallelRows(height, gradeRow);
}

} // namespace spk::desktop::layer2
