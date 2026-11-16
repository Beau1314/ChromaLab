#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QTimer>
#include <QUuid>
#include <QVariantList>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

// The light table (task 6.4) — the Windows port of the Mac's Light Table
// (RFC-045): an A4 (or A5/A6) light pad seen straight down, measured in
// millimetres, with the library's frames lying on it at their true size.
//
// The arithmetic core is a **statement-for-statement port** of
// app/SpektraLab/Sources/LightTable/LightTableModel.swift, so a film put down
// here lies where the website (spektralab.app/lighttable, table.js) and the
// Mac would put it. Units are the Mac's: millimetres on the table, x right and
// y **up** (the screen's y is down — the flip happens at the viewport), the
// origin the middle of the lit area; angles radians counter-clockwise, any
// value, degrees only as the UI reads them. `tests/lighttable.cpp` holds the
// core against the same numbers the Mac's tools/lighttable_selftest.swift
// holds.
//
// What is ported here, by name (the Mac's own names):
//   * `LightTable`      -> Lt (the table's numbers) and the free functions
//                          jsRound/settle/unwind/degrees/mm/half
//   * `LightTablePaper` -> Paper (A4, A5, A6; each half the one before)
//   * `LightTableItem`  -> Item (id, size, x, y, rot, on; quarter/lying/extent)
//   * `LightTableModel` -> Board (the pile, placing, turning, keeping on the
//                          light, freeSpot/leastCovered/carried, tidy/random/
//                          scatter, relayout, hit, onLight)
//   * `LightTableViewport` -> Viewport (s = sFit x zoom, zoom 1-8 widened only
//                          while Actual Size needs it, the anchor zoomAt, pan,
//                          clamp, and the rubber band's e^0.35 give)
//   * `MacActualSize`   -> ActualSize (points a millimetre from the display's
//                          EDID, with the guards: no EDID, no 1:1; axes that
//                          disagree by more than 5 %, no 1:1; outside
//                          0.25-20 px/mm, no 1:1)
//   * `MacLightTableCarry.takes` -> Carry::takes (the one admission filter:
//                          developed || filmEdge.active || pair)
//   * `LightTableSizing`-> Carry::frameSizeMM (the true millimetres, never
//                          guessed: a film edge's canvas estimate over the
//                          engine's own format table, a half-frame pair's
//                          piece, or a bare develop at its film_format_mm at
//                          the shown aspect; anything else does not qualify)
//   * `StoredTable` + `LightTableStore` -> StoredTable + TableModel's store
//                          (schema 2: {schemaVersion, portrait, paper,
//                          places:[{frame,on,x,y,rot,z}]}, written 300 ms
//                          after the last change of a burst, atomically, and
//                          at once when the page closes; a file this build
//                          cannot read is set aside beside itself as
//                          `<stem>.unreadable-<seconds>.<ext>` and never
//                          written over; **frame sizes are never stored** —
//                          they are the library's, re-read at every open)
//   * `LightTableRandom`-> Random (SplitMix64: one seed, one table)
//
// What is deliberately NOT here (each with where it went instead):
//   * `LightTableShown`'s easing and `TableGlide` (the Mac's animation of a
//     film towards where it is to be): the QML page animates its delegates —
//     the arithmetic the film settles AT is all in Board.
//   * `LightTableSteps` (the GPU texture budget) and `LightTableTextures`:
//     this page reads the library's own thumbnail store (the same decodes the
//     grid shows) and falls back to the film's true-mm silhouette while a
//     decode is not cached yet. The Mac's kept-develop full-band ladder is
//     Mac-only scope.
//   * A half-frame pair's strip/filed margins (LightTableSizing.pair's
//     filmEdge arm): the Windows frontend draws no film edge on a pair, so a
//     pair lies as its piece. The library join cannot see pair membership
//     anyway (a pair is a develop-page record keyed by two sources, not a
//     library row), so `Carry::takes`'s pair arm is wired for the filter's
//     completeness and carries no library rows today.
//   * The Click tool (pick up, click to put down) and the touch gestures: a
//     mouse drags; the page turns films with its buttons and menu, as the Mac
//     turns them with two fingers.
namespace spk::desktop::lighttable {

// --- the table's size --------------------------------------------------------
// ISO 216, each half the one before. A film keeps its own millimetres on every
// size, so on a smaller table it lies larger in the view: the table is fitted
// to the view, the films are not.
enum class Paper { A4, A5, A6 };

/// The name as stored in lighttable.json and printed on a box of paper.
QString paperName(Paper paper);
/// The stored name back ("a4"/"a5"/"a6"); nullopt for anything else.
std::optional<Paper> paperFromName(const QString& name);
/// Landscape, mm: 297 x 210, 210 x 148, 148 x 105.
QSizeF paperSize(Paper paper);

/// The lit area's half size, mm, at a paper size: landscape, or turned to portrait.
QSizeF halfSize(Paper paper, bool portrait);

// --- the table's numbers (table.js lines 34-50 and table-gl.js' loupe) -----
inline double clampd(double v, double a, double b) { return std::min(b, std::max(a, v)); }

struct Lt {
    /// The lit area, ISO 216 A4 landscape: 297 x 210 mm.
    static constexpr double a4Width = 297.0;
    static constexpr double a4Height = 210.0;
    /// The aluminium frame round it, mm.
    static constexpr double rim = 2.6;
    /// The lit area's corner radius, mm.
    static constexpr double corner = 3.0;
    /// Desk in view round the frame when the table fits the window, mm.
    static constexpr double desk = 5.0;
    /// The scale's band along the top and left edges, which placing keeps clear of, mm.
    static constexpr double clear = 10.0;
    /// Between films placed for the user, mm.
    static constexpr double gap = 3.0;
    /// The quad beyond a film's cut edge, for its anti-aliasing, mm.
    static constexpr double pad = 0.8;
    static constexpr double zoomMax = 8.0;
    /// As many films as the table holds (the owner: "the maximum allowed
    /// images onto the table is simply 99"); a 100th is refused.
    static constexpr double films = 99.0;

