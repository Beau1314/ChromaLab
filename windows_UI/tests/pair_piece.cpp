// The pair piece's composition core (PAIR-WIRING §2 and §4): the layout
// maths against the model's own formulas (HalfFramePair.pieceLayout/layout),
// the per-hole compose (crop, clockwise turn, scale, gain, zeros elsewhere),
// the meter key that excludes the placement, and PreviewHost::solveExposure's
// reading of a synthetic picture. Pure in the CTest sense -- no RAW fixture;
// the only engine work is one throwaway 1024 px meter session per solve.
#include "desktop/preview_host.hpp"
#include "core/json.hpp"
#include "PairPiece.hpp"
#include "spektralab/spk_engine.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace spk::desktop;
namespace pairc = spk::desktop::pair;
namespace io = spk::io;

namespace {

unsigned checks = 0;

void check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

void check(bool condition, std::string message) {
    check(condition, message.c_str());
}

template<class F> void rejected(F&& function, const char* message) {
    try {
        function();
    } catch (const std::exception& error) {
        check(*error.what() != '\0', "rejection omitted diagnostic");
        return;
    }
    throw std::runtime_error(message);
}

void close(double got, double want, double tolerance, const char* message) {
    check(std::abs(got - want) <= tolerance,
          std::string(message) + " (got " + std::to_string(got) + ", want " +
              std::to_string(want) + ")");
}

// A DecodedRaw-shaped hole picture (the compose path reads only rgb/width/
// height): every channel carries the given per-pixel value function.
io::DecodedRaw picture(int w, int h, double (*value)(double x, double y)) {
    io::DecodedRaw decoded;
    decoded.width = std::uint32_t(w);
    decoded.height = std::uint32_t(h);
    decoded.rgb.resize(std::size_t(w) * h * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const double v = value(double(x), double(y));
            decoded.rgb[(std::size_t(y) * w + x) * 3 + 0] = float(v);
            decoded.rgb[(std::size_t(y) * w + x) * 3 + 1] = float(v);
            decoded.rgb[(std::size_t(y) * w + x) * 3 + 2] = float(v);
        }
    return decoded;
}

double column(double x, double) { return x; }
double rowish(double x, double y) { return 0.1 * x + 0.01 * y; }
double flat(double, double) { return 0.25; }
double uniform(double x, double) { return x / 99.0; }

// The metering wire a hole is solved under (PAIR-WIRING §4.1): the meter on,
// exposure 0, a plain 24 mm frame, no film edge, no date.
std::string meter_wire() {
    spk::Json params = spk::Json::object();
    params.set("input_color_space", spk::Json(std::string("ProPhoto RGB")));
    params.set("input_cctf_decoding", spk::Json(false));
    params.set("film_stock", spk::Json(std::string("kodak_portra_400")));
    params.set("print_stock", spk::Json(std::string("kodak_portra_endura")));
    params.set("auto_exposure", spk::Json(true));
    params.set("auto_exposure_method", spk::Json(std::string("center_weighted")));
    params.set("exposure_compensation_ev", spk::Json(0.0));
    params.set("film_format_mm", spk::Json(24.0));
    params.set("grain_active", spk::Json(false));
    params.set("halation_active", spk::Json(false));
    params.set("glare_active", spk::Json(false));
    params.set("overscan_active", spk::Json(false));
    params.set("date_imprint_active", spk::Json(false));
    return params.dump();
}

// --- layout ---------------------------------------------------------------

void layout_advance_formula() {
    // A picture far wider than a hole: the hole-aspect fit leaves `across` =
    // the source height, and the piece cap cannot bind (the source is longer
    // than a whole piece's worth). The formulas then run alone:
    // along = round(a x 18/24), advance = round(along x (18+spacing)/18).
    const pairc::Layout level = pairc::layout(QSizeF(2000, 300), QSizeF(2000, 300), pairc::PieceSpec{});
    check(level.pieceSize == QSize(463, 300), "piece size is two holes and the gap");
    close(level.advancePx, 238.0, 0.0, "advance = round(along x (18+spacing)/18)");
    check(level.rect[0] == QRectF(0, 0, 225, 300), "the first hole sits at the long edge's start");
    check(level.rect[1] == QRectF(238, 0, 225, 300), "the second hole sits one advance along");
    // A wider gap moves the advance and the piece, never the hole.
    const pairc::Layout wide = pairc::layout(QSizeF(2000, 300), QSizeF(2000, 300), pairc::PieceSpec{2.0, false});
    close(wide.advancePx, 250.0, 0.0, "advance follows the spacing");
    check(wide.rect[1].x() == 250.0 && wide.rect[0].width() == 225.0,
          "a wider gap moves the second hole, not the holes themselves");
}

void layout_hole_is_the_smaller_picture() {
    // The holes are as large as the smaller picture is across its hole, so
    // neither is enlarged -- whichever side the smaller picture is on.
    const pairc::PieceSpec spec{};
    const pairc::Layout first = pairc::layout(QSizeF(2000, 300), QSizeF(150, 600), spec);
    const pairc::Layout second = pairc::layout(QSizeF(150, 600), QSizeF(2000, 300), spec);
    for (const pairc::Layout* l : {&first, &second}) {
        check(l->rect[0].size() == QSizeF(150, 200) && l->rect[1].size() == QSizeF(150, 200),
              "both holes take the smaller picture's across");
        check(l->pieceSize == QSize(308, 200), "the piece follows the smaller holes");
    }
    check(first.advancePx == second.advancePx, "the advance does not depend on the side order");
}

void layout_piece_cap() {
    // A pair of hole-aspect pictures: the uncapped piece would be 37/24 of a
    // frame's worth of film. The cap holds the piece at the longer picture,
    // and the holes shrink with it (both pictures are the same size here).
    const pairc::Layout level = pairc::layout(QSizeF(600, 800), QSizeF(600, 800), pairc::PieceSpec{});
    check(level.pieceSize.width() == 800, "the piece is never longer than the longer picture");
    check(level.pieceSize.height() == 518, "the holes shrink under the cap (a = 800 x 24/37)");
    check(level.rect[1].x() == 411.0, "the advance follows the capped holes");
    // Asymmetric pictures: the cap reads the longer one, and the holes shrink
    // below even the smaller picture's own across.
    const pairc::Layout mixed = pairc::layout(QSizeF(600, 800), QSizeF(900, 1200), pairc::PieceSpec{});
    check(mixed.pieceSize.width() == 1200, "the cap binds at the longer picture's own size");
    check(mixed.rect[0].height() == 778.0, "the holes shrink to the capped across");
}

void layout_turned() {
    // Turned, the film runs down: the hole transposes (across x along), the
    // second hole sits below, and the fit runs against the transposed aspect.
    // The source is taller than the transposed hole, so its fit width -- the
    // turned hole's across -- is the source's own width, exactly.
    const pairc::Layout turned = pairc::layout(QSizeF(300, 2000), QSizeF(300, 2000), pairc::PieceSpec{1.0, true});
    check(turned.pieceSize == QSize(300, 463), "a turned piece is 24 x 37-shaped");
    check(turned.rect[0] == QRectF(0, 0, 300, 225), "the first hole is the upper one");
    check(turned.rect[1] == QRectF(0, 238, 300, 225), "the second hole sits one advance down");
    close(turned.advancePx, 238.0, 0.0, "the turned advance uses the same formula");
}

void layout_empty_pair() {
    // An empty pair still takes a nominal size -- nothing divides by zero.
    const pairc::Layout empty = pairc::layout(QSizeF(0, 0), QSizeF(0, 0), pairc::PieceSpec{});
    check(empty.pieceSize == QSize(3700, 2400), "an empty pair takes the nominal piece");
    check(empty.rect[0].size() == QSizeF(1800, 2400), "the nominal hole follows the same shape");
}

void layout_one_sided_pair() {
    // PAIR-WIRING §5: a frame can leave its hole Missing. Swift skips the
    // absent side and takes the min over the present ones (`across.min() ??
    // 2400`); a missing side must not be read as a zero-across picture, or
    // the piece collapses onto the 8 px floor.
    const pairc::PieceSpec spec{};
    const pairc::Layout first = pairc::layout(QSizeF(2000, 300), QSizeF(0, 0), spec);
    const pairc::Layout second = pairc::layout(QSizeF(0, 0), QSizeF(2000, 300), spec);
    for (const pairc::Layout* l : {&first, &second}) {
        check(l->pieceSize == QSize(463, 300), "a one-sided pair lays out the present picture alone");
        check(l->rect[0] == QRectF(0, 0, 225, 300), "the hole fits the present picture, not the 8 px floor");
        check(l->rect[1] == QRectF(238, 0, 225, 300), "the missing side keeps its hole slot");
    }
    check(first.advancePx == second.advancePx, "the advance follows the present picture regardless of side");
}

void layout_single_hole_meter_frame() {
    // PAIR-WIRING §4.1: the meter session reads THAT HOLE'S PICTURE ALONE --
    // the hole itself is the piece: no gap, no second hole. The hole's own
    // size follows the pair's formulas, so the meter's hole matches the
    // pair's hole for the same picture.
    const pairc::PieceSpec spec{};
    const pairc::Layout one = pairc::holeLayout(QSizeF(2000, 300), spec);
    const pairc::Layout pair = pairc::layout(QSizeF(2000, 300), QSizeF(0, 0), spec);
    check(one.rect[0] == pair.rect[0], "the meter's hole is the pair's own hole");
    check(one.pieceSize == QSize(225, 300), "the piece is the hole alone");
    check(one.rect[1].isNull(), "the single-hole layout has no second hole");
    check(one.advancePx == 0.0, "the single-hole layout has no advance");
    // A hole-aspect picture fills its hole exactly.
    check(pairc::holeLayout(QSizeF(750, 1000), spec).pieceSize == QSize(750, 1000),
          "a hole-aspect picture is its own single-hole piece");
    // Turned, the fit runs against the transposed aspect: the level fit of
    // (2000, 300) is 225 x 300, the turned one 400 x 300.
    const pairc::Layout turned = pairc::holeLayout(QSizeF(2000, 300), pairc::PieceSpec{1.0, true});
    check(turned.pieceSize == QSize(400, 300), "a turned single hole runs the film down");
    check(turned.rect[0] == QRectF(0, 0, 400, 300), "the turned hole is the fit's own shape");
    // An empty picture still takes the nominal hole -- nothing divides by zero.
    const pairc::Layout empty = pairc::holeLayout(QSizeF(0, 0), spec);
    check(empty.pieceSize == QSize(1800, 2400), "an empty single hole takes the nominal hole");
}

// --- compose --------------------------------------------------------------

void compose_identity_and_both_holes() {
    // A hand-built two-hole layout with square holes: what lands in a hole is
    // its picture, pixel for pixel, and the two holes carry two pictures.
    const io::DecodedRaw first = picture(4, 4, &column);
    const io::DecodedRaw second = picture(4, 4, &rowish);
    pairc::Layout layout;
    layout.pieceSize = QSize(8, 4);
    layout.rect[0] = QRectF(0, 0, 4, 4);
    layout.rect[1] = QRectF(4, 0, 4, 4);
    layout.advancePx = 4.0;
    const auto piece = pairc::compose(layout,
                                      pairc::HoleInput{&first, QRectF(0, 0, 1, 1), 0, 0.0, true},
                                      pairc::HoleInput{&second, QRectF(0, 0, 1, 1), 0, 0.0, true});
    check(piece.size() == std::size_t(8) * 4 * 3, "the piece is one RGB buffer");
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            close(piece[(std::size_t(y) * 8 + x) * 3], double(x), 1e-6, "the first hole is the first picture");
            close(piece[(std::size_t(y) * 8 + 4 + x) * 3], rowish(double(x), double(y)), 1e-6,
                  "the second hole is the second picture");
        }
}

