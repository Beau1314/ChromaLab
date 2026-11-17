#include "preview_host.hpp"
#include "io/image_writer.hpp"
#include "json.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace spk::desktop {
namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using EnginePtr = std::unique_ptr<spk_engine, decltype(&spk_engine_destroy)>;
using SessionPtr = std::unique_ptr<spk_session, decltype(&spk_session_release)>;

double elapsed(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string utf8(const fs::path& path) {
    const auto s = path.u8string();
    return {reinterpret_cast<const char*>(s.data()), s.size()};
}

Json read_json(const fs::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    require(bool(file), "Cannot read bundled stock profile: " + utf8(path));
    const auto length = file.tellg();
    require(length > 0 && length <= 16 * 1024 * 1024, "Invalid bundled stock profile size");
    std::string text(std::size_t(length), '\0');
    file.seekg(0);
    require(bool(file.read(text.data(), std::streamsize(length))), "Cannot read complete stock profile");
    Json value;
    std::string error;
    const bool ok = Json::parse(text, value, error);
    require(ok, "Bundled stock profile: " + error);
    return value;
}

Catalog read_catalog(const fs::path& resources) {
    Catalog catalog;
    std::set<std::string> ids;
    for (const auto& entry : fs::directory_iterator(resources / "profiles")) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
        const Json profile = read_json(entry.path());
        const auto& info = profile.at("info");
        Stock stock{info.at("stock").as_string(), info.at("name").as_string()};
        const auto& stage = info.at("stage").as_string();
        const auto& type = info.at("type").as_string();
        require(!stock.id.empty() && stock.id == utf8(entry.path().stem()) &&
                    std::all_of(stock.id.begin(), stock.id.end(), [](unsigned char c) {
                        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                               (c >= '0' && c <= '9') || c == '_';
                    }), "Invalid stock identifier in bundled profile");
        require(!stock.label.empty() && ids.insert(stock.id).second,
                "Missing or duplicate bundled stock metadata");
        require(type == "negative" || type == "positive", "Unknown bundled stock type: " + type);
        stock.is_positive = type == "positive";
        if (stage == "filming") catalog.films.push_back(std::move(stock));
        else if (stage == "printing") catalog.papers.push_back(std::move(stock));
        else throw std::runtime_error("Unknown bundled stock stage: " + stage);
    }
    const auto by_label = [](const Stock& a, const Stock& b) { return a.label < b.label; };
    std::sort(catalog.films.begin(), catalog.films.end(), by_label);
    std::sort(catalog.papers.begin(), catalog.papers.end(), by_label);
    require(!catalog.films.empty() && !catalog.papers.empty(), "Bundled film/paper catalog is empty");
    return catalog;
}

