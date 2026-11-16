// The light table's arithmetic core (task 6.4): the statement-for-statement
// port of app/SpektraLab/Sources/LightTable/LightTableModel.swift held against
// the same numbers the Mac's tools/lighttable_selftest.swift holds — the
// table's constants and rounding, the film's quarter/lying/extent geometry,
// freeSpot/leastCovered/keptOn/carried placement, the pile CRUD, tidy/random/
// scatter, relayout, hit testing, the viewport (fit, zoom 1–8, the anchor
// zoomAt, the rubber band's e^0.35 give), the 1:1 px/mm guards, the admission
// filter and the true-mm frame sizes, and the lighttable.json persistence
// (schema 2 round trip, the schema-1 A4 default, the corrupt-file set-aside,
// the debounced atomic write, and frame sizes never stored).
// No engine, no helper process, no GPU: pure QtCore/QtGui over a temp
// directory for the store tier.
#include "LightTableModel.hpp"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimer>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace lt = spk::desktop::lighttable;
namespace {
int checks = 0;
void check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
void checkNear(double got, double want, double tol, const char* message) {
    ++checks;
    if (!(std::abs(got - want) <= tol))
        throw std::runtime_error(std::string(message) + " (got " + std::to_string(got) +
                                 ", want " + std::to_string(want) + ")");
}
QUuid id(int n) { return QUuid::createUuidV5(QUuid(), QString::number(n)); }
bool fuzzy(QSizeF a, QSizeF b, double tol = 1e-9) {
    return std::abs(a.width() - b.width()) <= tol && std::abs(a.height() - b.height()) <= tol;
}
bool fuzzy(QPointF a, QPointF b, double tol = 1e-9) {
    return std::abs(a.x() - b.x()) <= tol && std::abs(a.y() - b.y()) <= tol;
}
bool sameBoard(const lt::Board& a, const lt::Board& b) {
    return a.items == b.items && a.order == b.order && a.portrait == b.portrait &&
           a.paper == b.paper;
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        // --- the table's numbers -------------------------------------------
        check(fuzzy(lt::paperSize(lt::Paper::A4), QSizeF(297, 210)), "a4 297x210");
        check(fuzzy(lt::paperSize(lt::Paper::A5), QSizeF(210, 148)), "a5 210x148");
        check(fuzzy(lt::paperSize(lt::Paper::A6), QSizeF(148, 105)), "a6 148x105");
        check(lt::Lt::rim == 2.6, "rim 2.6");
        check(lt::Lt::corner == 3.0, "corner 3");
        check(lt::Lt::desk == 5.0, "desk 5");
        check(lt::Lt::clear == 10.0, "keep-clear 10");
        check(lt::Lt::gap == 3.0, "gap 3");
        check(lt::Lt::pad == 0.8, "pad 0.8");
        check(lt::Lt::zoomMax == 8.0, "zoomMax 8");
        check(lt::Lt::films == 99, "films 99");
        check(lt::Lt::mag == 3.0 && lt::Lt::lensPad == 1.5, "loupe 3x");
        checkNear(lt::Lt::snap, 4 * M_PI / 180, 1e-12, "snap 4deg");
        checkNear(lt::Lt::snapTouch, 7 * M_PI / 180, 1e-12, "snapTouch 7deg");
        checkNear(lt::Lt::step, 15 * M_PI / 180, 1e-12, "step 15deg");
        checkNear(lt::Lt::scatterTurn, 9 * M_PI / 180, 1e-12, "scatterTurn 9deg");
        checkNear(lt::Lt::zoomGive, 0.35, 1e-12, "zoomGive 0.35");
        check(lt::Lt::rest.x == 0.5 && lt::Lt::rest.y == -1.1 && lt::Lt::rest.blur == 2.4 &&
                  lt::Lt::rest.alpha == 0.16,
              "rest shadow");
        check(lt::Lt::lift.x == 2.4 && lt::Lt::lift.y == -4.6 && lt::Lt::lift.blur == 6.5 &&
                  lt::Lt::lift.alpha == 0.2,
              "lift shadow");

        // --- rounding and angles (JavaScript's semantics) -------------------
        check(lt::jsRound(2.5) == 3, "jsRound halves up");
        check(lt::jsRound(-2.5) == -2, "jsRound negative half towards +inf");
        check(lt::jsRound(2.4) == 2, "jsRound down");
        checkNear(lt::settle(88 * M_PI / 180), 90 * M_PI / 180, 1e-12, "settle within 4deg of a quarter");
        checkNear(lt::settle(85 * M_PI / 180), 85 * M_PI / 180, 1e-12, "settle keeps 5deg off");
        checkNear(lt::settle(100 * M_PI / 180, true), 105 * M_PI / 180, 1e-12, "settle steps");
        checkNear(lt::unwind(4 * M_PI), 0.0, 1e-12, "unwind a full turn");
        checkNear(lt::unwind(-3 * M_PI), -M_PI, 1e-12,
                  "unwind into -pi..pi (the half rounds towards +inf)");
        check(lt::degreesText(M_PI / 2) == "90°", "degrees 90");
        check(lt::degreesText(-3 * M_PI / 4) == "-135°", "degrees -135");
        check(lt::degreesText(3.5 * M_PI / 180) == "3.5°", "degrees decimal");
        check(lt::mmText(297.25) == "297.3", "mm one decimal, half up");
        check(lt::mmText(105.24) == "105.2", "mm one decimal");
        check(fuzzy(lt::halfSize(lt::Paper::A4, false), QSizeF(148.5, 105)), "half landscape");
        check(fuzzy(lt::halfSize(lt::Paper::A4, true), QSizeF(105, 148.5)), "half portrait");

        // --- a film's geometry ----------------------------------------------
        {
            lt::Item it;
            it.id = id(1);
            it.size = QSizeF(36, 24);
            check(it.w() == 36 && it.h() == 24, "item w h");
            it.rot = M_PI / 2;
            check(it.quarter() == 1, "quarter of 90deg");
            check(fuzzy(it.lying(), QSizeF(24, 36)), "lying swapped");
            it.rot = 3 * M_PI / 4; // 135deg: JS rounds 1.5 halves UP to quarter 2
            check(it.quarter() == 2, "quarter 135deg rounds up to 2");
            check(fuzzy(it.lying(), QSizeF(36, 24)), "lying unswapped at quarter 2");
            it.rot = M_PI / 4;
            const double c = std::sqrt(0.5);
            check(fuzzy(it.extent(), QSizeF(60 * c, 60 * c)), "extent at 45deg");
            it.rot = 0;
            check(fuzzy(it.extent(), QSizeF(36, 24)), "extent square");
        }

        // --- the RNG (SplitMix64, seed-decided) -----------------------------
        {
            // four sequences, so no comparison consumes another's draws
            lt::Random a(42), b(42), c(7), d(42);
            bool same = true, differ = false;
            for (int i = 0; i < 8; ++i) {
                same = same && a.next() == b.next();
                differ = differ || c.next() != d.next();
            }
            check(same, "same seed same sequence");
            check(differ, "different seeds differ");
        }

        // --- freeSpot: clear of the scale band and the films ----------------
        {
            lt::Board board;
            board.paper = lt::Paper::A4;
            // the nearest free point to the middle is the middle
            const QPointF mid = board.freeSpot(36, 24, QPointF(0, 0));
            check(fuzzy(mid, QPointF(0, 0)), "freeSpot middle free");
            // a film down: the nearest free spot clears it by the gap
            // (extent to extent: the box grows by the gap on both sides)
            const QUuid a = id(10);
            check(board.place(a, QSizeF(36, 24), QPointF(0, 0)), "place first");
            const QPointF p = board.freeSpot(36, 24, QPointF(0, 0));
            const bool clear = std::abs(p.x()) >= 18 + 3 + 18 - 1e-9 ||
                               std::abs(p.y()) >= 12 + 3 + 12 - 1e-9;
            check(clear, "freeSpot clears the film by gap");
            check(std::hypot(p.x(), p.y()) < 28.5, "freeSpot nearest");
            // a film wider than the room: the walk never runs, the ask is clamped
            const QPointF wide = board.freeSpot(400, 24, QPointF(50, 0));
            check(fuzzy(wide, QPointF(50, 0)), "freeSpot clamped for an oversized film");
        }

        // --- leastCovered: overlap least, nearest among equals --------------
        {
            lt::Board board;
            const QUuid a = id(11);
            check(board.place(a, QSizeF(36, 24), QPointF(0, 0)), "place for leastCovered");
            lt::Item it;
            it.id = id(12);
            it.size = QSizeF(36, 24);
            const QPointF p = board.leastCovered(it, QPointF(0, 0));
            check(lt::Board::overlap(lt::Item(it.id, QSizeF(36, 24), p.x(), p.y()),
                                     *board.item(a)) == 0.0,
                  "leastCovered zero overlap with room");
            check(std::hypot(p.x(), p.y()) < 30, "leastCovered nearest among equals");
            // squared overlap: one whole film hides less than a corner of three
            lt::Board::ItemList others;
            lt::Item o1;
            o1.id = id(13);
            o1.size = QSizeF(50, 50);
            o1.x = 0;
            o1.y = 0;
            lt::Item o2 = o1;
            o2.id = id(14);
            o2.x = 60;
            others.push_back(o1);
            others.push_back(o2);
            const QPointF square = board.leastCovered(o2, QPointF(0, 0), &others);
            (void)square; // the shape's smoke: it lies where overlap sums least
            check(std::isfinite(square.x()), "leastCovered finite");
        }

        // --- keptOn: all of it on the light ---------------------------------
        {
            lt::Board board;
            lt::Item it;
            it.id = id(15);
            it.size = QSizeF(36, 24);
            it.x = 200;
            it.y = -200;
            const lt::Item kept = board.keptOn(it);
            checkNear(kept.x, 130.5, 1e-9, "keptOn right edge");
            checkNear(kept.y, -93.0, 1e-9, "keptOn bottom edge");
        }

        // --- carried: one film, and one arrangement -------------------------
        {
            lt::Item it;
            it.id = id(16);
            it.size = QSizeF(36, 24);
            it.x = 130.5; // at its right edge on A4
            const QPointF same = lt::Board::carried(it, QSizeF(148.5, 105), QSizeF(148.5, 105));
            check(fuzzy(same, QPointF(130.5, 0)), "carried same table exact");
            const QPointF small =
                lt::Board::carried(it, QSizeF(148.5, 105), QSizeF(74, 52.5));
            checkNear(small.x(), 56.0, 1e-9, "carried edge stays at the edge");
            lt::Item mid2 = it;
            mid2.x = 0;
            check(fuzzy(lt::Board::carried(mid2, QSizeF(148.5, 105), QSizeF(74, 52.5)),
                        QPointF(0, 0)),
                  "carried middle stays middle");
            // an arrangement keeps its shape while it fits
            lt::Item b = mid2;
            b.id = id(17);
            b.x = 100;
            std::vector<lt::Item> two{mid2, b};
            const QHash<QUuid, QPointF> out =
                lt::Board::carried(two, QSizeF(148.5, 105), QSizeF(74, 52.5));
            check(out.size() == 2, "carried arrangement maps both");
            check(out.value(mid2.id).x() < out.value(b.id).x(), "carried order kept");
            checkNear(out.value(b.id).x() - out.value(mid2.id).x(), 100.0, 1e-6,
                      "carried arrangement keeps millimetres while it fits");
        }

        // --- place / takeOff / forget / toFront / setSize -------------------
        {
            lt::Board board;
            const QUuid a = id(20), b = id(21), c = id(22);
            check(board.place(a, QSizeF(36, 24), QPointF(0, 0)), "place a");
            check(board.place(b, QSizeF(36, 24), QPointF(30, 0)), "place b");
            check(board.items.size() == 2 && board.order.size() == 2, "two on");
            check(board.order.back() == b, "last on top");
            // a film already on comes to the top, its size read again
            check(board.place(a, QSizeF(36, 24)), "place again true");
            check(board.order.back() == a, "place again to front");
            // kept: where it lay last time, carried
            lt::Board::Kept kept;
            kept.x = 100;
            kept.y = 0;
            kept.rot = M_PI / 4;
            kept.portrait = false;
            kept.paper = lt::Paper::A4;
            check(board.place(c, QSizeF(36, 24), {}, &kept), "place kept");
            check(board.order.back() == c, "kept placed on top");
            check(board.item(c)->on, "kept is on");
            checkNear(board.item(c)->rot, M_PI / 4, 1e-12, "kept angle restored");
            // off the table: it waits
            board.takeOff(c);
            check(!board.item(c)->on, "takeOff off");
            check(board.order.size() == 2, "takeOff leaves the pile");
            // out for good
            board.forget(b);
            check(board.item(b) == nullptr && board.order.size() == 1, "forget removes");
            // setSize keeps a film on the light
            board.setSize(a, QSizeF(400, 24));
            check(fuzzy(board.item(a)->size, QSizeF(400, 24)), "setSize applied");
            checkNear(board.item(a)->x, 0.0, 1e-9, "setSize kept on light (wider than the light)");
            // turnQuarter: a quarter from wherever it is, kept on, to front
            board.setSize(a, QSizeF(36, 24));
            board.turnQuarter(a, -1);
            checkNear(board.item(a)->rot, -M_PI / 2, 1e-12, "turnQuarter right");
            check(fuzzy(board.item(a)->lying(), QSizeF(24, 36)), "turnQuarter lying");
            // setRotation stores the settled angle as given
            board.setRotation(a, 1.0);
            checkNear(board.item(a)->rot, 1.0, 1e-12, "setRotation exact");
            // moveTo keeps on the light
            board.moveTo(a, QPointF(1000, 0));
            check(board.item(a)->x <= 130.5 + 1e-9, "moveTo kept on");
            // move by so many millimetres
            lt::Board mover;
            const QUuid m = id(23);
            mover.place(m, QSizeF(36, 24), QPointF(0, 0));
            const QPointF before = QPointF(mover.item(m)->x, mover.item(m)->y);
            mover.move(m, QPointF(10, -5));
            check(fuzzy(QPointF(mover.item(m)->x, mover.item(m)->y),
                        before + QPointF(10, -5)),
                  "move by");
            // orient / relayout no-op and flip
            board.orient(false);
            check(!board.portrait, "orient same no-op");
            board.orient(true);
            check(board.portrait, "orient flips");
            board.orient(false);
        }

        // --- the 99 limit ----------------------------------------------------
        {
            lt::Board board;
            for (int i = 0; i < 99; ++i) {
                if (!board.place(id(100 + i), QSizeF(36, 24), QPointF(0, 0))) {
                    check(false, "99 films all place");
                    break;
                }
            }
            check(board.isFull(), "99 films full");
            check(board.order.size() == 99, "pile of 99");
            check(!board.place(id(300), QSizeF(36, 24), QPointF(0, 0)), "100th refused");
            check(board.order.size() == 99, "refusal changed nothing");
            // an item already known still re-fronts past full (place's first arm)
            check(board.place(id(100), QSizeF(36, 24)), "known film re-fronts");
        }

        // --- hit and onLight --------------------------------------------------
        {
            lt::Board board;
            const QUuid a = id(40), b = id(41);
            board.place(a, QSizeF(36, 24), QPointF(-50, 0));
            board.place(b, QSizeF(36, 24), QPointF(50, 0));
            // lay them over by hand (place never overlaps when a free spot exists)
            board.moveTo(a, QPointF(0, 0));
            board.moveTo(b, QPointF(5, 0)); // overlaps a; b placed last is on top
            check(board.hit(QPointF(5, 0)) == b, "hit top first");
            check(board.hit(QPointF(-15, 0)) == a, "hit the lower film clear of the top");
            check(board.hit(QPointF(20.4, 12.4)) == b, "hit half a millimetre round it");
            check(board.hit(QPointF(25, 12.6)) == std::nullopt, "hit misses past half mm");
            check(board.onLight(QPointF(148.5 + 2.6, 0)), "onLight takes the rim");
            check(!board.onLight(QPointF(151.2, 0)), "onLight refuses past the rim");
        }

        // --- tidy: rows in the order given, centred, clear of the band -------
        {
            lt::Board board;
            const QUuid a = id(50), b = id(51);
            board.place(a, QSizeF(36, 24), QPointF(-50, 0));
            board.place(b, QSizeF(36, 24), QPointF(50, 0));
            board.tidy({b, a}); // b first: the rows follow the given order
            check(board.order.size() == 2 && board.order.front() == b,
                  "tidy pile becomes the laid order");
            // one row of two 36-wide films, centred across the light's 284 mm
            // of usable width (left -138.5, right 145.5, centre 3.5)
            checkNear(board.item(b)->x, -17.5, 1e-9, "tidy first x");
            checkNear(board.item(a)->x, 24.5, 1e-9, "tidy second x");
            checkNear(board.item(b)->y, -3.5, 1e-9, "tidy row y");
            checkNear(board.item(b)->rot, 0.0, 1e-12, "tidy lays square");
            check(board.tidied.has_value(), "tidy remembers");
        }

        // --- random and scatter: one seed, one table -------------------------
        {
            lt::Board board;
            for (int i = 0; i < 5; ++i) board.place(id(60 + i), QSizeF(36, 24), QPointF(0, 0));
            lt::Board one = board;
            one.random(42);
            check(!one.tidied.has_value(), "random forgets tidy");
            bool allSquare = true;
            for (const auto& it : one.items) allSquare = allSquare && it.rot == 0.0;
            check(allSquare, "random lays square");
            lt::Board two = board;
            two.random(42);
            check(sameBoard(one, two), "random seed decides");
            lt::Board three = board;
            three.random(43);
            check(!sameBoard(one, three), "another seed another table");
            lt::Board four = board;
            four.scatter(7);
            lt::Board five = board;
            five.scatter(7);
            check(sameBoard(four, five), "scatter seed decides");
            bool nearQuarter = true;
            for (const auto& it : four.items)
                nearQuarter = nearQuarter && std::abs(it.rot) <= 9 * M_PI / 180 + 1e-12;
            check(nearQuarter, "scatter turns a little about square");
        }

        // --- relayout: tidied re-tidies, carried carries, nothing lost -------
        {
            lt::Board board;
            const QUuid a = id(70), b = id(71);
            board.place(a, QSizeF(36, 24), QPointF(-50, 0));
            board.place(b, QSizeF(36, 24), QPointF(50, 0));
            board.tidy({a, b});
            board.relayout(lt::Paper::A6, false);
            check(board.paper == lt::Paper::A6, "relayout paper set");
            check(board.tidied.has_value(), "a tidied table tidied again");
            check(board.order.size() == 2 && board.item(a)->on && board.item(b)->on,
                  "relayout loses nothing");
            // an arrangement (not tidied) is carried as one: an axis too wide is
            // drawn together, all by one share, and the order along it kept
            lt::Board board2;
            board2.place(a, QSizeF(36, 24), QPointF(-120, 0));
            board2.place(b, QSizeF(36, 24), QPointF(120, 0));
            const double beforeSpan = board2.item(b)->x - board2.item(a)->x;
            board2.relayout(lt::Paper::A6, false);
            const double afterSpan = board2.item(b)->x - board2.item(a)->x;
            check(afterSpan < beforeSpan, "carried arrangement drawn together on a small table");
            check(afterSpan > 0, "carried arrangement keeps its order");
            check(board2.order.size() == 2, "carried arrangement keeps both");
            // an arrangement with room is carried shape for shape: a table with
            // room turned and turned back is as it was
            lt::Board board3;
            board3.place(a, QSizeF(36, 24), QPointF(-50, 0));
            board3.place(b, QSizeF(36, 24), QPointF(50, 0));
            const QPointF aBefore = QPointF(board3.item(a)->x, board3.item(a)->y);
            board3.relayout(lt::Paper::A6, false);
            board3.relayout(lt::Paper::A4, false);
            board3.relayout(lt::Paper::A6, false);
            check(fuzzy(aBefore, QPointF(board3.item(a)->x, board3.item(a)->y), 1e-6),
                  "turn and turn back");
        }

        // --- the viewport -----------------------------------------------------
        {
            lt::Viewport v;
            const bool moved = v.measure(QSizeF(800, 600), lt::Viewport::Insets(), lt::halfSize(lt::Paper::A4, false));
            check(moved, "measure reports change");
            const double boxW = 2 * (148.5 + lt::Lt::rim + lt::Lt::desk);
            const double boxH = 2 * (105 + lt::Lt::rim + lt::Lt::desk);
            checkNear(v.sFit, std::min((800 - 32) / boxW, (600 - 64) / boxH), 1e-9, "fit math");
            checkNear(v.s(), v.sFit, 1e-12, "s is sFit at zoom 1");
            check(!v.isZoomed(), "zoom 1 not zoomed");
            // toScreen/toWorld round trip
            const QPointF w(30, -20);
            check(fuzzy(v.toWorld(v.toScreen(w)), w, 1e-9), "screen world round trip");
            // the anchor zoom: the world point stays under the screen point
            v.zoomAt(2.0, w, QPointF(400, 300));
            checkNear(v.zoom, 2.0, 1e-12, "zoomAt sets zoom");
            check(fuzzy(v.toWorld(QPointF(400, 300)), w, 1e-9), "zoomAt anchors");
            check(fuzzy(v.toScreen(w), QPointF(400, 300), 1e-9), "the anchor is kept still");
            // the limits: 1..8, widened only while Actual Size needs it
            v.zoomAt(100, w, QPointF(400, 300));
            checkNear(v.zoom, lt::Lt::zoomMax, 1e-12, "zoom ceiling 8");
            v.zoomAt(0.1, w, QPointF(400, 300));
            checkNear(v.zoom, 1.0, 1e-12, "zoom floor 1");
            v.zoomFloor = 0.5;
            v.zoomCeiling = 12;
            v.zoomAt(12, w, QPointF(400, 300));
            checkNear(v.zoom, 12, 1e-12, "widened ceiling holds");
            v.zoomFloor = 1;
            v.zoomCeiling = lt::Lt::zoomMax;
            v.zoomAt(1.5, w, QPointF(400, 300));
            check(v.isSettled(), "within limits settled");
            // setZoom about the middle; a huge pan is clamped back into the view
            v.setZoom(2);
            checkNear(v.zoom, 2, 1e-12, "setZoom");
            v.pan(QPointF(v.cx, v.cy), QPointF(1e6, 0));
            check(v.isSettled(), "pan clamped");
            // the rubber band: continuous past the limits, same way back
            const double gave = lt::Viewport::band(0.7, lt::Lt::zoomGive);
            check(gave > 0 && gave < lt::Lt::zoomGive, "band gives less than the ask");
            checkNear(lt::Viewport::unband(gave, lt::Lt::zoomGive), 0.7, 1e-9, "unband inverts");
            const double held = lt::Viewport::rubber(12.0, 1.0, 8.0, lt::Lt::zoomGive);
            check(held > 8.0 && held < 8.0 + lt::Lt::zoomGive + 1e-9,
                  "the most past 8x is e^0.35 in log space");
            checkNear(lt::Viewport::unrubber(held, 1.0, 8.0, lt::Lt::zoomGive), 12.0, 1e-9,
                      "unrubber inverts");
            // hold: the zoom past the ceiling gives, settled brings it back
            lt::Viewport h;
            h.measure(QSizeF(800, 600), lt::Viewport::Insets(), lt::halfSize(lt::Paper::A4, false));
            h.hold(100, QPointF(0, 0), QPointF(400, 300));
            check(h.zoom > lt::Lt::zoomMax, "hold gives past the ceiling");
            check(h.zoom < std::exp(std::log(lt::Lt::zoomMax) + lt::Lt::zoomGive) + 1e-9,
                  "hold's give is bounded");
            const lt::Viewport s = h.settled(QPointF(400, 300));
            check(s.zoom <= lt::Lt::zoomMax + 1e-12, "settled back within the ceiling");
            // measure again, same everything: nothing moves
            check(!h.measure(QSizeF(800, 600), lt::Viewport::Insets(),
                             lt::halfSize(lt::Paper::A4, false)),
                  "measure idempotent");
            // a new size keeps the centre's world point
            const QPointF centreWorld = h.toWorld(QPointF(400, 300));
            h.measure(QSizeF(1000, 700), lt::Viewport::Insets(), lt::halfSize(lt::Paper::A4, false));
            check(fuzzy(h.toWorld(QPointF(500, 350)), centreWorld, 1e-6),
                  "measure keeps the middle's world point");
            check(h.lensRadius() == 102 && h.lensRim() == 7, "loupe lens wide window");
        }

        // --- Actual Size: the display's own numbers, guarded ------------------
        {
            check(!lt::ActualSize::pxPerMM(0, 0, 1).has_value(), "no EDID, no 1:1");
            check(!lt::ActualSize::pxPerMM(96, 96, 0).has_value(), "no scale, no 1:1");
            // a projector's 0.25 px a mm is refused at the bound itself
            check(!lt::ActualSize::pxPerMM(6.35, 6.35, 1).has_value(), "0.25 px/mm refused");
            check(lt::ActualSize::pxPerMM(12.7, 12.7, 1).has_value(), "0.5 px/mm taken");
            // a panel past 20 px a mm is a bad EDID
            check(!lt::ActualSize::pxPerMM(600, 600, 1).has_value(), "23.6 px/mm refused");
            check(lt::ActualSize::pxPerMM(500, 500, 1).has_value(), "19.7 px/mm taken");
            // axes that disagree by more than 5 percent are not a real size
            check(!lt::ActualSize::pxPerMM(96, 110, 1).has_value(), "axes disagree refused");
            check(lt::ActualSize::pxPerMM(96, 100, 1).has_value(), "axes within 5 percent");
            // points a millimetre = device px a mm / devicePixelRatio
            const double v = lt::ActualSize::pxPerMM(96, 96, 1).value();
            checkNear(v, 96 / 25.4, 1e-9, "96 dpi is 3.78 px a mm");
            const double hidpi = lt::ActualSize::pxPerMM(192, 192, 2).value();
            checkNear(hidpi, 96 / 25.4, 1e-9, "devicePixelRatio divides out");
        }

        // --- admission: developed || filmEdge.active || pair ------------------
        {
            check(!lt::Carry::takes(false, false, false), "plain undeveloped refused");
            check(lt::Carry::takes(true, false, false), "developed taken");
            check(lt::Carry::takes(false, true, false), "film edge on taken");
            check(lt::Carry::takes(false, false, true), "pair taken");
            check(lt::Carry::takes(true, true, true), "all taken");
        }

        // --- the true-mm frame sizes (never guessed) --------------------------
        {
            lt::Carry::Spec spec;
            // a half-frame pair: the piece, two 18x24 holes and the gap
            spec.pair = true;
            spec.pairSpacingMM = 1.0;
            spec.filmFormatMM = 37;
            const QSizeF piece = lt::Carry::frameSizeMM(spec).value();
            check(fuzzy(piece, QSizeF(37, 24)), "pair piece along x");
            spec.pairSpacingMM = 2.85;
            check(fuzzy(lt::Carry::frameSizeMM(spec).value(), QSizeF(38.85, 24)),
                  "pair piece follows the gap");
            spec.pairTurned = true;
            check(fuzzy(lt::Carry::frameSizeMM(spec).value(), QSizeF(24, 38.85)),
                  "turned pair swaps");
            spec.pair = false;
            spec.pairTurned = false;
            // film edge on, strip 135: the film's width across, the gate along
            spec.filmEdgeActive = true;
            spec.filmEdgeMode = "strip";
            spec.filmEdgeFormat = "135";
            spec.originalPx = QSizeF(3600, 2400);
            const QSizeF strip = lt::Carry::frameSizeMM(spec).value();
            checkNear(strip.height(), 35.0, 0.05, "135 strip across is the film width");
            checkNear(strip.width(), 36.25 + 2 * 0.95, 0.05, "135 strip along is gate+2 margins");
            // filed: the gate and about 1.1 mm of carrier
            spec.filmEdgeMode = "filed";
            const QSizeF filed = lt::Carry::frameSizeMM(spec).value();
            checkNear(filed.width(), 36.25 + 2 * 1.0, 0.05, "135 filed along");
            checkNear(filed.height(), 2400.0 * (36.25 / 3600.0) + 2.2, 0.05,
                      "135 filed across (the picture's own px + the carrier)");
            // a format the table does not know: no size, not a guess
            spec.filmEdgeFormat = "unknown_format";
            check(!lt::Carry::frameSizeMM(spec).has_value(), "unknown format refused");
            spec.filmEdgeFormat.clear();
            check(!lt::Carry::frameSizeMM(spec).has_value(), "no format refused");
            spec.filmEdgeActive = false;
            // bare developed: the shown picture at the uncut frame's mm a pixel
            spec.developed = true;
            spec.filmFormatMM = 36;
            spec.originalPx = QSizeF(3000, 2000);
            spec.displayPx = QSizeF(3000, 2000);
            check(fuzzy(lt::Carry::frameSizeMM(spec).value(), QSizeF(36, 24)), "bare full frame");
            spec.displayPx = QSizeF(1500, 2000);
            check(fuzzy(lt::Carry::frameSizeMM(spec).value(), QSizeF(18, 24)),
                  "bare half crop at the same mm a pixel");
            spec.filmFormatMM = 0;
            check(!lt::Carry::frameSizeMM(spec).has_value(), "no film mm refused");
            spec.filmFormatMM = 36;
            spec.originalPx = QSizeF(0, 0);
            check(!lt::Carry::frameSizeMM(spec).has_value(), "no pixels refused");
            spec.developed = false;
            check(!lt::Carry::frameSizeMM(spec).has_value(), "nothing known refused");
        }

        // --- lighttable.json: the stored shape and the restore ----------------
        {
            lt::Board board;
            const QUuid a = id(80), b = id(81);
            board.place(a, QSizeF(36, 24), QPointF(-20, 10));
            board.place(b, QSizeF(36, 24), QPointF(30, 0));
            board.takeOff(b);
            const lt::StoredTable stored = lt::StoredTable::from(board);
            const QJsonDocument doc = stored.toJson();
            const QJsonObject root = doc.object();
            check(root.value("schemaVersion").toInt() == 2, "schemaVersion 2");
            check(root.value("portrait").toBool() == false, "portrait stored");
            check(root.value("paper").toString() == "a4", "paper stored");
            const QJsonArray places = root.value("places").toArray();
            check(places.size() == 2, "two places");
            bool sawA = false, sawB = false;
            for (const auto& v : places) {
                const QJsonObject p = v.toObject();
                check(p.contains("frame") && p.contains("on") && p.contains("x") &&
                          p.contains("y") && p.contains("rot") && p.contains("z"),
                      "place keys");
                check(!p.contains("w") && !p.contains("h") && !p.contains("size"),
                      "frame sizes never stored");
                if (p.value("frame").toString() == a.toString(QUuid::WithoutBraces)) {
                    sawA = true;
                    check(p.value("on").toBool(), "a on stored");
                    check(p.value("z").toInt() >= 0, "a has pile place");
                }
                if (p.value("frame").toString() == b.toString(QUuid::WithoutBraces)) {
                    sawB = true;
                    check(!p.value("on").toBool(), "b off stored");
                    check(p.value("z").toInt() == -1, "b off pile place");
                }
            }
            check(sawA && sawB, "both frames stored");
            const QByteArray bytes = doc.toJson(QJsonDocument::Compact);
            const std::optional<lt::StoredTable> back = lt::StoredTable::parse(bytes);
            check(back.has_value(), "round trip parses");
            check(back->places.size() == 2 && !back->portrait &&
                      back->paper.value_or(lt::Paper::A6) == lt::Paper::A4,
                  "round trip fields");
            // restore: the pile in its order, each where it lay, sizes from the library
            const lt::Board got = back->restore(
                lt::Paper::A4, false, [](QUuid want) -> std::optional<QSizeF> {
                    if (want == id(80)) return QSizeF(36, 24);
                    if (want == id(81)) return QSizeF(36, 24);
                    return std::nullopt;
                });
            check(got.item(a) && got.item(a)->on, "restore a on");
            checkNear(got.item(a)->x, -20, 1e-6, "restore a x");
            checkNear(got.item(a)->y, 10, 1e-6, "restore a y");
            check(got.item(b) && !got.item(b)->on, "restore b off");
            // a frame the library dropped is left out
            const lt::Board less = back->restore(
                lt::Paper::A4, false,
                [](QUuid) -> std::optional<QSizeF> { return std::nullopt; });
            check(less.items.empty(), "dropped frames left out");
            // schema 1 (no paper): the A4 it was
            const QByteArray schema1 = R"({"places":[{"frame":")" +
                                       a.toString(QUuid::WithoutBraces).toUtf8() +
                                       R"(","on":true,"rot":0,"x":5,"y":6,"z":0}],"portrait":false,"schemaVersion":1})";
            const std::optional<lt::StoredTable> old = lt::StoredTable::parse(schema1);
            check(old.has_value() && old->keptPaper() == lt::Paper::A4, "schema 1 is A4");
            // corrupt: parse fails, the caller sets the file aside
            check(!lt::StoredTable::parse(QByteArray("{not json")).has_value(), "corrupt refused");
            check(!lt::StoredTable::parse(QByteArray("{\"schemaVersion\":9}")).has_value(),
                  "unknown schema refused");
            check(!lt::StoredTable::parse(QByteArray("{\"schemaVersion\":2,\"paper\":\"x4\",\"portrait\":false,\"places\":[]}"))
                       .has_value(),
                  "unknown paper refused");
        }

        // --- TableModel: the QML-facing store over a temp directory ----------
        {
            QTemporaryDir dir;
            check(dir.isValid(), "temp dir");
            const QString path = dir.filePath("lighttable.json");

            lt::TableModel first;
            first.open(path);
            QVariantList frames;
            frames.append(QVariantMap{{"uuid", id(90).toString()},
                                      {"mmw", 36.0},
                                      {"mmh", 24.0}});
            frames.append(QVariantMap{{"uuid", id(91).toString()},
                                      {"mmw", 43.0},
                                      {"mmh", 24.0}});
            const int placed = first.carry(frames);
            check(placed == 2, "carry places both");
            check(first.rowCount() == 2, "two rows");
            check(first.filmCount() == 2, "filmCount");
            first.close(); // saveNow, atomic

            check(QFile::exists(path), "file written");
            lt::TableModel second;
            second.open(path);
            check(second.rowCount() == 2, "restored rows");
            check(second.filmCount() == 2, "restored pile");
            check(fuzzy(QPointF(second.itemX(id(90).toString()), second.itemY(id(90).toString())),
                        QPointF(first.itemX(id(90).toString()), first.itemY(id(90).toString())), 1e-6),
                  "restored where laid");
            // a frame's size is re-read from the library, never the file
            // (sync carries the library's whole admitted list)
            QVariantList grown;
            grown.append(QVariantMap{{"uuid", id(90).toString()}, {"mmw", 84.0}, {"mmh", 56.0}});
            grown.append(QVariantMap{{"uuid", id(91).toString()}, {"mmw", 43.0}, {"mmh", 24.0}});
            second.sync(grown);
            check(fuzzy(QSizeF(second.itemW(id(90).toString()), second.itemH(id(90).toString())),
                        QSizeF(84, 56)),
                  "sync re-reads the size");
            // a frame the library dropped is taken off at open
            QVariantList dropped;
            dropped.append(QVariantMap{{"uuid", id(91).toString()}, {"mmw", 43.0}, {"mmh", 24.0}});
            second.sync(dropped); // the library now offers only id91
            check(second.rowCount() == 1, "dropped frame forgotten");
            second.close();

            // the debounced write: soon after the last change, not at once
            lt::TableModel third;
            third.open(path);
            QVariantList more;
            more.append(QVariantMap{{"uuid", id(92).toString()}, {"mmw", 36.0}, {"mmh", 24.0}});
            third.carry(more);
            const QByteArray before = [&] {
                QFile f(path);
                f.open(QIODevice::ReadOnly);
                return f.readAll();
            }();
            QEventLoop loop;
            QTimer::singleShot(700, &loop, &QEventLoop::quit);
            loop.exec();
            const QByteArray after = [&] {
                QFile f(path);
                f.open(QIODevice::ReadOnly);
                return f.readAll();
            }();
            check(after != before, "the debounced write landed");
            check(after.contains(id(92).toString(QUuid::WithoutBraces).toUtf8()),
                  "debounced write has the frame");

            // a corrupt file is set aside, never written over
            QFile corrupt(path);
            corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate);
            corrupt.write("{ half a json");
            corrupt.close();
            lt::TableModel fourth;
            fourth.open(path);
            check(fourth.rowCount() == 0, "corrupt opens empty");
            const QFileInfo info(path);
            const QStringList entries =
                QDir(info.absolutePath())
                    .entryList(QStringList() << info.completeBaseName() + ".unreadable-*",
                               QDir::Files);
            check(entries.size() >= 1, "corrupt set aside");
            bool sameBytes = false;
            for (const QString& e : entries) {
                QFile aside(dir.filePath(e));
                aside.open(QIODevice::ReadOnly);
                sameBytes = sameBytes || aside.readAll() == QByteArray("{ half a json");
            }
            check(sameBytes, "the aside holds the unreadable bytes");
        }

        std::printf("lighttable: %d checks passed\n", checks);
        return 0;
    } catch (const std::exception& e) {
        std::printf("lighttable: FAIL after %d checks: %s\n", checks, e.what());
        return 1;
    }
}