void compose_crop() {
    // The window is source-normalised, applied before the turn: the right
    // half of an 8-wide picture fills a 4-wide hole, unresampled.
    const io::DecodedRaw wide = picture(8, 4, &column);
    pairc::Layout layout;
    layout.pieceSize = QSize(4, 4);
    layout.rect[0] = QRectF(0, 0, 4, 4);
    layout.rect[1] = QRectF(0, 0, 0, 0);
    const auto piece = pairc::compose(layout,
                                      pairc::HoleInput{&wide, QRectF(0.5, 0.0, 0.5, 1.0), 0, 0.0, true},
                                      pairc::HoleInput{});
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            close(piece[(std::size_t(y) * 4 + x) * 3], double(4 + x), 1e-6,
                  "the window cuts the picture the placement asked for");
}

void compose_turns() {
    // quarterTurns turns the picture clockwise as seen, about its own crop.
    const io::DecodedRaw turned = picture(4, 4, &column);
    pairc::Layout layout;
    layout.pieceSize = QSize(4, 4);
    layout.rect[0] = QRectF(0, 0, 4, 4);
    layout.rect[1] = QRectF(0, 0, 0, 0);
    for (int turns = 0; turns < 4; ++turns) {
        // The column gradient reads back one quarter step clockwise a turn.
        const auto piece = pairc::compose(layout,
                                          pairc::HoleInput{&turned, QRectF(0, 0, 1, 1), turns, 0.0, true},
                                          pairc::HoleInput{});
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) {
                const double want = turns == 0 ? double(x)
                                  : turns == 1 ? double(y)
                                  : turns == 2 ? double(3 - x)
                                  : double(3 - y);
                close(piece[(std::size_t(y) * 4 + x) * 3], want, 1e-6,
                      "a clockwise turn rotates the picture as seen");
            }
    }
}

