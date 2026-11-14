#pragma once

// Layer 2 — the scan-side grade (white balance on the scan, exposure, curves,
// colour balance, vignette), a port of `desktop/src/shared/adjustments.ts`.
// Field names, defaults and the JSON wire shape match that file exactly, so an
// edit saved here still opens on the Mac and desktop builds.
//
// This task carries the data contract: the structures, the neutrality test and
// JSON encode/decode. The math below is declared here and ported expression by
// expression in the Layer 2 math task, in adjustments.ts's fixed order:
// white balance → exposure/contrast/brightness → highlights/shadows
// → black/white point → saturation → colour balance → curves → vignette.

#include <QImage>
#include <QJsonObject>
#include <QPointF>
#include <array>
#include <cstdint>
#include <vector>

namespace spk::desktop::layer2 {

struct Curve {
    std::vector<QPointF> points;
    bool operator==(const Curve&) const = default;
};

// The five shader rows, in the shader's layout (adjustments.ts CURVE_CHANNELS).
struct CurveSet {
    Curve rgb, luma, red, green, blue;
    bool operator==(const CurveSet&) const = default;
};

[[nodiscard]] Curve identityCurve();
[[nodiscard]] CurveSet identityCurves();

struct ColorZone {
    double hue = 0;
    double saturation = 0;
    double luminance = 0;
    bool operator==(const ColorZone&) const = default;
};

struct ColorBalance {
    ColorZone master, shadows, midtones, highlights;
    bool operator==(const ColorBalance&) const = default;
};

struct Vignette {
    double amount = 0;
    double midpoint = 50;
    bool operator==(const Vignette&) const = default;
};

struct Adjustments {
    bool enabled = true;
    double temperature = 0;
    double tint = 0;
    double exposure = 0;
    double contrast = 0;
    double brightness = 0;
    double saturation = 0;
    double highlights = 0;
    double shadows = 0;
    double blackPoint = 0;
    double whitePoint = 0;
    CurveSet curves = identityCurves();
    ColorBalance colorBalance;
    Vignette vignette;
    bool operator==(const Adjustments&) const = default;
};

// All sliders zero, every curve the two-point identity (0,0)–(1,1), every zone
// inert in saturation and luminance, the vignette amount zero. Hue and the
// vignette midpoint alone are not adjustments (adjustments.ts isNeutral).
[[nodiscard]] bool isNeutral(const Adjustments& adjustments);

// The wire shape the desktop's decodeAdjustments reads; keys and defaults are
// exactly its, and decode accepts both [x, y] and {x, y} curve points (the
// Mac's CGPoint encoding).
[[nodiscard]] QJsonObject encode(const Adjustments& adjustments);
[[nodiscard]] Adjustments decode(const QJsonObject& raw);

// --- the layer arithmetic; declared here, implemented in the math task ------

// sRGB-encoded mid grey (adjustments.ts SRGB_MID_GREY; the ProPhoto one is
// 0.3857). The grade runs on pixels encoded in midGrey's curve.
inline constexpr double kMidGreySrgb = 0.4614;

double evaluateCurve(const Curve& curve, double x);
// 256 × 5 floats: rgb, luma, red, green, blue — the shader's table layout.
std::vector<float> curveTables(const CurveSet& curves);
int insertPoint(Curve& curve, double x, double y);
void movePoint(Curve& curve, int index, double x, double y);
void removePoint(Curve& curve, int index);

// The per-frame uniform block; its fields mirror adjustments.ts
// Layer2Uniforms and are defined with the math task.
struct Uniforms;
Uniforms uniforms(const Adjustments& adjustments, double midGrey);
std::array<double, 3> pixel(const std::array<double, 3>& rgb, const Uniforms& uniforms,
                            const float* tables, double x, double y);
void applyImage(QImage& image, const Adjustments& adjustments, double midGrey = kMidGreySrgb);
void applyRgba16(std::vector<std::uint16_t>& rgba, int width, int height,
                 const Adjustments& adjustments, double midGrey = kMidGreySrgb);

} // namespace spk::desktop::layer2