    /// A film's shadow lying on the diffuser (`rest`), and held a few
    /// millimetres above it (`lift`): offset x, y, blur, alpha.
    struct Shadow {
        double x, y, blur, alpha;
        bool operator==(const Shadow&) const = default;
    };
    static constexpr Shadow rest{0.5, -1.1, 2.4, 0.16};
    static constexpr Shadow lift{2.4, -4.6, 6.5, 0.2};

    static constexpr double quarter = 3.14159265358979323846 / 2;
    static constexpr double deg = 3.14159265358979323846 / 180;
    /// A free turn settles square within `snap` of a quarter turn —
    /// `snapTouch` under fingers — and goes in `step`s where shift asks.
    static constexpr double snap = 4 * deg;
    static constexpr double snapTouch = 7 * deg;
    static constexpr double step = 15 * deg;

    /// The loupe's magnification, and the half-extent of what it is shown.
    static constexpr double mag = 3.0;
    static constexpr double lensPad = 1.5;

    /// The most the zoom gives past 1x and 8x under the fingers, in its logarithm.
    static constexpr double zoomGive = 0.35;

    /// The small turn Scatter gives each film about its own quarter, either way.
    static constexpr double scatterTurn = 9 * deg;
};

/// JavaScript's Math.round: halves go up, towards +infinity. C++'s floor(v+0.5)
/// is exactly that (std::round would take a negative half away from zero,
/// which differs: a turn of -135 degrees settles to a different quarter by it).
/// Every rounding the website does goes through this.
double jsRound(double v);

/// An angle settled square when it comes near a quarter turn, or to 15-degree
/// steps (`settle`).
double settle(double a, bool steps = false, double snap = Lt::snap);

/// The website's `unwind`: an angle step brought into -pi..pi, so a turn that
/// crosses atan2's seam keeps counting the same way.
double unwind(double d);

/// Degrees as a person reads them: -180 to 180, a decimal only off the square.
QString degreesText(double a);

/// Millimetres as the website writes a size: one decimal.
QString mmText(double v);

// --- a seeded random ----------------------------------------------------------
/// SplitMix64: a small random whose whole sequence is decided by its seed.
struct Random {
    explicit Random(quint64 seed) : state(seed) {}
    quint64 next();
    /// 0 <= u < 1.
    double unit();

private:
    quint64 state;
};

// --- a film -------------------------------------------------------------------
/// One library frame on (or off) the table. `size` is the frame's own, in
/// millimetres, as its picture lies unturned: never stored as the truth — it
/// is the library's, and read again whenever the frame changes.
struct Item {
    Item() = default;
    Item(QUuid id, QSizeF size, double x = 0.0, double y = 0.0)
        : id(std::move(id)), size(size), x(x), y(y) {}