bool validate_settings(const RenderSettings& settings, const Catalog& catalog) {
    const auto contains = [](const std::vector<Stock>& stocks, const std::string& id) {
        return std::any_of(stocks.begin(), stocks.end(), [&](const Stock& s) { return s.id == id; });
    };
    const auto film = std::find_if(catalog.films.begin(), catalog.films.end(),
        [&](const Stock& stock) { return stock.id == settings.film_stock; });
    require(film != catalog.films.end(), "Unknown film stock: " + settings.film_stock);
    require(contains(catalog.papers, settings.print_stock), "Unknown paper stock: " + settings.print_stock);
    require(std::isfinite(settings.print_exposure) && settings.print_exposure >= 0.05 &&
                settings.print_exposure <= 20.0, "Print exposure must be between 0.05 and 20");
    const auto range = [](double value, double low, double high, const char* label) {
        require(std::isfinite(value) && value >= low && value <= high,
                std::string(label) + " must be between " + std::to_string(low) + " and " + std::to_string(high));
    };
    // Reject before touching the live session, including NaN/Inf values
    // that cannot be represented faithfully in the JSON parameter delta.
    range(settings.film_exposure_ev, -8.0, 8.0, "Film exposure (EV)");
    range(settings.film_format_mm, 4.0, 200.0, "Film format (mm)");
    range(settings.grain_amount, 0.0, 2.0, "Grain amount");
    range(settings.halation_amount, 0.0, 4.0, "Halation amount");
    range(settings.glare_amount, 0.0, 30.0, "Glare amount");
    range(settings.y_filter_shift, -1.0, 1.0, "Yellow filter shift");
    range(settings.m_filter_shift, -1.0, 1.0, "Magenta filter shift");
    // The mask's amounts are stops and the core is a half-width in the same
    // units; the scale is the base extractor's sigma as a long-edge fraction.
    // The ranges are the engine schema's own rows (params.cpp).
    range(settings.contrast_mask_highlights, 0.0, 3.0, "Contrast mask highlights");
    range(settings.contrast_mask_shadows, 0.0, 3.0, "Contrast mask shadows");
    range(settings.contrast_mask_core, 0.0, 3.0, "Contrast mask core");
    range(settings.contrast_mask_scale, 0.002, 0.12, "Contrast mask scale");
    // The HDR scan's headroom is the clear base's gain and the output scale
    // is the rendition's post-gamut gain; the ranges are the engine schema's
    // own rows (params.cpp). The stretch flag has no range of its own.
    range(settings.scan_hdr_headroom, 0.0, 16.0, "Scan HDR headroom");
    range(settings.scan_hdr_output_scale, 0.0625, 1.0, "Scan HDR output scale");
    // The half-frame pair's second frame (params.cpp's split/b_* rows): the
    // split is a place along the piece's long edge and the b_* knees/rooms
    // have the ranges the first frame's curve has. b_active is a switch.
    range(settings.scene_latitude_split, 0.0, 1.0, "Scene latitude split");
    range(settings.scene_latitude_b_highlight_knee, -24.0, 24.0, "Scene latitude B highlight knee");
    range(settings.scene_latitude_b_highlight_room, 0.0, 24.0, "Scene latitude B highlight room");
    range(settings.scene_latitude_b_shadow_knee, -24.0, 24.0, "Scene latitude B shadow knee");
    range(settings.scene_latitude_b_shadow_room, 0.0, 24.0, "Scene latitude B shadow room");
    // The film edge's numbers, ranges and choices are the engine schema's own
    // rows (params.cpp): the seeds are the RFC-032 §26.1 draw, fog/leaks are
    // EV-ish gains in [0, 4], f_number is EXIF's [0, 64] with 0 = unknown,
    // frame_number is the edge print's [0, 99] with 0 = the seed's draw, and
    // the date back's exposure is [-2, 8] EV with its size in [0.4, 3] and
    // insets in [0, 30] mm. The enumerations (mode/format/gate/holes/carrier,
    // placement/style/corner) the engine validates at the layout and the date
    // back's own validation, and the schema refuses anything else first.
    range(settings.overscan_fog, 0.0, 4.0, "Overscan fog");
    range(settings.overscan_leaks, 0.0, 4.0, "Overscan leaks");
    range(settings.overscan_f_number, 0.0, 64.0, "Overscan f-number");
    range(settings.date_imprint_exposure_ev, -2.0, 8.0, "Date imprint exposure");
    range(settings.date_imprint_size, 0.4, 3.0, "Date imprint size");
    range(settings.date_imprint_inset_x, 0.0, 30.0, "Date imprint inset X");
    range(settings.date_imprint_inset_y, 0.0, 30.0, "Date imprint inset Y");
    require(settings.overscan_frame_seed >= 0 && settings.overscan_camera_seed >= 0,
            "Overscan seeds must be non-negative");
    require(settings.overscan_frame_number >= 0 && settings.overscan_frame_number <= 99,
            "Overscan frame number must be between 0 and 99");
    static const char* kModes[] = {"strip", "filed"};
    static const char* kFormats[] = {"135", "135_half", "120_645", "120_6x6", "120_6x7",
                                     "120_6x8", "120_6x9", "135_xpan", "120_6x12", "120_6x17"};
    static const char* kGates[] = {"auto", "square", "rounded", "eared", "shouldered", "kicked"};
    static const char* kHoles[] = {"white", "black"};
    static const char* kCarriers[] = {"none", "black", "open"};
    static const char* kPlacements[] = {"frame", "rebate"};
    static const char* kStyles[] = {"lcd", "dots", "data"};
    static const char* kCorners[] = {"br", "bl", "tr", "tl"};
    const auto one_of = [](const std::string& value, const char* const* choices, size_t n,
                           const char* label) {
        require(std::any_of(choices, choices + n,
                            [&](const char* c) { return value == c; }),
                std::string(label) + " is not one of the engine's choices: " + value);
    };
    one_of(settings.overscan_mode, kModes, 2, "Overscan mode");
    one_of(settings.overscan_format, kFormats, 10, "Overscan format");
    one_of(settings.overscan_gate, kGates, 6, "Overscan gate");
    one_of(settings.overscan_holes, kHoles, 2, "Overscan holes");
    one_of(settings.overscan_carrier, kCarriers, 3, "Overscan carrier");
    one_of(settings.date_imprint_placement, kPlacements, 2, "Date imprint placement");
    one_of(settings.date_imprint_style, kStyles, 3, "Date imprint style");
    one_of(settings.date_imprint_corner, kCorners, 4, "Date imprint corner");
    // The WB override is the decode's own protocol range (R2, mirrored by
    // the engine host): 0 = As Shot, otherwise an illuminant in Kelvin the
    // decoder must be able to neutralise. The Robertson mapping clamps to
    // 1700..50000 K internally; outside the protocol range is a caller bug,
    // not a clamp case.
    if (settings.wb_temperature_k != 0.0) {
        require(std::isfinite(settings.wb_temperature_k) &&
                    settings.wb_temperature_k >= 2000.0 && settings.wb_temperature_k <= 50000.0,
                "White balance temperature must be 2000..50000 K, or 0 for As Shot");
    }
    require(std::isfinite(settings.wb_tint) &&
                settings.wb_tint >= -150.0 && settings.wb_tint <= 150.0,
            "White balance tint must be between -150 and 150");
    return film->is_positive;
}