void compose_gain() {
    const io::DecodedRaw even = picture(4, 4, &flat);
    pairc::Layout layout;
    layout.pieceSize = QSize(4, 4);
    layout.rect[0] = QRectF(0, 0, 4, 4);
    layout.rect[1] = QRectF(0, 0, 0, 0);
    const auto up = pairc::compose(layout, pairc::HoleInput{&even, QRectF(0, 0, 1, 1), 0, 2.0, true},
                                   pairc::HoleInput{});
    close(up[0], 1.0, 1e-6, "two stops up is four times the light");
    const auto down = pairc::compose(layout, pairc::HoleInput{&even, QRectF(0, 0, 1, 1), 0, -1.0, true},
                                     pairc::HoleInput{});
    close(down[0], 0.125, 1e-6, "a stop down is half the light");
    check(pairc::gain(pairc::stops(1.5, -0.5)) == 2.0,
          "stops adds the meter reading and the hole's own exposure");
}

void compose_gap_and_empty_holes() {
    // The real layout at a hole of 225 x 300: a 13 px gap of unexposed film
    // between the holes, and an unpresent picture leaves its hole empty too.
    const pairc::Layout layout = pairc::layout(QSizeF(2000, 300), QSizeF(2000, 300), pairc::PieceSpec{});
    const io::DecodedRaw left_picture = picture(300, 300, &flat);
    const io::DecodedRaw right_picture = picture(300, 300, &flat);
    auto piece = pairc::compose(layout,
                                pairc::HoleInput{&left_picture, QRectF(0, 0, 1, 1), 0, 0.0, true},
                                pairc::HoleInput{&right_picture, QRectF(0, 0, 1, 1), 0, 0.0, false});
    close(piece[(std::size_t(150) * layout.pieceSize.width() + 224) * 3], 0.25, 1e-6,
          "the first hole carries its picture to its right edge");
    for (int x = 225; x < 238; ++x)
        close(piece[(std::size_t(150) * layout.pieceSize.width() + x) * 3], 0.0, 0.0,
              "the gap is unexposed film: no light at all");
    for (int x = 238; x < 463; x += 7)
        close(piece[(std::size_t(150) * layout.pieceSize.width() + x) * 3], 0.0, 0.0,
              "a hole with present=false is an empty hole: zeros");
    // A null decode is an empty hole as well.
    piece = pairc::compose(layout,
                           pairc::HoleInput{nullptr, QRectF(0, 0, 1, 1), 0, 0.0, true},
                           pairc::HoleInput{&right_picture, QRectF(0, 0, 1, 1), 0, 0.0, true});
    close(piece[(std::size_t(150) * layout.pieceSize.width() + 100) * 3], 0.0, 0.0,
          "a null decode leaves the hole at no light");
    close(piece[(std::size_t(150) * layout.pieceSize.width() + 300) * 3], 0.25, 1e-6,
          "the second hole composes on regardless");
}