    QUuid id;
    QSizeF size;
    double x = 0.0;
    double y = 0.0;
    /// Radians, any angle.
    double rot = 0.0;
    bool on = false;

    bool operator==(const Item&) const = default;

    double w() const { return size.width(); }
    double h() const { return size.height(); }
    /// Its nearest quarter turn, 0-3 (`quarter`).
    int quarter() const;
    /// Its width and height as it lies, by that quarter (`lying`).
    QSizeF lying() const;
    /// The box it takes up on the table at its own angle (`extent`).
    QSizeF extent() const;
    QPointF centre() const { return QPointF(x, y); }
};

// --- the table -----------------------------------------------------------------
/// The films and their pile. `order` is the pile on the table, bottom to top:
/// the film last put down, moved or turned lies on top, takes a touch first,
/// and casts its shadow on the ones below. `items` holds every frame the table
/// has been given, on it or taken off it (a film taken off waits, with its
/// place forgotten, to be put back).
struct Board {
    using ItemList = std::vector<Item>;

    ItemList items;
    std::vector<QUuid> order;
    /// The table turned to portrait: the person's choice, never the host's own way up.
    bool portrait = false;
    Paper paper = Paper::A4;
    /// The order Tidy Up last laid the films in, while they still lie as it
    /// laid them. Anything that moves, turns, puts down or takes off a film forgets it.
    std::optional<std::vector<QUuid>> tidied;

    QSizeF half() const { return halfSize(paper, portrait); }
    bool isFull() const { return double(order.size()) >= Lt::films; }
    /// The films on the table, bottom to top.
    ItemList pile() const;
    const Item* item(QUuid id) const;
    Item* item(QUuid id);

    // --- placing ---------------------------------------------------------------
    /// The free spot nearest `at` where a w x h film lies clear of the scale
    /// and of `others` (the films on the table, unless told others):
    /// `freeSpot`. The grid search walks 2.5 mm steps from the band's corner.
    QPointF freeSpot(double w, double h, QPointF at,
                     const ItemList* others = nullptr) const;
    /// Where a film overlaps the films on the table least, on the light, in
    /// 5 mm steps; nearest `at` among equals. Each overlap counts by its
    /// square, so a film overlaps several a little rather than one whole.
    QPointF leastCovered(const Item& it, QPointF at,
                         const ItemList* others = nullptr) const;
    /// All of it on the light: off the box a film would read the frame through
    /// itself (`keepOn`).
    Item keptOn(Item it) const;

    /// What the table remembered of a film: its centre, its angle, and the
    /// table it lay on then — which way it was turned and its size.
    struct Kept {
        double x;
        double y;
        double rot;
        bool portrait;
        Paper paper = Paper::A4;
    };

    /// A film's centre carried from a table of half size `was` to one of half
    /// size `now`, each axis in proportion to the room it has there.
    static QPointF carried(const Item& it, QSizeF was, QSizeF now);
    /// The films on the table carried to a table of half size `now` **as one
    /// arrangement**: what the person laid out keeps its shape where it fits.
    static QHash<QUuid, QPointF> carried(const ItemList& films, QSizeF was, QSizeF now);

