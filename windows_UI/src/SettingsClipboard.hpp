#pragma once

#include "EditStore.hpp"

#include <QSet>
#include <QString>
#include <QStringList>
#include <optional>
#include <vector>

// The settings clipboard (task 4.4) — the Windows port of the Mac's
// Model/SettingsClipboard.swift: what Copy Settings takes and what Paste
// writes, as pure state arithmetic (`Clip::appliedTo`).
//
// THE ONE CONTRACT: a paste writes SETTINGS, never solved numbers. The
// target meters its own exposure, keeps its own As Shot white balance,
// re-fits its own placement, and keeps its own crop, decode mode and grade
// master switch. On this host "the solved numbers" live nowhere in the
// stored state — the engine re-meters and re-fits at every render — so the
// settings-only rule reduces to: only the ticked groups' fields travel, and
// the fields outside them (crop, mode, the rest of every row) survive the
// paste byte for byte.
//
// GROUPS. The Mac's sheet vocabulary, cut to the fields this host's
// RenderSettings/layer2 surface actually carries. The Mac filters boxes by
// FeatureFlags ("a box for settings that cannot be seen or changed would be
// a box that lies"); this host's cut is the same rule applied to its own
// surface: `Couplers` and `Preflash` stay in the enum (the shared contract,
// and the tests pin that a forced tick writes nothing) but have no
// Windows fields, so offeredGroups() never shows them. `filmEdgeAndDateBack`
// and `hdr` (the Mac's twelfth and eleventh boxes) have no group here at
// all — their fields (overscan_*/date_imprint_*/scan_hdr_*) are simply
// never touched by a paste.
//
// FIELD MAP (per group, the Mac's `written` on this host's rows):
//   filmAndPaper  film_stock, print_stock, di_active, di_blue_compensation
//                 (the DI replaces the paper stage, as the Mac's scanFilm +
//                 digitalIntermediate ride filmAndPaper)
//   filmExposure  auto_exposure, film_exposure_ev (the target re-meters at
//                 its next render; nothing solved is stored to clear)
//   format        film_format_mm, grain_amount, halation_amount (the Mac's
//                 format group carries the format and the two strengths that
//                 scale with it; the view-shape rows have no Windows field)
//   decode        wb_temperature_k + wb_tint — ONLY when the copied balance
//                 is not As Shot (0/0): an As Shot clip keeps the target's
//                 own pair, because the As Shot is the target's own camera
//                 (the Mac's `if s.decode.whiteBalance != .asShot`)
//   enlarger      print_exposure, y_filter_shift, m_filter_shift
//   couplers      (no Windows fields — inert)
//   preflash      (no Windows fields — inert)
//   light         layer2 exposure/contrast/brightness/highlights/shadows/
//                 blackPoint/whitePoint + the vignette (Adjustments.take's
//                 own grouping: the vignette rides the light tool)
//   curve         layer2 curves (the five rows, whole)
//   colour        layer2 temperature/tint/saturation + colorBalance
// Never travels: geometry (the crop), DecodeMode, layer2.enabled (the
// grade's master switch is the target's own), the contrast mask, the scene
// latitude (the placement re-fits), the film edge and date back, the scan
// HDR, glare, the grain/halation on-off flags.
namespace spk::desktop::clipboard {
}

/// The spelling the model and its callers use (`clip::Clip`), defined once
/// beside the namespace. Re-aliasing the same namespace is legal.
namespace clip = spk::desktop::clipboard;

namespace spk::desktop::clipboard {

enum class Group {
    FilmAndPaper, FilmExposure, Format, Decode, Enlarger, Couplers, Preflash,
    Light, Curve, Colour
};

/// The ticked groups: `int(Group)` in a set — cheap to intersect, and the
/// QML sheet hands the same ints back as ids.
using GroupSet = QSet<int>;

/// The sheet's rows, in the Mac's offered order, cut to this host's fields.
std::vector<Group> offeredGroups();

/// The stable string ids the remembered selection and the QML sheet carry.
QString groupId(Group group);
std::optional<Group> groupFromId(const QString& id);

/// The ids of a set, in the sheet's order (round-trips through
/// groupSetFromIds).
QStringList groupIds(const GroupSet& groups);
GroupSet groupSetFromIds(const QStringList& ids);

/// The sheet's opening state: every offered group ticked.
GroupSet defaultSelection();

struct Clip {
    /// The ticked groups. An empty set writes nothing.
    GroupSet groups;
    /// The source's state when it was copied.
    editing::State settings;
    /// The source's file name, for the "what is held" line. Display only.
    QString sourceName;

    bool hasGroup(Group group) const { return groups.contains(int(group)); }

    /// `target` with this clip's groups written into it, and nothing else.
    /// Pure: the whole meaning of a paste.
    editing::State appliedTo(const editing::State& target) const;
};

} // namespace spk::desktop::clipboard
