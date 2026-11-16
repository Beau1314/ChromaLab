// The settings clipboard's group semantics (task 4.4) — the pure tier of the
// Mac's Model/SettingsClipboard.swift, testable without a helper process, an
// engine or a page. The whole meaning of a paste is `Clip::appliedTo`: a
// paste writes SETTINGS, never solved numbers — the target meters its own
// exposure, keeps its own As Shot white balance, re-fits its own placement
// and keeps its own crop; only the ticked groups' fields travel.
#include "SettingsClipboard.hpp"

#include <QPointF>
#include <cstdio>
#include <stdexcept>

namespace clip = spk::desktop::clipboard;
namespace editing = spk::desktop::editing;

namespace {

int checks = 0;
void check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}

editing::State stateAt(double filmEv, double printExposure, double wbK, double wbTint,
                       double grain, double formatMm, const char* film, const char* paper,
                       double layer2Exposure, double layer2Temperature) {
    editing::State state;
    state.settings.film_exposure_ev = filmEv;
    state.settings.print_exposure = printExposure;
    state.settings.wb_temperature_k = wbK;
    state.settings.wb_tint = wbTint;
    state.settings.grain_amount = grain;
    state.settings.film_format_mm = formatMm;
    state.settings.film_stock = film;
    state.settings.print_stock = paper;
    state.settings.auto_exposure = filmEv != 0.0;
    state.settings.di_active = filmEv > 0.0;
    state.settings.di_blue_compensation = filmEv > 0.0;
    state.settings.y_filter_shift = filmEv / 10.0;
    state.settings.m_filter_shift = filmEv / 20.0;
    state.settings.glare_amount = 12.0 + filmEv;
    state.settings.grain_active = filmEv > 0.0;
    state.settings.halation_amount = 2.0 + filmEv;
    state.settings.contrast_mask_active = filmEv > 0.0;
    state.settings.overscan_active = filmEv > 0.0;
    state.settings.date_imprint_active = filmEv > 0.0;
    state.layer2.exposure = layer2Exposure;
    state.layer2.temperature = layer2Temperature;
    state.layer2.tint = layer2Temperature / 2.0;
    state.layer2.saturation = 10.0 + layer2Temperature;
    state.layer2.brightness = 3.0 + layer2Exposure;
    state.layer2.contrast = -2.0 + layer2Exposure;
    state.layer2.highlights = 4.0 + layer2Exposure;
    state.layer2.shadows = -4.0 + layer2Exposure;
    state.layer2.blackPoint = 1.0 + layer2Exposure;
    state.layer2.whitePoint = 2.0 + layer2Exposure;
    state.layer2.vignette.amount = 5.0 + layer2Exposure;
    state.layer2.vignette.midpoint = 40.0 + layer2Exposure;
    state.layer2.colorBalance.master.hue = 1.0 + layer2Temperature;
    state.layer2.colorBalance.shadows.saturation = 2.0 + layer2Temperature;
    state.layer2.curves.rgb.points = {QPointF(0, 0), QPointF(0.5, 0.6), QPointF(1, 1)};
    state.layer2.curves.red.points = {QPointF(0, 0), QPointF(0.4, 0.45), QPointF(1, 1)};
    state.layer2.enabled = layer2Exposure >= 0.0;
    state.geometry.crop = QRectF(0.1, 0.2, 0.5, 0.6);
    state.geometry.quarterTurns = 1;
    state.geometry.straightenDegrees = 3.5;
    state.mode = spk::desktop::DecodeMode::headroom;
    return state;
}

// The copy's source and the paste's target: two states that differ in every
// field a group could carry, so "unchanged" is never a coincidence.
const editing::State& source() {
    static const editing::State state = stateAt(0.3, 1.25, 6500, 10, 1.4, 56.0, "fujifilm_velvia_100",
                                                "kodak_ektar_100", 0.5, 6.0);
    return state;
}
const editing::State& target() {
    static const editing::State state = stateAt(-0.7, 0.9, 4800, -7, 0.6, 24.0, "kodak_portra_400",
                                                "kodak_portra_endura", -0.4, -3.0);
    return state;
}

clip::Clip clipWith(std::initializer_list<clip::Group> groups) {
    clip::Clip clip;
    for (clip::Group group : groups) clip.groups.insert(int(group));
    clip.settings = source();
    clip.sourceName = QStringLiteral("source.png");
    return clip;
}

bool hasGroup(const clip::Clip& clip, clip::Group group) {
    return clip.groups.contains(int(group));
}

} // namespace