    /// Onto the table (`place`): at the free spot nearest `at`, square; or
    /// where it lay last time (`kept`), its place carried to the table as it
    /// is now. A film already on the table comes to the top. False when the
    /// table already holds Lt::films films.
    bool place(QUuid id, QSizeF size, std::optional<QPointF> at = std::nullopt,
               const Kept* kept = nullptr);
    /// Off the table (`takeOff`): it waits, with its place forgotten, to be put back.
    void takeOff(QUuid id);
    /// Out of the table for good: the library let go of its frame.
    void forget(QUuid id);
    /// To the top of the pile: a film put down, moved, turned or resized.
    void toFront(QUuid id);
    /// The frame's size, read again from the library: a film on the table is kept on the light.
    void setSize(QUuid id, QSizeF size);
    /// A quarter turn from wherever it is — `dir` -1 to the right, 1 to the
    /// left — as Photoshop's Rotate 90. A turned film may now reach past the
    /// light: its centre comes back in.
    void turnQuarter(QUuid id, int dir = -1);
    /// Its angle, already settled by whoever turns it.
    void setRotation(QUuid id, double rot);
    /// Its centre, as a finger carries it: kept on the light.
    void moveTo(QUuid id, QPointF p, bool keepOn = true);
    /// Moved by so many millimetres (the website's arrow keys).
    void move(QUuid id, QPointF delta);
    /// Kept on the light after a turn let go of (`turned`).
    void keepOn(QUuid id);
    /// The table turned to portrait or back (`orient`): relayout at the size it has.
    void orient(bool portrait);
    /// The table at another size, or turned the other way. **No film is left
    /// out, taken off or lost off the light**, whatever the new table's size:
    /// a table Tidy Up laid out is tidied again in the same order; any other
    /// table's films are carried as one arrangement, then — bottom of the pile
    /// first — a film carried over a film it was clear of before goes to the
    /// free spot nearest its carried place, or where it overlaps them least.
    void relayout(Paper paper, bool portrait);

    // --- tidying ---------------------------------------------------------------
    /// **Tidy Up**: every film laid square, in rows in the order given, each
    /// row centred across the light and the rows together centred down it,
    /// clear of the scale's band, Lt::gap x 2 between films. When they do not
    /// all fit, the rows close up the way slides are fanned on a light box:
    /// each step shortened by a share, so every film's top left corner shows
    /// and the overlap is spread evenly.
    void tidy(const std::vector<QUuid>& ids);
    /// **Random** (the aligned one): every film laid down again square, at the
    /// free spot nearest a random aim; where no free spot is left, where it
    /// overlaps least. One seed always gives the same table.
    void random(quint64 seed);
    /// **Scatter**: every film scattered as if dropped on the light box — a
    /// random place, turned a little about its quarter, the pile in a random
    /// order. One seed always gives the same scatter.
    void scatter(quint64 seed);

    /// The area two films' boxes on the table share, mm^2 (by their extents).
    static double overlap(const Item& a, const Item& b);

    // --- touching ---------------------------------------------------------------
    /// The film under a table point, top first (`hit`): within its turned
    /// rectangle and half a millimetre round it.
    std::optional<QUuid> hit(QPointF p) const;
    /// The website's `onLight`: a table point on the lit area or its frame.
    bool onLight(QPointF p) const;

private:
    void put(const Item& it);
};

// --- the view onto it -----------------------------------------------------------
/// The orthographic view: the table's point (cx, cy) in the middle of the
/// view, `s()` points a millimetre (`sFit x zoom`), and the room the page's
/// own controls leave round it (`inset`). The website's measureStage,
/// clampView, zoomAt, setZoom, toWorld and toScreen.
struct Viewport {
    struct Insets {
        double top = 48.0;
        double left = 16.0;
        double bottom = 16.0;
        double right = 16.0;
        bool operator==(const Insets&) const = default;
    };

    /// The view, points.
    QSizeF size{1.0, 1.0};
    Insets inset;
    double sFit = 1.0;
    double zoom = 1.0;
    double cx = 0.0;
    double cy = 0.0;
    /// The lit area's half size, mm.
    QSizeF half = halfSize(Paper::A4, false);
    bool measured = false;
    /// The zoom's limits: 1-8x, widened only while the window's Actual Size
    /// (1:1) needs a zoom outside them. The host never changes them otherwise.
    double zoomFloor = 1.0;
    double zoomCeiling = Lt::zoomMax;

    double s() const { return sFit * zoom; }
    bool isZoomed() const { return zoom > 1.01; }

    QPointF toWorld(QPointF p) const;
    QPointF toScreen(QPointF w) const;

    /// The table with its frame and the desk round it, mm (`box`).
    QSizeF box() const;

    void clampView();

