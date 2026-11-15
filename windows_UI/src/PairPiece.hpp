// The half-frame pair's piece: the layout maths, the per-hole composition and
// the meter key (PAIR-WIRING §2 and §4; the semantic model is
// app/SpektraLab/Sources/Model/HalfFramePair.swift, ported to plain C++/Qt).
// Two decoded pictures become one piece of film: two 18 x 24 mm holes and the
// gap, everything but the holes unexposed (zeros). The engine is then handed
// the whole piece like any frame; each picture's exposure already sits in its
// pixels, multiplied in here by 2^stops.
#pragma once

#include "io/raw_decoder.hpp"

#include <QRectF>
#include <QSize>
#include <QSizeF>
#include <QString>

#include <vector>

namespace spk::desktop::pair {

// Millimetres, the camera's own half frame (PAIR-WIRING §2's table).
constexpr double kHoleAlongMM = 18.0;    // a hole's extent along the film
constexpr double kHoleAcrossMM = 24.0;   // a hole's extent across the film
constexpr double kStripAdvanceMM = 19.0; // the strip's advance: hole + 1 mm gap

// One hole's picture and how it sits in its hole. The window is the
// placement's source rectangle, normalised to the decoded picture with y down
// and applied BEFORE the turn; the turn is clockwise as seen. The composition
// order is the wire's own (PAIR-WIRING §2): crop, turn, scale to the hole,
// multiply by 2^gainStops, place at the layout's rectangle.
struct HoleInput {
    const io::DecodedRaw* decoded = nullptr;
    QRectF window{0.0, 0.0, 1.0, 1.0};
    int quarterTurns = 0;
    double gainStops = 0.0;
    bool present = true; // false = an empty hole: zeros, like the gap
};

struct PieceSpec {
    double spacingMM = 1.0; // the strip's 1 mm (HalfFramePair.effectiveSpacingMM)
    bool turned = false;    // the film runs down: the holes stack vertically
};

// The piece in pixels, top-left origin: two holes of one size, the second
// `advancePx` along the film. The holes' rectangles tile the piece but for
// the gap.
struct Layout {
    QSize pieceSize;
    QRectF rect[2]; // [0] = the left (or upper) hole, [1] = the right (or lower)
    double advancePx = 0.0;
};

// The piece's layout for two pictures as framed (after their own crop and
// turn; the placement's own turns are the caller's to apply first). The holes
// are as large as the smaller picture is across its hole, so neither picture
// is enlarged, and the piece is never longer than the longer picture -- two
// half frames are one frame's worth of film (HalfFramePair.pieceLayout). The
// cap reads the camera's own 1 mm, whatever the spacing: a hole's size must
// not move with the gap. A pair with no usable picture takes the nominal
// 2,400 px hole.
Layout layout(QSizeF holeA, QSizeF holeB, const PieceSpec& spec);

// The meter's layout for one picture as placed (PAIR-WIRING §4.1: the meter
// session reads THAT HOLE'S PICTURE ALONE, 1,024 px at most): the hole alone
// is the piece -- no gap, no second hole -- sized by the same hole-aspect
// fit, so a hole-aspect picture fills its hole exactly. The second rect is
// left empty; compose it with an unpresent hole.
Layout holeLayout(QSizeF hole, const PieceSpec& spec);

// The piece's pixels: tightly packed RGB float32, top row first, zeros
// everywhere but the two holes (linear ProPhoto, exactly what spk_open
// expects of a decode). An unpresent hole or a null decode leaves its hole
// unexposed.
std::vector<float> compose(const Layout& layout, const HoleInput& first, const HoleInput& second);

// The hole's exposure in stops: the meter's reading for the picture alone
// plus the hole's own exposure (PAIR-WIRING §4.3; with the hole's meter off
// the metered reading is 0).
double stops(double meteredEV, double holeEV);

// 2^stops: the engine's own meter is exactly this gain on the frame it is
// handed (the desktop measured the two renders equal to the last code).
double gain(double stops);

// What the meter's reading was taken for (HalfFramePair.meterKey): the frame,
// its decode, its own crop and turn, and the meter's intent. The placement is
// not an input -- a picture moved under its hole is not metered again, so it
// does not pump while it is dragged. `crop` and `quarterTurns` are the frame's
// own geometry, never the placement's window or turn.
QString meterKey(const QString& frameId, const QString& decodeKey,
                 double temperature, double tint, bool lensCorrection,
                 const QString& method, const QRectF& crop, int quarterTurns);

}  // namespace spk::desktop::pair
