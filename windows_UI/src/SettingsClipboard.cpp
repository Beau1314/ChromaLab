#include "SettingsClipboard.hpp"

#include <algorithm>

namespace spk::desktop::clipboard {

namespace {

constexpr Group kOffered[] = {
    Group::FilmAndPaper, Group::FilmExposure, Group::Format, Group::Decode,
    Group::Enlarger, Group::Light, Group::Curve, Group::Colour};

struct GroupName {
    Group group;
    const char* id;
};

constexpr GroupName kNames[] = {
    {Group::FilmAndPaper, "filmAndPaper"}, {Group::FilmExposure, "filmExposure"},
    {Group::Format, "format"},             {Group::Decode, "decode"},
    {Group::Enlarger, "enlarger"},         {Group::Couplers, "couplers"},
    {Group::Preflash, "preflash"},         {Group::Light, "light"},
    {Group::Curve, "curve"},               {Group::Colour, "colour"}};

} // namespace

std::vector<Group> offeredGroups() {
    return std::vector<Group>(std::begin(kOffered), std::end(kOffered));
}

QString groupId(Group group) {
    for (const auto& named : kNames)
        if (named.group == group) return QLatin1String(named.id);
    return {};
}

std::optional<Group> groupFromId(const QString& id) {
    for (const auto& named : kNames)
        if (id == QLatin1String(named.id)) return named.group;
    return std::nullopt;
}

QStringList groupIds(const GroupSet& groups) {
    QStringList out;
    for (const Group group : offeredGroups())
        if (groups.contains(int(group))) out << groupId(group);
    return out;
}

GroupSet groupSetFromIds(const QStringList& ids) {
    GroupSet out;
    for (const QString& id : ids) {
        const auto group = groupFromId(id);
        if (group.has_value()) out.insert(int(*group));
    }
    return out;
}

GroupSet defaultSelection() {
    GroupSet out;
    for (const Group group : offeredGroups()) out.insert(int(group));
    return out;
}

editing::State Clip::appliedTo(const editing::State& target) const {
    editing::State out = target; // every group's complement survives untouched
    const editing::State& s = settings;
    if (hasGroup(Group::FilmAndPaper)) {
        out.settings.film_stock = s.settings.film_stock;
        out.settings.print_stock = s.settings.print_stock;
        // The DI replaces the paper stage, so its two rows ride the group
        // that carries the paper (the Mac's scanFilm + digitalIntermediate).
        out.settings.di_active = s.settings.di_active;
        out.settings.di_blue_compensation = s.settings.di_blue_compensation;
    }
    if (hasGroup(Group::FilmExposure)) {
        // Settings only: the engine re-meters on the target's next render.
        out.settings.auto_exposure = s.settings.auto_exposure;
        out.settings.film_exposure_ev = s.settings.film_exposure_ev;
    }
    if (hasGroup(Group::Format)) {
        out.settings.film_format_mm = s.settings.film_format_mm;
        out.settings.grain_amount = s.settings.grain_amount;
        out.settings.halation_amount = s.settings.halation_amount;
    }
    if (hasGroup(Group::Decode)) {
        // As Shot is the *target's* camera: a clip copied from an As Shot
        // frame carries 0/0 and writes nothing (the Mac's
        // `if s.decode.whiteBalance != .asShot`). A measured balance
        // travels with its numbers.
        if (s.settings.wb_temperature_k != 0.0 || s.settings.wb_tint != 0.0) {
            out.settings.wb_temperature_k = s.settings.wb_temperature_k;
            out.settings.wb_tint = s.settings.wb_tint;
        }
    }
    if (hasGroup(Group::Enlarger)) {
        out.settings.print_exposure = s.settings.print_exposure;
        out.settings.y_filter_shift = s.settings.y_filter_shift;
        out.settings.m_filter_shift = s.settings.m_filter_shift;
    }
    // couplers / preflash: no fields on this host; even a forced tick is
    // inert (see the header note and the tests).
    if (hasGroup(Group::Light)) {
        out.layer2.exposure = s.layer2.exposure;
        out.layer2.contrast = s.layer2.contrast;
        out.layer2.brightness = s.layer2.brightness;
        out.layer2.highlights = s.layer2.highlights;
        out.layer2.shadows = s.layer2.shadows;
        out.layer2.blackPoint = s.layer2.blackPoint;
        out.layer2.whitePoint = s.layer2.whitePoint;
        out.layer2.vignette = s.layer2.vignette; // Adjustments.take's grouping
    }
    if (hasGroup(Group::Curve)) {
        out.layer2.curves = s.layer2.curves;
    }
    if (hasGroup(Group::Colour)) {
        out.layer2.temperature = s.layer2.temperature;
        out.layer2.tint = s.layer2.tint;
        out.layer2.saturation = s.layer2.saturation;
        out.layer2.colorBalance = s.layer2.colorBalance;
    }
    return out;
}

} // namespace spk::desktop::clipboard