void compose_tier_scaling() {
    // The same layout scaled for a smaller job: the hole resamples the same
    // window, and bilinear at a half-pixel average keeps the columns' means.
    const io::DecodedRaw gradient = picture(100, 100, &uniform);
    pairc::Layout layout;
    layout.pieceSize = QSize(100, 50);
    layout.rect[0] = QRectF(0, 0, 50, 50);
    layout.rect[1] = QRectF(50, 0, 50, 50);
    layout.advancePx = 50.0;
    const auto piece = pairc::compose(layout,
                                      pairc::HoleInput{&gradient, QRectF(0, 0, 1, 1), 0, 0.0, true},
                                      pairc::HoleInput{&gradient, QRectF(0, 0, 1, 1), 0, 0.0, true});
    for (int x = 0; x < 50; x += 7) {
        // 100 -> 50 lands between two source columns: their average.
        close(piece[(std::size_t(25) * 100 + x) * 3], (4.0 * x + 1.0) / 198.0, 1e-6,
              "the scaled hole resamples the window (half-pixel average)");
        close(piece[(std::size_t(25) * 100 + 50 + x) * 3], (4.0 * x + 1.0) / 198.0, 1e-6,
              "the second hole scales the same way");
    }
}

void compose_single_hole_meter_frame() {
    // PAIR-WIRING §4.1: the meter's buffer carries no unexposed film -- the
    // single-hole piece is picture everywhere, so the solver never averages
    // the gap or an empty hole into the reading.
    const io::DecodedRaw even = picture(300, 400, &flat);
    const pairc::Layout one = pairc::holeLayout(QSizeF(300, 400), pairc::PieceSpec{});
    const auto piece = pairc::compose(one,
                                      pairc::HoleInput{&even, QRectF(0, 0, 1, 1), 0, 0.0, true},
                                      pairc::HoleInput{});
    check(piece.size() == std::size_t(one.pieceSize.width()) * one.pieceSize.height() * 3,
          "the single-hole buffer is the hole's own RGB buffer");
    for (int y = 0; y < one.pieceSize.height(); y += 13)
        for (int x = 0; x < one.pieceSize.width(); x += 13)
            close(piece[(std::size_t(y) * one.pieceSize.width() + x) * 3], 0.25, 1e-6,
                  "the meter's frame is picture to its last pixel: no gap, no empty hole");
}