    // --- under the fingers: the rubber band ------------------------------------
    /// The give past a limit (UIScrollView's rubber band): `o` asked past it,
    /// `d` the most it ever gives; continuous, and the same way back.
    static double band(double o, double d);
    static double unband(double b, double d);
    static double rubber(double x, double lo, double hi, double d);
    static double unrubber(double x, double lo, double hi, double d);

    /// The view as fingers hold it: the zoom they ask for, unclamped, with the
    /// table point `w` under the view point `p`. Past the limits it gives less
    /// and less rather than stopping; `settled` brings it back.
    void hold(double z, QPointF w, QPointF p);
    /// What `hold` would be given, with the fingers' middle at `p`: `hold`'s inverse.
    std::pair<double, QPointF> held(QPointF p) const;

    /// Within the table's limits: 1-8x, the table in the view.
    bool isSettled() const;
    /// Where the view comes back to when the fingers let go past a limit.
    Viewport settled(QPointF p) const;
    /// The zoom and the centre as given, unclamped: a step of the way back.
    void put(double z, double x, double y);

    /// Zoom so that the table point `w` sits under the view point `p`.
    void zoomAt(double z, QPointF w, QPointF p);
    /// The middle of the room the controls leave (`middle`).
    QPointF middle() const;
    /// Zoom about a view point, the middle when none is given (`setZoom`).
    void setZoom(double z, std::optional<QPointF> p = std::nullopt);
    /// Pan by a travel in points, clamped.
    void pan(QPointF c0, QPointF d);

    /// The view laid out (`measureStage`): the table fitted in the room the
    /// controls leave, the table point in the middle kept where it was. True
    /// when anything moved.
    bool measure(QSizeF newSize, Insets newInset, QSizeF newHalf);

    /// The loupe's lens radius and glass rim, points: the website's for a narrow window.
    double lensRadius() const { return size.width() < 600 ? 72 : 102; }
    double lensRim() const { return size.width() < 600 ? 5 : 7; }

private:
    double clampAxis(double c, double h, double view, double lo, double hi) const;
    std::pair<double, double> axisRange(double h, double view, double lo, double hi) const;
    std::pair<std::pair<double, double>, std::pair<double, double>> ranges() const;
};

// --- Actual Size ------------------------------------------------------------------
/// Points (logical pixels) a millimetre on a screen whose physical resolution
/// is `dpiX`/`dpiY` device pixels an inch (the EDID's numbers, QScreen's
/// physicalDotsPerInchX/Y) and whose device pixel ratio is `dpr`. The Mac's
/// guards, on the Qt numbers: no EDID (a dpi of 0 or less) has no 1:1; axes
/// that disagree by more than 5 % are not a real size; anything outside
/// 0.25-20 px/mm — a 100-inch projector to a past-panel EDID — is refused.
namespace ActualSize {
std::optional<double> pxPerMM(double dpiX, double dpiY, double dpr);
}

// --- what may lie on the table, and how big it is -----------------------------------
namespace Carry {

/// The one admission filter (`MacLightTableCarry.takes`): a frame of
/// SpektraLab's whose film's size is known — developed (every develop knows
/// the size its grain was made at), or carrying the size itself (a film edge
/// on, or a half-frame pair). A picture only listed, or a frame never
/// developed, has none.
bool takes(bool developed, bool filmEdgeActive, bool pair);

/// Everything a frame's size needs, gathered by the library join
/// (LibraryModel::lightTableFrames). Nothing here is guessed at.
struct Spec {
    bool developed = false;      // a develop record exists
    bool filmEdgeActive = false; // the record's overscan_active
    QString filmEdgeMode;        // "strip" | "filed"
    QString filmEdgeFormat;      // the engine's format id ("135", "120_6x17", ...)
    bool pair = false;           // a half-frame pair (no Windows library rows today)
    double pairSpacingMM = 1.0;  // the pair record's spacing
    bool pairTurned = false;     // the pair's holes stack down
    double filmFormatMM = 0.0;   // the develop's film_format_mm
    QSizeF displayPx;            // the shown picture's pixels (after crop and turns)
    QSizeF originalPx;           // the uncut frame's pixels
};

/// The frame's own size in millimetres, as its picture lies unturned
/// (`LightTableSizing`); nullopt when nothing here knows it — never a guess:
/// a pair lies as its piece; a film edge lies as the canvas estimate prices it
/// over the engine's own format table; a bare develop lies at its
/// film_format_mm at the shown aspect. A frame with no size does not qualify.
std::optional<QSizeF> frameSizeMM(const Spec& spec);

/// The engine's own table (`kFormats`, engine/src/pipeline/overscan.cpp):
/// film width, the gate across and along the film, perforated.
struct FilmFormat {
    const char* id;
    double filmWidthMM;
    double acrossMM;
    double alongMM;
    bool perforated;
};
/// The format by the engine's id; nullopt for an id the table does not know.
std::optional<FilmFormat> filmFormat(const QString& id);

} // namespace Carry

// --- the table as it is written -------------------------------------------------------
/// The table as it is written (the Mac's StoredTable, schema 2): each frame's
/// id and its place — centre, angle, place in the pile — and the table it lay
/// on. **A size is never written**: it is the frame's, read again from the
/// library at every open, so a format changed while the table was closed comes
/// back at its new size, kept on the light.
struct StoredTable {
    struct Place {
        QUuid frame;
        bool on;
        double x;
        double y;
        double rot;
        /// Its place in the pile, bottom 0; -1 off the table.
        int z;
        bool operator==(const Place&) const = default;
    };