Json opening_params(const RenderSettings& settings, bool scan_film) {
    // Unexposed effects keep their engine defaults. The exposed settings
    // start at those same defaults, so opening an unedited image retains
    // the existing look, including the physical 35 mm grain/halation scale.
    Json params = Json::object();
    params.set("input_color_space", Json(std::string("ProPhoto RGB")));
    params.set("input_cctf_decoding", Json(false));
    params.set("output_color_space", Json(std::string("sRGB")));
    params.set("output_cctf_encoding", Json(true));
    params.set("extended_dynamic_range", Json(false));
    params.set("film_stock", Json(settings.film_stock));
    // Desktop policy matches the macOS session: a slide is already a
    // positive and must bypass negative-to-paper printing. The generic
    // engine API still permits deliberate positive-to-paper experiments.
    params.set("scan_film", Json(scan_film));
    params.set("print_stock", Json(settings.print_stock));
    params.set("print_exposure", Json(settings.print_exposure));
    params.set("exposure_compensation_ev", Json(settings.film_exposure_ev));
    // The meter switch and its intent travel together (the schema's own
    // neighbouring rows), so a fresh open and a later toggle can never
    // disagree about which metering is in effect.
    params.set("auto_exposure", Json(settings.auto_exposure));
    params.set("auto_exposure_method", Json(std::string("center_weighted")));
    params.set("film_format_mm", Json(settings.film_format_mm));
    params.set("grain_active", Json(settings.grain_active));
    params.set("grain_amount", Json(settings.grain_amount));
    params.set("halation_active", Json(settings.halation_active));
    params.set("halation_amount", Json(settings.halation_amount));
    params.set("glare_active", Json(settings.glare_active));
    params.set("glare_amount", Json(settings.glare_amount));
    params.set("y_filter_shift", Json(settings.y_filter_shift));
    params.set("m_filter_shift", Json(settings.m_filter_shift));
    // The Cineon Digital Intermediate travels with every open too: a saved
    // DI-on session must restore as one when the photograph is reopened.
    params.set("digital_intermediate", Json(settings.di_active));
    params.set("digital_intermediate_blue_compensation", Json(settings.di_blue_compensation));
    // Scene Latitude Mapping's resolved curve as well: the fit is render
    // state, and a committed mapping must survive reopening the photograph.
    params.set("scene_latitude_active", Json(settings.scene_latitude_active));
    params.set("scene_latitude_norm", Json(settings.scene_latitude_norm));
    params.set("scene_latitude_highlight_knee", Json(settings.scene_latitude_highlight_knee));
    params.set("scene_latitude_highlight_room", Json(settings.scene_latitude_highlight_room));
    params.set("scene_latitude_shadow_knee", Json(settings.scene_latitude_shadow_knee));
    params.set("scene_latitude_shadow_room", Json(settings.scene_latitude_shadow_room));
    params.set("scene_latitude_rolloff", Json(settings.scene_latitude_rolloff));
    params.set("scene_latitude_max_lift", Json(settings.scene_latitude_max_lift));
    // The half-frame pair's second frame rides with every open too: a saved
    // split/b_* state must restore as one when the photograph is reopened.
    // The engine applies the b_ rows only while a split is set, so carrying
    // them is always safe.
    params.set("scene_latitude_split", Json(settings.scene_latitude_split));
    params.set("scene_latitude_b_active", Json(settings.scene_latitude_b_active));
    params.set("scene_latitude_b_highlight_knee", Json(settings.scene_latitude_b_highlight_knee));
    params.set("scene_latitude_b_highlight_room", Json(settings.scene_latitude_b_highlight_room));
    params.set("scene_latitude_b_shadow_knee", Json(settings.scene_latitude_b_shadow_knee));
    params.set("scene_latitude_b_shadow_room", Json(settings.scene_latitude_b_shadow_room));
    // The contrast mask's curve travels with every open too: a saved mask-on
    // session must restore as one when the photograph is reopened.
    params.set("contrast_mask_active", Json(settings.contrast_mask_active));
    params.set("contrast_mask_highlights", Json(settings.contrast_mask_highlights));
    params.set("contrast_mask_shadows", Json(settings.contrast_mask_shadows));
    params.set("contrast_mask_core", Json(settings.contrast_mask_core));
    params.set("contrast_mask_scale", Json(settings.contrast_mask_scale));
    // The slide's HDR scan travels with every open too: a saved HDR-on session
    // must restore as one when the photograph is reopened (a negative film
    // ignores the fields, so carrying them is always safe).
    params.set("scan_hdr_headroom", Json(settings.scan_hdr_headroom));
    params.set("scan_hdr_stretch", Json(settings.scan_hdr_stretch));
    params.set("scan_hdr_output_scale", Json(settings.scan_hdr_output_scale));
    // The film edge and the date back travel with every open too: a saved
    // overscan-on session must restore as one when the photograph is
    // reopened. The pair and its second frame's date ride along (the reader
    // gap the unlock closed).
    params.set("overscan_active", Json(settings.overscan_active));
    params.set("overscan_mode", Json(settings.overscan_mode));
    params.set("overscan_format", Json(settings.overscan_format));
    params.set("overscan_gate", Json(settings.overscan_gate));
    params.set("overscan_holes", Json(settings.overscan_holes));
    params.set("overscan_carrier", Json(settings.overscan_carrier));
    params.set("overscan_edge_text", Json(settings.overscan_edge_text));
    params.set("overscan_frame_number", Json(double(settings.overscan_frame_number)));
    params.set("overscan_frame_seed", Json(double(settings.overscan_frame_seed)));
    params.set("overscan_camera_seed", Json(double(settings.overscan_camera_seed)));
    params.set("overscan_f_number", Json(settings.overscan_f_number));
    params.set("overscan_fog", Json(settings.overscan_fog));
    params.set("overscan_leaks", Json(settings.overscan_leaks));
    params.set("overscan_pair", Json(settings.overscan_pair));
    params.set("date_imprint_active", Json(settings.date_imprint_active));
    params.set("date_imprint_text", Json(settings.date_imprint_text));
    params.set("date_imprint_text_b", Json(settings.date_imprint_text_b));
    params.set("date_imprint_data_text", Json(settings.date_imprint_data_text));
    params.set("date_imprint_placement", Json(settings.date_imprint_placement));
    params.set("date_imprint_ev", Json(settings.date_imprint_exposure_ev));
    params.set("date_imprint_style", Json(settings.date_imprint_style));
    params.set("date_imprint_size", Json(settings.date_imprint_size));
    params.set("date_imprint_corner", Json(settings.date_imprint_corner));
    params.set("date_imprint_inset_x", Json(settings.date_imprint_inset_x));
    params.set("date_imprint_inset_y", Json(settings.date_imprint_inset_y));
    return params;
}