// --- meter key --------------------------------------------------------------

void meter_key_identity() {
    // PAIR-WIRING §4.2: the key is the frame, its decode, its own crop and
    // the intent -- exactly those fields, and nothing of the placement or the
    // piece. The same frame under a moved placement (a different compose
    // window, a different piece layout) meters once.
    const QString key = pairc::meterKey(QStringLiteral("frame-1"), QStringLiteral("as_shot"),
                                        5300.0, 10.0, true, QStringLiteral("balanced"),
                                        QRectF(0.1, 0.2, 0.6, 0.8), 1);
    check(key == QStringLiteral("frame-1|as_shot|5300|10|1|balanced|0.1,0.2,0.6,0.8,1"),
          "the key is the documented fields joined with '|'");
    check(key == pairc::meterKey(QStringLiteral("frame-1"), QStringLiteral("as_shot"), 5300.0, 10.0,
                                 true, QStringLiteral("balanced"), QRectF(0.1, 0.2, 0.6, 0.8), 1),
          "the same frame and decode keep one key");
    const auto differs = [&](const QString& id, const QString& decode, double temp, double tint,
                             bool lens, const QString& method, QRectF crop, int turns,
                             const char* why) {
        check(key != pairc::meterKey(id, decode, temp, tint, lens, method, crop, turns), why);
    };
    differs(QStringLiteral("frame-2"), QStringLiteral("as_shot"), 5300.0, 10.0, true,
            QStringLiteral("balanced"), QRectF(0.1, 0.2, 0.6, 0.8), 1, "another frame is another key");
    differs(QStringLiteral("frame-1"), QStringLiteral("daylight"), 5300.0, 10.0, true,
            QStringLiteral("balanced"), QRectF(0.1, 0.2, 0.6, 0.8), 1, "another decode is another key");
    differs(QStringLiteral("frame-1"), QStringLiteral("as_shot"), 6500.0, 10.0, true,
            QStringLiteral("balanced"), QRectF(0.1, 0.2, 0.6, 0.8), 1, "another temperature is another key");
    differs(QStringLiteral("frame-1"), QStringLiteral("as_shot"), 5300.0, -10.0, true,
            QStringLiteral("balanced"), QRectF(0.1, 0.2, 0.6, 0.8), 1, "another tint is another key");
    differs(QStringLiteral("frame-1"), QStringLiteral("as_shot"), 5300.0, 10.0, false,
            QStringLiteral("balanced"), QRectF(0.1, 0.2, 0.6, 0.8), 1, "the lens switch is part of the key");
    differs(QStringLiteral("frame-1"), QStringLiteral("as_shot"), 5300.0, 10.0, true,
            QStringLiteral("protect_highlights"), QRectF(0.1, 0.2, 0.6, 0.8), 1,
            "the intent is part of the key");
    differs(QStringLiteral("frame-1"), QStringLiteral("as_shot"), 5300.0, 10.0, true,
            QStringLiteral("balanced"), QRectF(0.1, 0.3, 0.6, 0.8), 1, "the frame's own crop is part of the key");
    differs(QStringLiteral("frame-1"), QStringLiteral("as_shot"), 5300.0, 10.0, true,
            QStringLiteral("balanced"), QRectF(0.1, 0.2, 0.6, 0.8), 3, "the frame's own turn is part of the key");
}

