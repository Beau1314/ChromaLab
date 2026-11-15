#include "PairPiece.hpp"

#include <algorithm>
#include <cmath>
#include <QStringList>

namespace spk::desktop::pair {
namespace {

// The hole's aspect as drawn: width over height. Held level a hole is 24 mm
// across and 18 along; turned, the piece transposes and so does the hole.
double holeAspect(bool turned) {
    return turned ? kHoleAcrossMM / kHoleAlongMM : kHoleAlongMM / kHoleAcrossMM;
}

// The largest hole-aspect rectangle centred in the source, at scale 1
// (HalfFramePair.sourceRect with a scale-1, centred placement): the picture
// as framed, fit to the hole without enlargement. Empty for an empty source.
QSizeF holeFit(QSizeF source, bool turned) {
    if (source.width() <= 0 || source.height() <= 0) return QSizeF();
    const double aspect = holeAspect(turned);
    double w = source.width(), h = source.height();
    if (w / h > aspect) w = h * aspect;
    else h = w / aspect;
    return QSizeF(w, h);
}

// `layout` for an `across` already decided (HalfFramePair.layout): the
// formulas the model is held to. Swift's rounded() is round-half-away-from-
// zero, exactly std::lround's rule.
Layout layoutForAcross(int acrossPx, const PieceSpec& spec) {
    const int a = std::max(acrossPx, 8);
    const int along = std::max(int(std::lround(a * kHoleAlongMM / kHoleAcrossMM)), 6);
    const int advance = int(std::lround(along * (kHoleAlongMM + spec.spacingMM) / kHoleAlongMM));
    Layout layout;
    layout.advancePx = advance;
    layout.pieceSize = spec.turned ? QSize(a, advance + along) : QSize(advance + along, a);
    const QRectF hole = spec.turned ? QRectF(0, 0, a, along) : QRectF(0, 0, along, a);
    layout.rect[0] = hole;
    layout.rect[1] = spec.turned ? hole.translated(0, advance) : hole.translated(advance, 0);
    return layout;
}

// One bilinear sample of the decoded picture at a pixel coordinate (already
// centred): clamped to the picture, mixing the four neighbours.
void sampleBilinear(const io::DecodedRaw& source, double sx, double sy, float out[3]) {
    const int w = int(source.width), h = int(source.height);
    const double x = std::clamp(sx, 0.0, double(w - 1));
    const double y = std::clamp(sy, 0.0, double(h - 1));
    const int x0 = int(x), y0 = int(y);
    const int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
    const double fx = x - x0, fy = y - y0;
    const float* p00 = &source.rgb[(std::size_t(y0) * w + x0) * 3];
    const float* p10 = &source.rgb[(std::size_t(y0) * w + x1) * 3];
    const float* p01 = &source.rgb[(std::size_t(y1) * w + x0) * 3];
    const float* p11 = &source.rgb[(std::size_t(y1) * w + x1) * 3];
    for (int c = 0; c < 3; ++c) {
        const double top = p00[c] * (1.0 - fx) + p10[c] * fx;
        const double bottom = p01[c] * (1.0 - fx) + p11[c] * fx;
        out[c] = float(top * (1.0 - fy) + bottom * fy);
    }
}

// One hole's picture into its rectangle of the piece (the wire's §2 order:
// crop by the window, turn clockwise, scale to the hole, multiply by the
// gain, place). The inverse of the clockwise turn carries each destination
// pixel back to its source pixel: a quarter turn maps (nx, ny) -> (ny, 1-nx)
// and so on, read backwards here.
void placeHole(std::vector<float>& piece, const QSize& pieceSize, const QRectF& rect,
               const HoleInput& hole) {
    const io::DecodedRaw& source = *hole.decoded;
    if (source.width == 0 || source.height == 0 ||
        source.rgb.size() < std::size_t(source.width) * source.height * 3)
        return;
    const QRect dest = rect.toRect().intersected(QRect(0, 0, pieceSize.width(), pieceSize.height()));
    if (dest.isEmpty() || rect.width() <= 0 || rect.height() <= 0) return;
    const int turns = ((hole.quarterTurns % 4) + 4) % 4;
    const double factor = std::pow(2.0, hole.gainStops);
    for (int y = dest.top(); y <= dest.bottom(); ++y) {
        const double ny = (y - rect.y() + 0.5) / rect.height();
        for (int x = dest.left(); x <= dest.right(); ++x) {
            const double nx = (x - rect.x() + 0.5) / rect.width();
            double cx, cy; // the pixel's place in the cropped picture, y down
            switch (turns) {
                case 1:  cx = ny;        cy = 1.0 - nx;  break;
                case 2:  cx = 1.0 - nx;  cy = 1.0 - ny;  break;
                case 3:  cx = 1.0 - ny;  cy = nx;        break;
                default: cx = nx;        cy = ny;        break;
            }
            const double sx = (hole.window.x() + cx * hole.window.width()) * source.width - 0.5;
            const double sy = (hole.window.y() + cy * hole.window.height()) * source.height - 0.5;
            float rgb[3];
            sampleBilinear(source, sx, sy, rgb);
            float* out = &piece[(std::size_t(y) * pieceSize.width() + x) * 3];
            out[0] = float(rgb[0] * factor);
            out[1] = float(rgb[1] * factor);
            out[2] = float(rgb[2] * factor);
        }
    }
}

}  // namespace

Layout layout(QSizeF holeA, QSizeF holeB, const PieceSpec& spec) {
    // Each hole's extent across the film at its own fit: level that is the
    // fit's height, turned its width. The holes take the smaller of the two,
    // and the piece is never longer than the longer picture -- read at the
    // camera's own 1 mm, so a hole's size does not move with the gap.
    const QSizeF sizes[2] = {holeA, holeB};
    double across[2] = {0.0, 0.0};
    bool any = false;
    double longest = 0.0;
    for (int i = 0; i < 2; ++i) {
        const QSizeF fit = holeFit(sizes[i], spec.turned);
        if (fit.isNull() || fit.isEmpty()) continue;
        across[i] = spec.turned ? fit.width() : fit.height();
        any = true;
        longest = std::max(longest, std::max(sizes[i].width(), sizes[i].height()));
    }
    double a = any ? std::min(across[0], across[1]) : 2400.0;
    if (longest > 0.0) {
        const double pieceLong = (kHoleAlongMM * 2.0 + 1.0) / kHoleAcrossMM;
        a = std::min(a, longest / pieceLong);
    }
    return layoutForAcross(int(std::floor(a)), spec);
}

std::vector<float> compose(const Layout& layout, const HoleInput& first, const HoleInput& second) {
    const QSize size = layout.pieceSize;
    std::vector<float> piece(std::size_t(std::max(size.width(), 0)) * std::size_t(std::max(size.height(), 0)) * 3,
                             0.0f);
    if (size.isEmpty()) return piece;
    const HoleInput* holes[2] = {&first, &second};
    for (int side = 0; side < 2; ++side) {
        // An unpresent hole or a null decode is unexposed film: zeros, like
        // the gap. Everything but the holes is zeros and stays so.
        if (!holes[side]->present || holes[side]->decoded == nullptr) continue;
        placeHole(piece, size, layout.rect[side], *holes[side]);
    }
    return piece;
}

double stops(double meteredEV, double holeEV) { return meteredEV + holeEV; }

double gain(double stops) { return std::pow(2.0, stops); }

QString meterKey(const QString& frameId, const QString& decodeKey,
                 double temperature, double tint, bool lensCorrection,
                 const QString& method, const QRectF& crop, int quarterTurns) {
    // PAIR-WIRING §4.2: the frame, its decode, its own crop and the intent.
    // The placement is deliberately absent from the argument list: a picture
    // moved under its hole keeps its key, so it does not pump while dragged.
    const QString cropField = QStringLiteral("%1,%2,%3,%4,%5")
                                  .arg(QString::number(crop.x()), QString::number(crop.y()),
                                       QString::number(crop.width()), QString::number(crop.height()),
                                       QString::number(quarterTurns));
    return QStringList{frameId, decodeKey, QString::number(temperature), QString::number(tint),
                       QString::number(int(lensCorrection)), method, cropField}
        .join(u'|');
}

}  // namespace spk::desktop::pair
