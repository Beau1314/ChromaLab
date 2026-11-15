#pragma once

#include "FrameOutput.hpp"
#include <QHash>
#include <QString>
#include <QUrl>
#include <optional>

// Per-photo edits belong to the application's own directory, never beside a
// RAW. The directory is mandatory so test callers cannot reach the real store.
namespace spk::desktop::editing {

struct State {
    RenderSettings settings;
    DecodeMode mode = DecodeMode::compatible16;
    output::Geometry geometry;
    // The scan-side grade above the engine's print (Layer 2). Persisted under
    // the wire shape of layer2::encode; a record without the key restores the
    // neutral grade so older histories keep loading.
    layer2::Adjustments layer2;
    bool operator==(const State&) const = default;
};

struct LoadResult {
    std::optional<State> state;
    QString warning;
    // A record exists but cannot safely be replaced (or its source cannot be
    // checked). The editor can still work in memory and should show a warning.
    bool blocked = false;
};

// --- The half-frame pair's record (PAIR-WIRING §5; v2 .spektrapair
// semantics: version/sources/hole states/look). A pair keeps its own
// independent record file keyed by the union hash of BOTH source URLs, with
// the same atomicity and conflict discipline as a photo record. A missing
// record is simply a new pair.

// One hole's placement: the fit rectangle of the as-seen picture (largest
// hole-aspect rect at scale 1) zoomed about its centre and translated in
// fractions of the source. The placement is compose input only -- the meter
// key deliberately excludes it (PAIR-WIRING §4.2).
struct PairPlacement {
    double scale = 1.0;   // zoom about the fit's centre; 1 = fit, 8 = closest
    double x = 0.0;       // translation, fractions of the source's width
    double y = 0.0;
    int quarterTurns = 0; // clockwise as seen, 0 through 3
    bool operator==(const PairPlacement&) const = default;
};

// One hole's own print (PAIR-WIRING §12): an offset of the four enlarger
// rows for the frame's own print. The Windows host sends no preflash row
// yet, so the fourth row is stored and persisted, awaiting the layered
// print port.
struct PairTrim {
    double print_exposure = 0.0;    // stops offset
    double y_filter_shift = 0.0;
    double m_filter_shift = 0.0;
    double preflash_exposure = 0.0; // stored; not wired on Windows yet
    bool operator==(const PairTrim&) const = default;
};

struct PairHoleState {
    PairPlacement placement;
    double exposure_ev = 0.0; // the hole's own exposure in stops (§4.3)
    bool dated = false;
    QString date_text;        // already formatted text, e.g. "'26 11 15"
    PairTrim trim;
    bool operator==(const PairHoleState&) const = default;
};

struct PairState {
    double spacing_mm = 1.0;  // the strip's 1 mm; a wider gap is a bare pair
    bool turned = false;      // the film runs down: the holes stack vertically
    PairHoleState holes[2];   // [0] = the first (left/upper) hole
    State look;               // the pair item's own look -- the wire of §3
    bool operator==(const PairState& other) const {
        return spacing_mm == other.spacing_mm && turned == other.turned &&
               holes[0] == other.holes[0] && holes[1] == other.holes[1] &&
               look == other.look;
    }
};

struct PairLoadResult {
    std::optional<PairState> state;
    QString warning;
    bool blocked = false;
};

class Store final {
public:
    explicit Store(QString directory);
    LoadResult load(const QUrl& source, const Catalog& catalog) const;
    // Atomic replacement after validating the existing record and source.
    // Throws std::runtime_error; a rejected save leaves old edits untouched.
    void save(const QUrl& source, const State& state) const;
    QString filePath(const QUrl& source) const;
    // The pair record: file keyed by the ORDERED union hash of the two
    // source identities (the first hole is positional, so the swapped order
    // is a different pair and never shares a file).
    QString pairFilePath(const QUrl& left, const QUrl& right) const;
    PairLoadResult loadPair(const QUrl& left, const QUrl& right,
                            const Catalog& catalog) const;
    void savePair(const QUrl& left, const QUrl& right, const PairState& state) const;

private:
    struct Observation {
        QByteArray source, record;
        QString warning;
        std::optional<Catalog> catalog;
    };
    QString directory_;
    // All calls belong to the controller thread. Remember what it loaded so
    // a changed source or another app instance cannot silently lose edits.
    mutable QHash<QString, Observation> observations_;
};

} // namespace spk::desktop::editing