// --- solveExposure ----------------------------------------------------------

spk_image image_of(std::vector<float>& buffer, int w, int h) {
    return spk_image{buffer.data(), std::uint32_t(w), std::uint32_t(h), 3};
}

std::vector<float> gradient(int w, int h, double low, double span, double tint) {
    std::vector<float> buffer(std::size_t(w) * h * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const double v = low + span * (double(x) / w) + 0.2 * span * (double(y) / h);
            buffer[(std::size_t(y) * w + x) * 3 + 0] = float(v);
            buffer[(std::size_t(y) * w + x) * 3 + 1] = float(v * tint);
            buffer[(std::size_t(y) * w + x) * 3 + 2] = float(v * tint * tint);
        }
    return buffer;
}

void solve_exposure(const fs::path& resources) {
    PreviewHost host(resources);
    check(host.current() == nullptr, "the meter needs no open session of its own");

    // A dark picture: the meter asks for a positive placement, a sane number
    // of stops, and the same reading twice.
    std::vector<float> dark = gradient(800, 600, 0.02, 0.05, 0.9);
    const spk_image small = image_of(dark, 800, 600);
    const double ev = host.solveExposure(small, meter_wire());
    check(std::isfinite(ev), "the meter returns a number");
    check(ev > 0.5 && ev < 12.0, "a dark picture meters up by a sane number of stops");
    check(host.solveExposure(small, meter_wire()) == ev, "the same picture meters the same twice");

    // A bright picture: the meter pulls it back.
    std::vector<float> bright = gradient(800, 600, 1.5, 1.0, 0.9);
    const double ev_bright = host.solveExposure(image_of(bright, 800, 600), meter_wire());
    check(std::isfinite(ev_bright) && ev_bright < -0.2 && ev_bright > -12.0,
          "a bright picture meters down by a sane number of stops");

    // Above 1024 px the meter session is downscaled first, and reads the same
    // picture within a fraction of a stop.
    std::vector<float> big = gradient(1600, 1200, 0.02, 0.05, 0.9);
    const double ev_big = host.solveExposure(image_of(big, 1600, 1200), meter_wire());
    check(std::isfinite(ev_big) && ev_big > 0.5 && ev_big < 12.0,
          "a picture above the meter cap still reads sanely");
    check(std::abs(ev_big - ev) < 0.35, "the meter cap moves the reading by a fraction of a stop");

    // The meter never becomes the host's own session.
    check(host.current() == nullptr, "the throwaway meter session is released");

    // Refusals: an unusable wire, an unusable image.
    rejected([&] { host.solveExposure(small, "{\"film_format_mm\": 99999.0}"); },
             "an out-of-range meter wire was accepted");
    const spk_image empty = spk_image{nullptr, 8, 8, 3};
    rejected([&] { host.solveExposure(empty, meter_wire()); },
             "a null image was accepted");
    const spk_image bad_channels = spk_image{dark.data(), 8, 8, 7};
    rejected([&] { host.solveExposure(bad_channels, meter_wire()); },
             "a wrong channel count was accepted");
    check(host.current() == nullptr, "a refused meter leaves no session behind");
}