Json changed_params(const RenderSettings& before, const RenderSettings& after, bool scan_film) {
    Json delta = Json::object();
    // Sending an unchanged film_stock still invalidates the negative in the
    // C API. A print-only edit must never include it incidentally.
    if (before.film_stock != after.film_stock) {
        delta.set("film_stock", Json(after.film_stock));
        // Send false too when returning to negative film, otherwise the
        // previous slide's direct-scan mode would survive the stock change.
        delta.set("scan_film", Json(scan_film));
    }
    if (before.print_stock != after.print_stock) delta.set("print_stock", Json(after.print_stock));
    if (before.print_exposure != after.print_exposure) delta.set("print_exposure", Json(after.print_exposure));
    // The C API retains carried user controls when loading another stock.
    // Send only changed fields: an unchanged SHOOT field in a paper-only
    // delta would unnecessarily invalidate the cached grain realization.
    if (before.film_exposure_ev != after.film_exposure_ev) delta.set("exposure_compensation_ev", Json(after.film_exposure_ev));
    // The intent only matters while the meter is on, and the toggle already
    // invalidates the SHOOT-stage state it belongs to, so it travels with it.
    if (before.auto_exposure != after.auto_exposure) {
        delta.set("auto_exposure", Json(after.auto_exposure));
        delta.set("auto_exposure_method", Json(std::string("center_weighted")));
    }
    if (before.film_format_mm != after.film_format_mm) delta.set("film_format_mm", Json(after.film_format_mm));
    if (before.grain_active != after.grain_active) delta.set("grain_active", Json(after.grain_active));
    if (before.grain_amount != after.grain_amount) delta.set("grain_amount", Json(after.grain_amount));
    if (before.halation_active != after.halation_active) delta.set("halation_active", Json(after.halation_active));
    if (before.halation_amount != after.halation_amount) delta.set("halation_amount", Json(after.halation_amount));
    if (before.glare_active != after.glare_active) delta.set("glare_active", Json(after.glare_active));
    if (before.glare_amount != after.glare_amount) delta.set("glare_amount", Json(after.glare_amount));
    if (before.y_filter_shift != after.y_filter_shift) delta.set("y_filter_shift", Json(after.y_filter_shift));
    if (before.m_filter_shift != after.m_filter_shift) delta.set("m_filter_shift", Json(after.m_filter_shift));
    if (before.di_active != after.di_active) delta.set("digital_intermediate", Json(after.di_active));
    if (before.di_blue_compensation != after.di_blue_compensation)
        delta.set("digital_intermediate_blue_compensation", Json(after.di_blue_compensation));
    // The mapping's curve: the toggle alone, or the fit commit's full set of
    // solved knees and rooms (the latitude card's 提交拟合 writes exactly the
    // keys the analysis reply's params_delta carries).
    if (before.scene_latitude_active != after.scene_latitude_active)
        delta.set("scene_latitude_active", Json(after.scene_latitude_active));
    if (before.scene_latitude_norm != after.scene_latitude_norm)
        delta.set("scene_latitude_norm", Json(after.scene_latitude_norm));
    if (before.scene_latitude_highlight_knee != after.scene_latitude_highlight_knee)
        delta.set("scene_latitude_highlight_knee", Json(after.scene_latitude_highlight_knee));
    if (before.scene_latitude_highlight_room != after.scene_latitude_highlight_room)
        delta.set("scene_latitude_highlight_room", Json(after.scene_latitude_highlight_room));
    if (before.scene_latitude_shadow_knee != after.scene_latitude_shadow_knee)
        delta.set("scene_latitude_shadow_knee", Json(after.scene_latitude_shadow_knee));
    if (before.scene_latitude_shadow_room != after.scene_latitude_shadow_room)
        delta.set("scene_latitude_shadow_room", Json(after.scene_latitude_shadow_room));
    if (before.scene_latitude_rolloff != after.scene_latitude_rolloff)
        delta.set("scene_latitude_rolloff", Json(after.scene_latitude_rolloff));
    if (before.scene_latitude_max_lift != after.scene_latitude_max_lift)
        delta.set("scene_latitude_max_lift", Json(after.scene_latitude_max_lift));
    // The half-frame pair's second frame: the same per-field diff -- the
    // split's position, the second curve's switch and its solved knees and
    // rooms (the fit commit writes exactly the keys the reply carries).
    if (before.scene_latitude_split != after.scene_latitude_split)
        delta.set("scene_latitude_split", Json(after.scene_latitude_split));
    if (before.scene_latitude_b_active != after.scene_latitude_b_active)
        delta.set("scene_latitude_b_active", Json(after.scene_latitude_b_active));
    if (before.scene_latitude_b_highlight_knee != after.scene_latitude_b_highlight_knee)
        delta.set("scene_latitude_b_highlight_knee", Json(after.scene_latitude_b_highlight_knee));
    if (before.scene_latitude_b_highlight_room != after.scene_latitude_b_highlight_room)
        delta.set("scene_latitude_b_highlight_room", Json(after.scene_latitude_b_highlight_room));
    if (before.scene_latitude_b_shadow_knee != after.scene_latitude_b_shadow_knee)
        delta.set("scene_latitude_b_shadow_knee", Json(after.scene_latitude_b_shadow_knee));
    if (before.scene_latitude_b_shadow_room != after.scene_latitude_b_shadow_room)
        delta.set("scene_latitude_b_shadow_room", Json(after.scene_latitude_b_shadow_room));
    // The mask's curve: the toggle alone, or the amounts with it -- the same
    // engine-parameter channel the DI and the mapping travel by.
    if (before.contrast_mask_active != after.contrast_mask_active)
        delta.set("contrast_mask_active", Json(after.contrast_mask_active));
    if (before.contrast_mask_highlights != after.contrast_mask_highlights)
        delta.set("contrast_mask_highlights", Json(after.contrast_mask_highlights));
    if (before.contrast_mask_shadows != after.contrast_mask_shadows)
        delta.set("contrast_mask_shadows", Json(after.contrast_mask_shadows));
    if (before.contrast_mask_core != after.contrast_mask_core)
        delta.set("contrast_mask_core", Json(after.contrast_mask_core));
    if (before.contrast_mask_scale != after.contrast_mask_scale)
        delta.set("contrast_mask_scale", Json(after.contrast_mask_scale));
    // The slide's HDR scan: the same engine-parameter channel the DI and the
    // mask travel by. A film that is not a positive scanned ignores it, so
    // the diff can carry it without a film gate of its own.
    if (before.scan_hdr_headroom != after.scan_hdr_headroom)
        delta.set("scan_hdr_headroom", Json(after.scan_hdr_headroom));
    if (before.scan_hdr_stretch != after.scan_hdr_stretch)
        delta.set("scan_hdr_stretch", Json(after.scan_hdr_stretch));
    if (before.scan_hdr_output_scale != after.scan_hdr_output_scale)
        delta.set("scan_hdr_output_scale", Json(after.scan_hdr_output_scale));
    // The film edge and the date back: the same engine-parameter channel, the
    // engine's own wire keys (params.cpp -- the date's exposure travels as
    // date_imprint_ev).
    if (before.overscan_active != after.overscan_active) delta.set("overscan_active", Json(after.overscan_active));
    if (before.overscan_mode != after.overscan_mode) delta.set("overscan_mode", Json(after.overscan_mode));
    if (before.overscan_format != after.overscan_format) delta.set("overscan_format", Json(after.overscan_format));
    if (before.overscan_gate != after.overscan_gate) delta.set("overscan_gate", Json(after.overscan_gate));
    if (before.overscan_holes != after.overscan_holes) delta.set("overscan_holes", Json(after.overscan_holes));
    if (before.overscan_carrier != after.overscan_carrier) delta.set("overscan_carrier", Json(after.overscan_carrier));
    if (before.overscan_edge_text != after.overscan_edge_text) delta.set("overscan_edge_text", Json(after.overscan_edge_text));
    if (before.overscan_frame_number != after.overscan_frame_number)
        delta.set("overscan_frame_number", Json(double(after.overscan_frame_number)));
    if (before.overscan_frame_seed != after.overscan_frame_seed)
        delta.set("overscan_frame_seed", Json(double(after.overscan_frame_seed)));
    if (before.overscan_camera_seed != after.overscan_camera_seed)
        delta.set("overscan_camera_seed", Json(double(after.overscan_camera_seed)));
    if (before.overscan_f_number != after.overscan_f_number) delta.set("overscan_f_number", Json(after.overscan_f_number));
    if (before.overscan_fog != after.overscan_fog) delta.set("overscan_fog", Json(after.overscan_fog));
    if (before.overscan_leaks != after.overscan_leaks) delta.set("overscan_leaks", Json(after.overscan_leaks));
    if (before.overscan_pair != after.overscan_pair) delta.set("overscan_pair", Json(after.overscan_pair));
    if (before.date_imprint_active != after.date_imprint_active)
        delta.set("date_imprint_active", Json(after.date_imprint_active));
    if (before.date_imprint_text != after.date_imprint_text) delta.set("date_imprint_text", Json(after.date_imprint_text));
    if (before.date_imprint_text_b != after.date_imprint_text_b)
        delta.set("date_imprint_text_b", Json(after.date_imprint_text_b));
    if (before.date_imprint_data_text != after.date_imprint_data_text)
        delta.set("date_imprint_data_text", Json(after.date_imprint_data_text));
    if (before.date_imprint_placement != after.date_imprint_placement)
        delta.set("date_imprint_placement", Json(after.date_imprint_placement));
    if (before.date_imprint_exposure_ev != after.date_imprint_exposure_ev)
        delta.set("date_imprint_ev", Json(after.date_imprint_exposure_ev));
    if (before.date_imprint_style != after.date_imprint_style) delta.set("date_imprint_style", Json(after.date_imprint_style));
    if (before.date_imprint_size != after.date_imprint_size) delta.set("date_imprint_size", Json(after.date_imprint_size));
    if (before.date_imprint_corner != after.date_imprint_corner)
        delta.set("date_imprint_corner", Json(after.date_imprint_corner));
    if (before.date_imprint_inset_x != after.date_imprint_inset_x)
        delta.set("date_imprint_inset_x", Json(after.date_imprint_inset_x));
    if (before.date_imprint_inset_y != after.date_imprint_inset_y)
        delta.set("date_imprint_inset_y", Json(after.date_imprint_inset_y));
    return delta;
}