    bool portrait = false;
    /// The table's size; nullopt in a file kept before there were sizes (schema 1), which was A4.
    std::optional<Paper> paper;
    std::vector<Place> places;

    static StoredTable from(const Board& board);
    /// The size it was kept at: A4 for a file from before there were sizes.
    Paper keptPaper() const { return paper.value_or(Paper::A4); }

    /// The keys as they are written; the object's keys sort as the Mac's
    /// sortedKeys encoder's do. Frame sizes appear nowhere.
    QJsonDocument toJson() const;
    /// nullopt for anything this build cannot read: bad JSON, a schema that is
    /// neither 1 nor 2, an unknown paper, a place without its keys, a bad id.
    static std::optional<StoredTable> parse(const QByteArray& bytes);

    /// The table laid out again from what was written (`restore`): the pile in
    /// its order, each film where it lay — carried in proportion to a table of
    /// another size or turned the other way — then those off the table,
    /// waiting. `size` is the library's answer for a frame; a frame it no
    /// longer has is left out.
    Board restore(Paper nowPaper, bool nowPortrait,
                  const std::function<std::optional<QSizeF>(QUuid)>& size) const;
    /// The board as the file remembers it, sizes unknown yet (0 x 0): the
    /// table opens at its own kept size, and the library's true millimetres
    /// arrive with the next sync (TableModel::sync). The pile in its order,
    /// then the ones off it.
    Board boardUnknownSizes() const;
};

// --- the QML-facing table --------------------------------------------------------------
/// The light table as QML sees it (task 6.4): the Board and its Viewport
/// behind a list model of films (roles: frame, on, x, y, rotDeg, w, h, image,
/// hasImage, z), the lighttable.json store (open/close, the 300 ms debounced
/// atomic write, the corrupt-file set-aside), and the invokables the page
/// drives (carry, sync, move, turn, zoom, Actual Size, tidy/random/scatter).
/// One instance per process, registered by main.cpp as `lightTable`.
class TableModel final : public QAbstractListModel {
    Q_OBJECT
    /// Bumped on every board or view change: QML bindings read it ahead of the
    /// derived values so a drag or a zoom re-positions every delegate at once
    /// (the Localization.epoch convention).
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged FINAL)
    Q_PROPERTY(int filmCount READ filmCount NOTIFY boardChanged FINAL)
    Q_PROPERTY(bool isFull READ isFull NOTIFY boardChanged FINAL)
    Q_PROPERTY(int offCount READ offCount NOTIFY boardChanged FINAL)
    Q_PROPERTY(QString paperName READ paperNameString NOTIFY boardChanged FINAL)
    Q_PROPERTY(bool portrait READ portrait WRITE setPortrait NOTIFY boardChanged FINAL)
    Q_PROPERTY(double zoom READ zoom NOTIFY viewChanged FINAL)
    Q_PROPERTY(bool isZoomed READ isZoomed NOTIFY viewChanged FINAL)
    /// The points a millimetre on the window's screen, or 0 when the display
    /// does not know its size (then there is no Actual Size button).
    Q_PROPERTY(double pointsPerMM READ pointsPerMM NOTIFY screenPropertyChanged FINAL)
    Q_PROPERTY(bool canActualSize READ canActualSize NOTIFY screenPropertyChanged FINAL)
    Q_PROPERTY(bool actualSizeActive READ actualSizeActive NOTIFY viewChanged FINAL)
    /// The number of films the last carry laid down (the Add flow's answer).
    Q_PROPERTY(int lastCarried READ lastCarried NOTIFY boardChanged FINAL)

public:
    explicit TableModel(QObject* parent = nullptr);