void testGroupVocabulary() {
    // The sheet's order is the Mac's offered list, cut to the fields the
    // Windows host surface actually carries: couplers and preflash have no
    // RenderSettings fields on this host, so offering their boxes would be
    // boxes that lie (the Mac's own FeatureFlags rule).
    const auto offered = clip::offeredGroups();
    check(offered.size() == 8, "the Windows sheet offers the eight groups with host fields");
    check(offered.front() == clip::Group::FilmAndPaper && offered.back() == clip::Group::Colour,
          "the sheet runs filmAndPaper .. colour");
    bool couplers = false, preflash = false;
    for (clip::Group group : offered) {
        couplers = couplers || group == clip::Group::Couplers;
        preflash = preflash || group == clip::Group::Preflash;
    }
    check(!couplers && !preflash, "couplers and preflash stay off the Windows sheet");
    // The ids survive a round trip (the remembered selection is stored as ids).
    for (clip::Group group : offered) {
        const auto parsed = clip::groupFromId(clip::groupId(group));
        check(parsed.has_value() && *parsed == group, "group ids parse back");
    }
    check(!clip::groupFromId(QStringLiteral("notAGroup")).has_value(),
          "an unknown id parses to nothing");
    check(clip::groupIds(clip::defaultSelection()).size() == 8,
          "the default selection ticks every offered group");
}

void testEmptyClipWritesNothing() {
    clip::Clip empty;
    empty.settings = source();
    const editing::State result = empty.appliedTo(target());
    check(result == target(), "a clip with no ticked groups writes nothing");
}

void testFilmAndPaper() {
    const auto result = clipWith({clip::Group::FilmAndPaper}).appliedTo(target());
    check(result.settings.film_stock == source().settings.film_stock &&
              result.settings.print_stock == source().settings.print_stock,
          "film and paper travel");
    check(result.settings.di_active == source().settings.di_active &&
              result.settings.di_blue_compensation == source().settings.di_blue_compensation,
          "the digital intermediate rides film and paper (it replaces the paper stage)");
    check(result.settings.film_exposure_ev == target().settings.film_exposure_ev &&
              result.settings.print_exposure == target().settings.print_exposure &&
              result.settings.wb_temperature_k == target().settings.wb_temperature_k &&
              result.settings.film_format_mm == target().settings.film_format_mm,
          "nothing outside film and paper moves");
    check(result.geometry == target().geometry && result.mode == target().mode &&
              result.layer2 == target().layer2,
          "crop, decode mode and the grade are the target's own");
}

void testFilmExposureReMeter() {
    const auto result = clipWith({clip::Group::FilmExposure}).appliedTo(target());
    check(result.settings.auto_exposure == source().settings.auto_exposure &&
              result.settings.film_exposure_ev == source().settings.film_exposure_ev,
          "the meter switch and the film placement travel as settings");
    check(result.settings.print_exposure == target().settings.print_exposure,
          "the enlarger brightness is not a film-exposure field");
    check(result.settings.scene_latitude_active == target().settings.scene_latitude_active,
          "the placement stays the target's own (it re-fits)");
}

void testFormat() {
    const auto result = clipWith({clip::Group::Format}).appliedTo(target());
    check(result.settings.film_format_mm == source().settings.film_format_mm,
          "the format's millimetres travel");
    check(result.settings.grain_amount == source().settings.grain_amount &&
              result.settings.halation_amount == source().settings.halation_amount,
          "the grain and halation strengths scale with the format and travel");
    check(result.settings.grain_active == target().settings.grain_active &&
              result.settings.glare_amount == target().settings.glare_amount,
          "the on/off flags and glare are not format fields");
}

void testDecodeKeepsOwnAsShot() {
    // A non-As Shot decode travels with its numbers.
    const auto warm = clipWith({clip::Group::Decode}).appliedTo(target());
    check(warm.settings.wb_temperature_k == source().settings.wb_temperature_k &&
              warm.settings.wb_tint == source().settings.wb_tint,
          "a measured white balance travels with its numbers");
    // THE rule: a clip whose white balance is As Shot never overwrites the
    // target's — the As Shot is the target's own camera. On this host the
    // copied state simply carries 0/0, and the paste leaves the pair alone.
    clip::Clip asShot = clipWith({clip::Group::Decode});
    editing::State shot = source();
    shot.settings.wb_temperature_k = 0.0;
    shot.settings.wb_tint = 0.0;
    asShot.settings = shot;
    const auto result = asShot.appliedTo(target());
    check(result.settings.wb_temperature_k == target().settings.wb_temperature_k &&
              result.settings.wb_tint == target().settings.wb_tint,
          "an As Shot clip keeps the target's own white balance");
}