// The session's resolved working space (CONTRACT §1.2): the open reply's
// params is the authority on what the frame's RGBA16 is encoded in -- a
// caller that overrides `output_color_space` owns the consequences, and the
// app's transform reads the *resolved* space out of the reply. The wire's
// params are the schema's flat names, not an io-nested tree. The host has
// no JSON lifetime to share, so the two fields are copied out and the reply
// freed before anything can throw.
void resolved_working_space(char* reply_json,
                            std::string& output_color_space, bool& output_cctf_encoding) {
    require(reply_json != nullptr, "Open reply carries no JSON");
    const std::string reply(reply_json);
    spk_string_free(reply_json);
    Json parsed;
    std::string error;
    require(Json::parse(reply, parsed, error), "Open reply: " + error);
    const Json& params = parsed.at("params");
    const std::string space = params.at("output_color_space").as_string();
    require(!space.empty(), "Open reply carries no resolved output colour space");
    output_color_space = space;
    output_cctf_encoding = params.at("output_cctf_encoding").as_bool();
}

SessionPtr open_session(spk_engine* engine, const io::DecodedRaw& decoded,
                        const RenderSettings& settings, bool scan_film,
                        std::string& output_color_space, bool& output_cctf_encoding) {
    const spk_image input{decoded.rgb.data(), decoded.width, decoded.height, 3};
    const auto params = opening_params(settings, scan_film).dump();
    // The reply is read once for the resolved io (above) and freed; nothing
    // else in the host shares JSON with the C API.
    char* reply_json = nullptr;
    SessionPtr session(spk_open(engine, &input, params.c_str(), &reply_json), &spk_session_release);
    require(bool(session), "Open image: " + std::string(spk_last_error(engine)));
    resolved_working_space(reply_json, output_color_space, output_cctf_encoding);
    return session;
}

// The meter's own size cap (PAIR-WIRING §4): a picture above 1,024 px on the
// long edge is box-downscaled before its throwaway session opens. A box keeps
// the pixel distribution the meter reads, so the reading does not move with
// the cap beyond sampling.
std::vector<float> meter_scale(const spk_image& input, int width, int height) {
    std::vector<float> out(std::size_t(width) * height * input.channels);
    const double fx = double(input.width) / width, fy = double(input.height) / height;
    for (int y = 0; y < height; ++y) {
        const int y0 = std::min(int(std::floor(y * fy)), int(input.height));
        const int y1 = std::min(std::max(int(std::floor((y + 1) * fy)), y0 + 1), int(input.height));
        for (int x = 0; x < width; ++x) {
            const int x0 = std::min(int(std::floor(x * fx)), int(input.width));
            const int x1 = std::min(std::max(int(std::floor((x + 1) * fx)), x0 + 1), int(input.width));
            double sum[4] = {0.0, 0.0, 0.0, 0.0};
            for (int sy = y0; sy < y1; ++sy) {
                const float* row = input.data + (std::size_t(sy) * input.width + x0) * input.channels;
                for (int sx = x0; sx < x1; ++sx, row += input.channels)
                    for (std::uint32_t c = 0; c < input.channels; ++c) sum[c] += row[c];
            }
            const double count = double((y1 - y0) * (x1 - x0));
            float* dst = &out[(std::size_t(y) * width + x) * input.channels];
            for (std::uint32_t c = 0; c < input.channels; ++c) dst[c] = float(sum[c] / count);
        }
    }
    return out;
}
}  // namespace

Frame::~Frame() { spk_result_free(&result_); }