void solve_exposure_single_hole(const fs::path& resources) {
    // PAIR-WIRING §4.1's executable heart: the meter frame is THAT HOLE'S
    // PICTURE ALONE. The solver reads every pixel it is handed (the engine's
    // zero-light exclusion is the scene latitude's, not the exposure
    // solver's), so composing the hole into the pair piece -- hole + gap +
    // an all-zero empty hole -- hands unexposed film to the solve and skews
    // the EV off the 1.x single-hole calibration basis.
    PreviewHost host(resources);
    io::DecodedRaw decoded;
    decoded.width = 750;
    decoded.height = 1000;
    decoded.rgb = gradient(750, 1000, 0.02, 0.05, 0.9);  // hole-aspect, dark
    const double ev_plain = host.solveExposure(image_of(decoded.rgb, 750, 1000), meter_wire());
    check(std::isfinite(ev_plain), "the meter returns a number");

    // The single-hole frame: the same picture meters the same.
    const pairc::Layout one = pairc::holeLayout(QSizeF(750, 1000), pairc::PieceSpec{});
    check(one.pieceSize == QSize(750, 1000), "a hole-aspect picture is its own single-hole piece");
    std::vector<float> buffer = pairc::compose(
        one, pairc::HoleInput{&decoded, QRectF(0, 0, 1, 1), 0, 0.0, true}, pairc::HoleInput{});
    const double ev_hole = host.solveExposure(
        image_of(buffer, one.pieceSize.width(), one.pieceSize.height()), meter_wire());
    check(std::abs(ev_hole - ev_plain) < 0.01,
          "the single-hole meter frame reads the plain picture's EV");

    // The pair piece is not the meter's basis: its unexposed film reaches
    // the solver and moves the reading. (The one-sided pair layout is the
    // hole plus the gap plus the empty hole's slot -- what the meter frame
    // must not be.)
    const pairc::Layout piece_layout = pairc::layout(QSizeF(750, 1000), QSizeF(0, 0), pairc::PieceSpec{});
    std::vector<float> piece = pairc::compose(piece_layout,
                                              pairc::HoleInput{&decoded, QRectF(0, 0, 1, 1), 0, 0.0, true},
                                              pairc::HoleInput{});
    const double ev_piece = host.solveExposure(
        image_of(piece, piece_layout.pieceSize.width(), piece_layout.pieceSize.height()),
        meter_wire());
    check(std::abs(ev_piece - ev_hole) > 0.2,
          "the pair piece's unexposed film is not the meter's calibration basis");
    check(host.current() == nullptr, "the throwaway meter session is released");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const fs::path resources = argc > 1 ? fs::path(argv[1]) : fs::path("resources");
        layout_advance_formula();
        layout_hole_is_the_smaller_picture();
        layout_piece_cap();
        layout_turned();
        layout_empty_pair();
        layout_one_sided_pair();
        layout_single_hole_meter_frame();
        compose_identity_and_both_holes();
        compose_crop();
        compose_turns();
        compose_gain();
        compose_gap_and_empty_holes();
        compose_tier_scaling();
        compose_single_hole_meter_frame();
        meter_key_identity();
        solve_exposure(resources);
        solve_exposure_single_hole(resources);
        std::cout << "Pair piece: " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