void testEnlarger() {
    const auto result = clipWith({clip::Group::Enlarger}).appliedTo(target());
    check(result.settings.print_exposure == source().settings.print_exposure &&
              result.settings.y_filter_shift == source().settings.y_filter_shift &&
              result.settings.m_filter_shift == source().settings.m_filter_shift,
          "the enlarger's three rows travel");
    check(result.settings.contrast_mask_active == target().settings.contrast_mask_active,
          "the print-side mask is not an enlarger row");
    check(result.settings.film_exposure_ev == target().settings.film_exposure_ev,
          "the film placement is not an enlarger row");
}

void testInertGroupsWriteNothing() {
    // Kept in the vocabulary for the shared Mac contract, but this host
    // carries no couplers or preflash fields: even a forced tick writes
    // nothing rather than a lie.
    const auto result = clipWith({clip::Group::Couplers, clip::Group::Preflash}).appliedTo(target());
    check(result == target(), "couplers and preflash have no Windows fields and write none");
}

void testLight() {
    const auto result = clipWith({clip::Group::Light}).appliedTo(target());
    check(result.layer2.exposure == source().layer2.exposure &&
              result.layer2.contrast == source().layer2.contrast &&
              result.layer2.brightness == source().layer2.brightness &&
              result.layer2.highlights == source().layer2.highlights &&
              result.layer2.shadows == source().layer2.shadows &&
              result.layer2.blackPoint == source().layer2.blackPoint &&
              result.layer2.whitePoint == source().layer2.whitePoint,
          "the light tool's seven scalars travel");
    check(result.layer2.vignette == source().layer2.vignette,
          "the vignette rides the light tool (Adjustments.take's own grouping)");
    check(result.layer2.temperature == target().layer2.temperature &&
              result.layer2.saturation == target().layer2.saturation &&
              result.layer2.colorBalance == target().layer2.colorBalance &&
              result.layer2.curves == target().layer2.curves,
          "colour and curve fields stay the target's own");
    check(result.layer2.enabled == target().layer2.enabled,
          "the grade's master switch is the target's own");
}

void testCurve() {
    const auto result = clipWith({clip::Group::Curve}).appliedTo(target());
    check(result.layer2.curves == source().layer2.curves, "the curve set travels whole");
    check(result.layer2.exposure == target().layer2.exposure &&
              result.layer2.temperature == target().layer2.temperature,
          "no other grade field rides the curve");
}

void testColour() {
    const auto result = clipWith({clip::Group::Colour}).appliedTo(target());
    check(result.layer2.temperature == source().layer2.temperature &&
              result.layer2.tint == source().layer2.tint &&
              result.layer2.saturation == source().layer2.saturation &&
              result.layer2.colorBalance == source().layer2.colorBalance,
          "the colour tool's scalars and balance travel");
    check(result.layer2.exposure == target().layer2.exposure &&
              result.layer2.curves == target().layer2.curves &&
              result.layer2.vignette == target().layer2.vignette,
          "light and curve fields stay the target's own");
}

void testCropAndModeNeverTravel() {
    const auto everything = clipWith({clip::Group::FilmAndPaper, clip::Group::FilmExposure,
                                      clip::Group::Format, clip::Group::Decode,
                                      clip::Group::Enlarger, clip::Group::Light,
                                      clip::Group::Curve, clip::Group::Colour});
    const auto result = everything.appliedTo(target());
    check(result.geometry == target().geometry, "the crop never travels");
    check(result.mode == target().mode, "the decode mode is the target's own");
    check(result.settings.overscan_active == target().settings.overscan_active &&
              result.settings.date_imprint_active == target().settings.date_imprint_active,
          "film edge and date back have no group on this host and stay the target's own");
}

int main() {
    try {
        testGroupVocabulary();
        testEmptyClipWritesNothing();
        testFilmAndPaper();
        testFilmExposureReMeter();
        testFormat();
        testDecodeKeepsOwnAsShot();
        testEnlarger();
        testInertGroupsWriteNothing();
        testLight();
        testCurve();
        testColour();
        testCropAndModeNeverTravel();
        std::printf("settings_clip: %d checks passed\n", checks);
        return 0;
    } catch (const std::exception& e) {
        std::printf("settings_clip: FAIL after %d checks: %s\n", checks, e.what());
        return 1;
    }
}