std::vector<std::uint8_t> rgba16_to_bgra8(const spk_result& result) {
    require(result.rgba16 && result.width && result.height && result.row_stride_px >= result.width,
            "Invalid RGBA16 display dimensions or row pitch");
    constexpr auto max_size = std::numeric_limits<std::size_t>::max();
    // Both the source address calculation and destination allocation must fit;
    // checking pixels alone would miss overflow of the row padding.
    require(std::size_t(result.height) <= max_size / result.row_stride_px / 4 / sizeof(std::uint16_t) &&
                std::size_t(result.height) <= max_size / result.width / 4,
            "RGBA16 display dimensions overflow address space");
    const std::size_t count = std::size_t(result.width) * result.height * 4;
    require(count <= std::vector<std::uint8_t>().max_size(), "BGRA8 display allocation is too large");
    std::vector<std::uint8_t> bgra(count);
    const auto byte = [](std::uint16_t value) {
        return std::uint8_t((std::uint32_t(value) + 128u) / 257u);
    };
    for (std::size_t y = 0; y < result.height; ++y) {
        const auto* src = result.rgba16 + y * result.row_stride_px * 4;
        auto* dst = bgra.data() + y * result.width * 4;
        for (std::size_t x = 0; x < result.width; ++x) {
            if (src[4 * x + 3] != 65535)
                throw std::runtime_error("Display requires opaque RGBA16 pixels");
            dst[4 * x] = byte(src[4 * x + 2]);
            dst[4 * x + 1] = byte(src[4 * x + 1]);
            dst[4 * x + 2] = byte(src[4 * x]);
            dst[4 * x + 3] = 255;
        }
    }
    return bgra;
}

struct PreviewHost::Impl {
    fs::path resources;
    Catalog catalog;
    double engine_create_ms = 0.0;
    EnginePtr engine{nullptr, &spk_engine_destroy};
    // Declaration order makes session release precede engine destruction.
    SessionPtr session{nullptr, &spk_session_release};
    // Retained only for reopening after a failed settings/render operation.
    // Ordinary parameter edits reuse the current GPU source and negative.
    io::DecodedRaw decoded;
    FramePtr frame;
};

PreviewHost::PreviewHost(const fs::path& resources) : impl_(std::make_unique<Impl>()) {
    impl_->resources = fs::absolute(resources);
    // The engine's file opens go through spk::file_path, which turns the
    // UTF-8 directory into a wide path on Windows — a non-ASCII install
    // folder (a user name, a project name) is supported end to end. The
    // old ASCII gate here predated that helper and silently greyed out
    // the whole app for anyone launching it from one.
    const auto path = utf8(impl_->resources);
    require(fs::is_directory(impl_->resources), "Engine resources directory is missing");
    impl_->catalog = read_catalog(impl_->resources);
    const auto started = Clock::now();
    impl_->engine.reset(spk_engine_create(path.c_str(), nullptr));
    impl_->engine_create_ms = elapsed(started);
    require(bool(impl_->engine), "Engine creation failed: " + std::string(spk_last_error(nullptr)));
}

PreviewHost::~PreviewHost() = default;
const Catalog& PreviewHost::catalog() const noexcept { return impl_->catalog; }
double PreviewHost::engine_create_ms() const noexcept { return impl_->engine_create_ms; }
FramePtr PreviewHost::current() const noexcept { return impl_->frame; }