    // --- QAbstractListModel -----------------------------------------------------
    enum Role {
        FrameRole = Qt::UserRole + 1,
        OnRole,
        XRole,
        YRole,
        RotRole, // degrees, as the UI reads them
        WRole,
        HRole,
        ImageRole,    // QVariant(QImage); the library's cached thumbnail, may be null
        HasImageRole,
        ZRole // the pile place, bottom 0; -1 off the table
    };
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // --- properties ---------------------------------------------------------------
    int revision() const { return revision_; }
    int filmCount() const { return int(board_.order.size()); }
    bool isFull() const { return board_.isFull(); }
    int offCount() const;
    QString paperNameString() const { return paperName(board_.paper).toUpper(); }
    bool portrait() const { return board_.portrait; }
    void setPortrait(bool value);
    double zoom() const { return viewport_.zoom; }
    bool isZoomed() const { return viewport_.isZoomed(); }
    double pointsPerMM() const { return pointsPerMM_.value_or(0.0); }
    bool canActualSize() const { return pointsPerMM_.has_value(); }
    bool actualSizeActive() const { return actualSizeActive_; }
    int lastCarried() const { return lastCarried_; }

    // --- one film's numbers, by uuid (the page's per-film reads) -------------------
    Q_INVOKABLE double itemX(const QString& uuid) const;
    Q_INVOKABLE double itemY(const QString& uuid) const;
    Q_INVOKABLE double itemW(const QString& uuid) const;
    Q_INVOKABLE double itemH(const QString& uuid) const;
    Q_INVOKABLE double itemRot(const QString& uuid) const; // radians
    Q_INVOKABLE bool itemOn(const QString& uuid) const;

    // --- the store ------------------------------------------------------------------
    /// Reads lighttable.json; a file this build cannot read is set aside
    /// beside itself (never written over) and the table opens empty. Frame
    /// sizes stay unknown until the next sync/carry supplies the library's.
    Q_INVOKABLE void open(const QString& path);
    /// Written now: the table is closing.
    Q_INVOKABLE void close();
    /// The pending write flushed at once.
    Q_INVOKABLE void saveNow();

    // --- the library join -------------------------------------------------------------
    /// Lays the library's frames on the table (`MacLightTableCarry`): each
    /// entry {uuid, mmw, mmh, image?}; a frame already on the table comes to
    /// the top with its size read again. Returns how many were laid. The 99
    /// limit refuses the rest.
    Q_INVOKABLE int carry(const QVariantList& frames);
    /// The library changed: each known frame's size read again, kept on the
    /// light; a frame the library no longer offers (dropped, or without a
    /// size) is taken off the table for good.
    Q_INVOKABLE void sync(const QVariantList& frames);

    // --- the pile ----------------------------------------------------------------------
    Q_INVOKABLE void takeOff(const QString& uuid);
    /// A quarter turn: dir -1 to the right, 1 to the left.
    Q_INVOKABLE void turnQuarter(const QString& uuid, int dir);
    /// The angle, settled here (steps asks for 15-degree steps, snapDegrees
    /// the square's band — the gesture's `settle`).
    Q_INVOKABLE void rotateTo(const QString& uuid, double radians, bool steps = false,
                              double snapDegrees = 4.0);
    Q_INVOKABLE void moveTo(const QString& uuid, double x, double y);
    Q_INVOKABLE void tidyUp();
    Q_INVOKABLE void randomize(int seed);
    Q_INVOKABLE void scatterNow(int seed);
    Q_INVOKABLE void setPaper(const QString& name); // "A4" | "A5" | "A6"
    Q_INVOKABLE QString selectedUuid() const { return selected_; }
    Q_INVOKABLE void setSelected(const QString& uuid);