std::vector<std::uint8_t> PreviewHost::plain_decode_preview(int preview_long_edge,
                                                            std::uint32_t* out_width,
                                                            std::uint32_t* out_height) const {
    // The retained decode: linear ProPhoto RGB float, the RAW's own pixels.
    const auto& d = impl_->decoded;
    if (d.rgb.empty() || d.width == 0 || d.height == 0)
        throw std::runtime_error("Nothing is decoded to compare against");
    // Working->display numbers; the decoded pixels are LINEAR, so the
    // source decode step is identity (mode 4) regardless of the reply's
    // encoded-space default.
    auto setup = outputTransform("ProPhoto RGB", "sRGB");
    setup.source_cctf_mode = 4;
    const double k = preview_long_edge > 0
        ? std::min(1.0, double(preview_long_edge) / double(std::max(d.width, d.height)))
        : 1.0;
    const std::uint32_t ow = std::max(1u, std::uint32_t(d.width * k + 0.5));
    const std::uint32_t oh = std::max(1u, std::uint32_t(d.height * k + 0.5));
    std::vector<std::uint8_t> out(size_t(ow) * oh * 3, 0);
    for (std::uint32_t y = 0; y < oh; ++y) {
        const std::uint32_t sy = std::min(std::uint32_t(double(y) / k), d.height - 1);
        auto* dst = out.data() + size_t(y) * ow * 3;
        for (std::uint32_t x = 0; x < ow; ++x) {
            const std::uint32_t sx = std::min(std::uint32_t(double(x) / k), d.width - 1);
            const float* src = d.rgb.data() + (size_t(sy) * d.width + sx) * 3;
            for (int c = 0; c < 3; ++c)
                dst[x * 3 + c] = std::uint8_t(std::clamp(src[c], 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
    if (out_width) *out_width = ow;
    if (out_height) *out_height = oh;
    return out;
}

FramePtr PreviewHost::open_raw(const fs::path& source, DecodeMode mode, const RenderSettings& settings) {
    const auto started = Clock::now();
    const bool scan_film = validate_settings(settings, impl_->catalog);
    require(mode == DecodeMode::compatible16 || mode == DecodeMode::headroom, "Unknown RAW decode mode");
    // The WB override is a decode-side parameter and the headroom policy
    // keeps the file's own as-shot multipliers, so the pair is refused
    // instead of silently ignored.
    require(!(settings.wb_temperature_k != 0.0 && mode == DecodeMode::headroom),
            "A custom white balance is not available in headroom decode mode");
    const auto path = fs::absolute(source);
    require(fs::is_regular_file(path), "RAW input is missing: " + utf8(path));
    auto next = std::shared_ptr<Frame>(new Frame());
    next->source = path;
    next->decode_mode = mode;
    next->settings = settings;
    next->scan_film = scan_film;
    io::DecodedRaw decoded;
    std::string error;
    auto t = Clock::now();
    const io::RawWhiteBalance white_balance{settings.wb_temperature_k, settings.wb_tint};
    const bool decoded_ok = mode == DecodeMode::headroom
        ? io::decode_raw_headroom(path, decoded, error)
        : settings.wb_temperature_k != 0.0
            ? io::decode_raw_compatible(path, white_balance, decoded, error)
            : io::decode_raw_compatible(path, decoded, error);
    require(decoded_ok, "RAW decode: " + error);
    next->timings.decode_ms = elapsed(t);
    next->metadata = decoded.metadata;
    next->decode_timings = decoded.timings;
    t = Clock::now();
    auto session = open_session(impl_->engine.get(), decoded, settings, scan_film,
                                next->output_color_space, next->output_cctf_encoding);
    next->timings.open_ms = elapsed(t);
    t = Clock::now();
    const auto status = spk_render(session.get(), "full", &next->result_);
    require(status == SPK_OK, "Render: " + std::string(spk_last_error(impl_->engine.get())));
    next->timings.render_ms = elapsed(t);
    next->timings.engine_render_ms = next->result_.elapsed_ms;
    t = Clock::now();
    next->bgra8 = rgba16_to_bgra8(next->result_);
    next->timings.display_ms = elapsed(t);
    // Commit only after all allocations and GPU/CPU work succeeded.
    impl_->session = std::move(session);
    impl_->decoded = std::move(decoded);
    impl_->frame = next;
    next->timings.total_ms = elapsed(started);
    return next;
}

FramePtr PreviewHost::open_pair(const float* piece, std::uint32_t width, std::uint32_t height,
                                const fs::path& source, const io::RawMetadata& metadata,
                                const RenderSettings& settings) {
    const auto started = Clock::now();
    const bool scan_film = validate_settings(settings, impl_->catalog);
    require(piece != nullptr && width > 0 && height > 0, "The pair piece carries no pixels");
    auto next = std::shared_ptr<Frame>(new Frame());
    next->source = fs::absolute(source);
    next->decode_mode = DecodeMode::compatible16;
    next->settings = settings;
    next->scan_film = scan_film;
    next->metadata = metadata;
    const spk_image input{piece, width, height, 3};
    auto t = Clock::now();
    const auto params = opening_params(settings, scan_film).dump();
    char* pair_reply = nullptr;
    SessionPtr session(spk_open(impl_->engine.get(), &input, params.c_str(), &pair_reply),
                       &spk_session_release);
    require(bool(session), "Open pair piece: " + std::string(spk_last_error(impl_->engine.get())));
    resolved_working_space(pair_reply, next->output_color_space, next->output_cctf_encoding);
    next->timings.open_ms = elapsed(t);
    t = Clock::now();
    const auto status = spk_render(session.get(), "full", &next->result_);
    require(status == SPK_OK, "Render: " + std::string(spk_last_error(impl_->engine.get())));
    next->timings.render_ms = elapsed(t);
    next->timings.engine_render_ms = next->result_.elapsed_ms;
    t = Clock::now();
    next->bgra8 = rgba16_to_bgra8(next->result_);
    next->timings.display_ms = elapsed(t);
    // Commit only after all allocations and GPU/CPU work succeeded. The pair
    // has no retained RAW decode: impl_->decoded is cleared, so a failed
    // settings operation afterwards is a clean error instead of a rebuild
    // from an empty input (the piece must be handed over again).
    impl_->session = std::move(session);
    impl_->decoded = io::DecodedRaw{};
    impl_->frame = next;
    next->timings.total_ms = elapsed(started);
    return next;
}

FramePtr PreviewHost::render(const RenderSettings& settings) {
    const auto started = Clock::now();
    require(bool(impl_->frame), "Open a RAW image before rendering");
    const bool scan_film = validate_settings(settings, impl_->catalog);
    auto next = std::shared_ptr<Frame>(new Frame());
    next->source = impl_->frame->source;
    next->decode_mode = impl_->frame->decode_mode;
    next->settings = settings;
    next->scan_film = scan_film;
    next->metadata = impl_->frame->metadata;
    next->decode_timings = impl_->frame->decode_timings;
    // The working space is the open reply's (this host's deltas never touch
    // it); a rebuilt session re-reads it below, a reused one carries it.
    next->output_color_space = impl_->frame->output_color_space;
    next->output_cctf_encoding = impl_->frame->output_cctf_encoding;
    const auto delta = changed_params(impl_->frame->settings, settings, scan_film);
    try {
        auto t = Clock::now();
        if (!impl_->session) {
            // C API settings changes may fail after applying part of their
            // state. Never reuse such a session; rebuild from the retained
            // decoded input on the next request, without decoding the file.
            impl_->session = open_session(impl_->engine.get(), impl_->decoded, settings, scan_film,
                                          next->output_color_space, next->output_cctf_encoding);
            next->timings.open_ms = elapsed(t);
        } else if (!delta.fields().empty()) {
            const auto text = delta.dump();
            const auto status = spk_set_params(impl_->session.get(), text.c_str(), nullptr);
            require(status == SPK_OK, "Apply settings: " + std::string(spk_last_error(impl_->engine.get())));
            next->timings.params_ms = elapsed(t);
        }
        t = Clock::now();
        const auto status = spk_reprint(impl_->session.get(), "full", &next->result_);
        require(status == SPK_OK, "Render: " + std::string(spk_last_error(impl_->engine.get())));
        next->timings.render_ms = elapsed(t);
        next->timings.engine_render_ms = next->result_.elapsed_ms;
        t = Clock::now();
        next->bgra8 = rgba16_to_bgra8(next->result_);
        next->timings.display_ms = elapsed(t);
    } catch (...) {
        impl_->session.reset();
        throw;
    }
    impl_->frame = next;
    next->timings.total_ms = elapsed(started);
    return next;
}

std::string PreviewHost::sceneLatitude(const std::string& request_json) const {
    // A failed settings/render operation resets the session while the last
    // frame stays on screen; the analysis needs the live session itself.
    require(bool(impl_->session), "Open a RAW image before analysing scene latitude");
    const char* request = request_json.empty() ? nullptr : request_json.c_str();
    char* json = nullptr;
    const auto status = spk_scene_latitude(impl_->session.get(), request, &json);
    require(status == SPK_OK && json,
            "Scene latitude analysis: " + std::string(spk_last_error(impl_->engine.get())));
    std::string reply(json);
    spk_string_free(json);
    return reply;
}

std::string PreviewHost::overscanGeometry() const {
    // The same live-session requirement: the gate rectangles answer for the
    // canvas the last render drew.
    require(bool(impl_->session), "Open a RAW image before reading overscan geometry");
    char* json = nullptr;
    const auto status = spk_overscan_geometry(impl_->session.get(), &json);
    require(status == SPK_OK && json,
            "Overscan geometry: " + std::string(spk_last_error(impl_->engine.get())));
    std::string reply(json);
    spk_string_free(json);
    return reply;
}

OutputTransformData PreviewHost::outputTransform(const std::string& src_cs,
                                                 const std::string& dst_cs,
                                                 const std::string& gamut_compress_json) const {
    // A fetch, not a render: only the engine is touched, and the reply's
    // table pointer is copied out before this returns (the engine owns it,
    // and this wrapper keeps nothing alive behind the caller's struct).
    const char* gamut = gamut_compress_json.empty() ? nullptr : gamut_compress_json.c_str();
    char* json = nullptr;
    const float* cmax = nullptr;
    std::uint32_t cmax_count = 0;
    const auto status = spk_output_transform(impl_->engine.get(), src_cs.c_str(), dst_cs.c_str(),
                                             gamut, &json, &cmax, &cmax_count);
    require(status == SPK_OK && json,
            "Output transform: " + std::string(spk_last_error(impl_->engine.get())));
    const std::string reply(json);
    spk_string_free(json);
    OutputTransformData data;
    Json parsed;
    std::string error;
    require(Json::parse(reply, parsed, error), "Output transform reply: " + error);
    data.source_space = parsed.at("source_color_space").as_string();
    data.target_space = parsed.at("target_color_space").as_string();
    const Json& source_mode = parsed.at("source_cctf_mode");
    const Json& target_mode = parsed.at("target_cctf_mode");
    require(source_mode.is_number() && target_mode.is_number(),
            "Output transform reply carries no transfer-function modes");
    data.source_cctf_mode = std::uint32_t(source_mode.as_double());
    data.target_cctf_mode = std::uint32_t(target_mode.as_double());
    const auto nine = [](const Json& v, double (&out)[9], const char* what) {
        require(v.is_array() && v.items().size() == 9,
                std::string("Output transform reply: ") + what + " is not nine numbers");
        for (std::size_t i = 0; i < 9; ++i) out[i] = v.items()[i].as_double();
    };
    nine(parsed.at("matrix"), data.matrix, "matrix");
    const Json& compress = parsed.at("gamut_compress");
    data.algorithm = compress.at("algorithm").as_string();
    data.lightness_compression_active = compress.at("lightness_compression_active").as_bool();
    nine(compress.at("m_to_xyz"), data.m_to_xyz, "m_to_xyz");
    nine(compress.at("m_to_rgb"), data.m_to_rgb, "m_to_rgb");
    const Json& k = compress.at("k");
    require(k.is_array() && k.items().size() == 22, "Output transform reply: k is not 22 scalars");
    for (std::size_t i = 0; i < 22; ++i) data.k[i] = k.items()[i].as_double();
    const Json& rows = compress.at("cmax_rows");
    const Json& cols = compress.at("cmax_cols");
    require(rows.is_number() && cols.is_number(), "Output transform reply carries no C_max shape");
    data.cmax_rows = std::uint32_t(rows.as_double());
    data.cmax_cols = std::uint32_t(cols.as_double());
    require(cmax != nullptr && cmax_count == data.cmax_rows * data.cmax_cols && cmax_count > 0,
            "Output transform reply's C_max table does not fit its shape");
    data.cmax.assign(cmax, cmax + cmax_count);
    return data;
}

double PreviewHost::solveExposure(const spk_image& input, const std::string& params_delta_json) const {
    // A hole's meter (PAIR-WIRING §4): its own small session, opened and
    // released here -- the live session, its negative and its frame are
    // never touched, and nothing replaces impl_->frame.
    require(input.data != nullptr && input.width > 0 && input.height > 0 &&
                (input.channels == 3 || input.channels == 4),
            "Exposure meter: invalid input image");
    spk_image meter_input = input;
    std::vector<float> downscaled;
    constexpr double kMeterLongEdge = 1024.0;
    const double scale = std::min(1.0, kMeterLongEdge / double(std::max(input.width, input.height)));
    if (scale < 1.0) {
        const auto width = std::max(1, int(std::floor(input.width * scale)));
        const auto height = std::max(1, int(std::floor(input.height * scale)));
        downscaled = meter_scale(input, width, height);
        meter_input = {downscaled.data(), std::uint32_t(width), std::uint32_t(height), input.channels};
    }
    SessionPtr session(spk_open(impl_->engine.get(), &meter_input, params_delta_json.c_str(), nullptr),
                       &spk_session_release);
    require(bool(session), "Exposure meter open: " + std::string(spk_last_error(impl_->engine.get())));
    char* json = nullptr;
    const auto status = spk_solve(session.get(), "exposure", &json);
    require(status == SPK_OK && json,
            "Exposure meter solve: " + std::string(spk_last_error(impl_->engine.get())));
    const std::string reply(json);
    spk_string_free(json);
    session.reset();  // the meter's session is done the moment it has read
    Json solved;
    std::string error;
    require(Json::parse(reply, solved, error), "Exposure meter reply: " + error);
    const Json ev = solved.at("solved_params").at("exposure_compensation_ev");
    require(ev.is_number(), "Exposure meter reply carries no exposure_compensation_ev");
    return ev.as_double();
}

void PreviewHost::export_tiff(const Frame& frame, const fs::path& destination) const {
    const auto& pixels = frame.pixels();
    std::string error;
    const bool written = io::write_srgb_tiff(destination, pixels.rgba16, pixels.width, pixels.height,
                                            pixels.row_stride_px, impl_->resources / "io" / "sRGB.icc", error);
    require(written, "TIFF export: " + error);
}

}  // namespace spk::desktop