    // --- the view ----------------------------------------------------------------------
    Q_INVOKABLE QPointF toWorld(QPointF p) const;
    Q_INVOKABLE QPointF toScreen(QPointF w) const;
    /// Points a millimetre right now (sFit x zoom).
    Q_INVOKABLE double scaleFactor() const { return viewport_.s(); }
    /// The view laid out in the room the page's controls leave.
    Q_INVOKABLE void measureView(double width, double height, double top, double left,
                                 double bottom, double right);
    Q_INVOKABLE void zoomAt(double z, QPointF world, QPointF screen);
    Q_INVOKABLE void setZoom(double z);
    Q_INVOKABLE void zoomIn();
    Q_INVOKABLE void zoomOut();
    Q_INVOKABLE void fitView();
    /// Reads the window's screen (its EDID numbers and device pixel ratio) and
    /// works out the points a millimetre; no EDID, no Actual Size.
    Q_INVOKABLE void attachWindow(QObject* window);
    /// 1:1 about the middle of the view; a second call goes back to fit.
    Q_INVOKABLE void actualSize();
    /// Pan from a held centre by a travel in points, clamped.
    Q_INVOKABLE void panView(QPointF from, double dx, double dy);
    /// The lit area's half size in millimetres (the pad's own numbers).
    Q_INVOKABLE double halfWidthMM() const { return board_.half().width(); }
    Q_INVOKABLE double halfHeightMM() const { return board_.half().height(); }
    /// The pad on screen: the lit area, and the aluminium frame round it
    /// (the rim's millimetres scaled). The page's revision-bound bindings
    /// re-read these, so a zoom or a pan re-lays the pad at once.
    Q_INVOKABLE QRectF litScreenRect() const;
    Q_INVOKABLE QRectF frameScreenRect() const;
    /// The table point in the middle of the view right now (a pan's held centre).
    Q_INVOKABLE QPointF viewCentre() const { return QPointF(viewport_.cx, viewport_.cy); }
    /// While a drag holds a film the list model never resets (a reset would
    /// destroy the dragging delegate): the moves land through dataChanged, so
    /// the roles follow the holds live; endGesture's last boardMutated resets
    /// the model for the pile order. Nesting counts, like the BusyGuard's.
    Q_INVOKABLE void beginGesture();
    Q_INVOKABLE void endGesture();

    /// Degrees as the UI reads them (a settled angle's label).
    Q_INVOKABLE QString degreesLabel(double radians) const { return degreesText(radians); }
    /// Millimetres as the UI writes a size.
    Q_INVOKABLE QString mmLabel(double v) const { return mmText(v); }

signals:
    void revisionChanged();
    void boardChanged();
    void viewChanged();
    void screenPropertyChanged();
    void storeSaved(const QString& path);
    void storeAside(const QString& aside);

private:
    /// Every board mutation lands here: model reset, boardChanged, the
    /// debounced write armed.
    void boardMutated();
    void viewMutated();
    void bumpRevision();
    /// The 300 ms debounced write (the website's own delay), cancelled by a
    /// newer change and by saveNow.
    void scheduleSave();
    void writeStore();
    /// Copies an unreadable file beside itself as
    /// `<stem>.unreadable-<seconds>.<ext>` (once per content), never over it.
    void setAside(const QString& path, const QByteArray& bytes);

    Board board_;
    Viewport viewport_;
    /// The frame's picture, by uuid — the library's cached decodes, carried in
    /// with the frames (ImageRole). The Mac's GPU texture ladder is Mac-only
    /// scope; a film without a picture yet shows its true-mm silhouette.
    QHash<QUuid, QImage> images_;
    QString storePath_;
    QTimer saveTimer_;
    QString selected_;
    /// The drag's nesting depth (see beginGesture/endGesture).
    int gestureDepth_ = 0;
    std::optional<double> pointsPerMM_;
    bool actualSizeActive_ = false;
    int lastCarried_ = 0;
    int revision_ = 0;
};

} // namespace spk::desktop::lighttable
