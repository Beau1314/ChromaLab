// Windows desktop host. Rendering and image I/O stay behind their existing
// boundaries; the UI only receives immutable, independently owned frames.
#pragma once

#include "io/raw_decoder.hpp"
#include "spektralab/spk_engine.h"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace spk::desktop {

enum class DecodeMode { compatible16, headroom };

struct RenderSettings {
    std::string film_stock = "kodak_portra_400";
    std::string print_stock = "kodak_portra_endura";
    double print_exposure = 1.0;
    // Film exposure places the scene on the characteristic curve. The
    // enlarger compensates its mid-grey; print_exposure controls brightness.
    double film_exposure_ev = 0.0;
    // Preserve the engine's existing 35 mm default, including grain scale.
    double film_format_mm = 35.0;
    bool grain_active = true;
    double grain_amount = 1.0;
    bool halation_active = true;
    double halation_amount = 1.0;
    bool glare_active = true;
    double glare_amount = 1.0;
    double y_filter_shift = 0.0;
    double m_filter_shift = 0.0;
    // Decode-side white balance as Temperature/Tint (Adobe DNG semantics;
    // the RAW decoder turns a non-zero override into multipliers via the
    // file's own camera matrix). 0/0 = As Shot, the sentinel the UI shows as
    // 自动. Because it is a decode parameter, changing it re-opens the image
    // (a transactional decode+render) instead of re-rendering the session.
    double wb_temperature_k = 0.0;
    double wb_tint = 0.0;
    // The camera's meter. true is the engine's own default (params.hpp) and
    // this port's, so an unchanged settings tree renders what an unedited
    // open always rendered; false removes the meter's placement, leaving
    // exposure_compensation_ev as the raw film placement. Unlike the WB pair
    // this is an engine parameter: a change re-renders without re-decoding.
    bool auto_exposure = true;
    // RFC-028's Cineon Digital Intermediate replaces the paper stage: the
    // developed negative is read in printing density, reversed and written
    // as Cineon log (`spk_di_encode`, ported in phase 2). Engine parameters
    // like the meter: a change re-renders without re-decoding. The engine
    // keeps the DI inactive for slide film (there is no negative to
    // reverse), so the UI gates the rows on the film's catalog metadata.
    bool di_active = false;
    bool di_blue_compensation = false;
    // RFC-023's Scene Latitude Mapping (ported in phase 2, `spk_scene_latitude`
    // on Vulkan): the resolved curve the render places on the scene before the
    // film. Like the DI these are engine parameters -- a change re-renders
    // without re-decoding. The full fit travels here (not just the toggle) so
    // the latitude card's 提交拟合 can write the analysis's solved pull-backs
    // through the ordinary changed-params channel, and an on session restores
    // as one on reopen.
    bool scene_latitude_active = false;
    std::string scene_latitude_norm = "power";
    double scene_latitude_highlight_knee = 2.0;
    double scene_latitude_highlight_room = 0.0;
    double scene_latitude_shadow_knee = -2.0;
    double scene_latitude_shadow_room = 0.0;
    double scene_latitude_rolloff = 2.0;
    double scene_latitude_max_lift = 4.0;
    // The half-frame pair's second frame (PAIR-WIRING §3): `split` is where
    // its curve takes over -- the gap's middle along the long edge, 0 when
    // there is no pair -- and the b_* rows are its own resolved placement,
    // the same rows the first frame's curve has. Engine parameters like the
    // mapping above: they ride with every open and every diff, and the
    // engine applies them only while a split is set.
    double scene_latitude_split = 0.0;
    bool scene_latitude_b_active = false;
    double scene_latitude_b_highlight_knee = 2.0;
    double scene_latitude_b_highlight_room = 0.0;
    double scene_latitude_b_shadow_knee = -2.0;
    double scene_latitude_b_shadow_room = 0.0;
    // RFC-024's virtual contrast mask (ported in phase 2, `spk_mask_reduce` /
    // `spk_mask_epilogue` on Vulkan): the print-side local contrast that lifts
    // print highlights and lowers print shadows past its identity core. Like
    // the DI and the mapping these are engine parameters -- a change re-renders
    // without re-decoding. The engine keeps the mask off for slide film (a
    // directly scanned positive), a positive paper, and the LUT path
    // (RFC-024 §7.5), so the UI gates the rows on the film's catalog metadata.
    // The scheme is not a setting: only "gaussian" is a product scheme
    // (params.hpp), and the UI never exposes a second one.
    bool contrast_mask_active = false;
    double contrast_mask_highlights = 0.0;  ///< stops raised, print-highlight side
    double contrast_mask_shadows = 0.0;     ///< stops lowered, print-shadow side
    double contrast_mask_core = 1.0;        ///< half-width of the identity core, stops
    double contrast_mask_scale = 0.03;      ///< base extractor's sigma, long-edge fraction
    // The slide's HDR scan (2026-11-06, ported in phase 2, `spk_scan_hdr_expand`
    // on Vulkan): each dye layer's shoulder moved above its own knee and the
    // layer lifted inside the spectral scan, so the clear base lands at
    // `scan_hdr_headroom` x today's (the table solve is entirely CPU double in
    // core/scan_hdr.cpp; the kernel only expands per pixel). Only a positive
    // film scanned has it -- a negative film ignores the fields, and
    // headroom <= 1 is today's scan byte for byte. The stretch variant solves
    // its own headroom (the clear base at the page's white) and wins over the
    // manual one. Like the DI and the mask these are engine parameters -- a
    // change re-renders without re-decoding -- and the UI gates the rows on
    // the film's catalog metadata (positive films only).
    double scan_hdr_headroom = 0.0;
    bool scan_hdr_stretch = false;
    double scan_hdr_output_scale = 1.0;
    // RFC-031/032's film edge and date back (ported in phase 2, the four
    // `spk_overscan_*` kernels on Vulkan, the edge print drawn through
    // edge_raster.cpp): the film outside the frame, and the date exposed onto
    // the negative from behind. Engine parameters like the scan HDR fields --
    // a change re-renders without re-decoding (overscan_active is a
    // shoot-layer field, so its edits re-develop the film). The seeds fix the
    // machine (camera) and the film advance + scan (frame) per RFC-032 §26.1;
    // 0 frame_number is the seed's own draw. The date is already formatted
    // text ("'26 11 15"); the data face (date_imprint data_text) prints the
    // shooting data as well as the date. The defaults are the engine's own
    // (params.hpp): everything off, format 135, white holes, no carrier.
    bool overscan_active = false;
    std::string overscan_mode = "strip";      // strip | filed
    std::string overscan_format = "135";      // 135 | 135_half | 120_645 | ... | 120_6x17
    std::string overscan_gate = "auto";       // auto | square | rounded | eared | shouldered | kicked
    std::string overscan_holes = "white";     // white (the scan's light) | black
    std::string overscan_carrier = "none";    // none | black | open
    std::string overscan_edge_text;           // the manufacturer's edge print
    int overscan_frame_number = 0;            // 0 = the frame seed's draw
    int overscan_frame_seed = 1;
    int overscan_camera_seed = 1;
    double overscan_f_number = 0.0;           // EXIF; 0 = unknown (f/5.6 assumed)
    double overscan_fog = 1.0;                // edge fog, 0 = none
    double overscan_leaks = 0.0;              // spool light leaks, 0 = none
    bool overscan_pair = false;               // a half-frame pair on one strip
    bool date_imprint_active = false;
    std::string date_imprint_text;            // already formatted, e.g. "'26 11 15"
    std::string date_imprint_text_b;          // a pair's second frame's date; empty = none on it
    std::string date_imprint_data_text;       // shooting data printed as well as the date
    std::string date_imprint_placement = "frame";   // frame | rebate
    double date_imprint_exposure_ev = 3.5;    // red-layer exposure above 18 % grey
    std::string date_imprint_style = "lcd";   // lcd | dots | data
    double date_imprint_size = 1.0;           // scale on the style's own character height
    std::string date_imprint_corner = "br";   // br | bl | tr | tl, in the film's frame
    double date_imprint_inset_x = 3.0;        // mm from the gate's side to the text
    double date_imprint_inset_y = 2.4;        // mm from the gate's top/bottom to the text

    bool operator==(const RenderSettings&) const = default;
};

struct Stock {
    std::string id, label;
    bool is_positive = false;
};
struct Catalog { std::vector<Stock> films, papers; };

// Wall-clock calls, including CPU result readback in render_ms. total_ms is
// the complete host call, not a sum of nested measurements. Engine creation
// and TIFF export are separate operations. Re-renders have zero decode/open
// time unless a previous engine failure required reopening the retained input.
struct Timings {
    double decode_ms = 0.0, open_ms = 0.0, params_ms = 0.0;
    double render_ms = 0.0, engine_render_ms = 0.0;
    double display_ms = 0.0, total_ms = 0.0;
};

class PreviewHost;
class Frame final {
public:
    ~Frame();
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;

    std::filesystem::path source;
    DecodeMode decode_mode = DecodeMode::compatible16;
    RenderSettings settings;
    // Resolved from the selected film's catalog metadata, not a separate
    // user setting. Slide film is viewed directly without the paper stage.
    bool scan_film = false;
    io::RawMetadata metadata;
    io::RawDecodeTimings decode_timings;
    Timings timings;
    // Top-down tightly packed BGRA8 encoded sRGB. Only the display copy is
    // reduced to 8 bits; pixels() retains the original full-size RGBA16.
    std::vector<std::uint8_t> bgra8;

    const spk_result& pixels() const noexcept { return result_; }
    std::uint32_t width() const noexcept { return result_.width; }
    std::uint32_t height() const noexcept { return result_.height; }
    bool reprint() const noexcept { return result_.reprint != 0; }
    bool negative_was_cached() const noexcept { return result_.negative_was_cached != 0; }

private:
    Frame() = default;
    spk_result result_{};
    friend class PreviewHost;
};

using FramePtr = std::shared_ptr<const Frame>;

// Pure conversion, including row-pitch handling, used by the host and tested
// independently. Throws on invalid dimensions/pitch or non-opaque pixels.
// Rounding is (value + 128) / 257; no colour transform or tone mapping.
std::vector<std::uint8_t> rgba16_to_bgra8(const spk_result& result);

// Construct, call and destroy on one serial worker thread. Public operations
// throw std::runtime_error on failure. The UI may concurrently read shared
// const Frames, which stay valid after this host and its engine are destroyed.
class PreviewHost final {
public:
    explicit PreviewHost(const std::filesystem::path& resources);
    ~PreviewHost();
    PreviewHost(const PreviewHost&) = delete;
    PreviewHost& operator=(const PreviewHost&) = delete;

    const Catalog& catalog() const noexcept;
    double engine_create_ms() const noexcept;
    FramePtr current() const noexcept;

    // New-file open is transactional: decode/open/render/display conversion
    // must all succeed before replacing the previous session and frame.
    FramePtr open_raw(const std::filesystem::path& source, DecodeMode mode,
                      const RenderSettings& settings = {});
    // Only changed settings are sent. Print-side edits request spk_reprint;
    // the result's actual cache flag determines whether reuse occurred.
    // A failure never mutates the previously returned Frame.
    FramePtr render(const RenderSettings& settings);
    // RFC-023's scene latitude analysis of the open session, at the engine's
    // suggestion defaults (an empty `request_json` travels as NULL): the
    // measured medium, the scene statistic, the suggested pull-backs and the
    // solved fit. A non-empty request is the engine's own request JSON -- a
    // half-frame pair's fit passes `"region": [x0, y0, x1, y1]` (PAIR-WIRING
    // §6.2), normalised to the frame the engine was handed, so the reading
    // measures one picture of the pair rather than both as one scene. Never
    // renders -- the session's pixels are unchanged. Windows supports this
    // analysis; only applying the fit's mapping is refused (see
    // WINDOWS_FEATURE_COVERAGE.md). Throws std::runtime_error when no session
    // is open (or a failed render discarded it) and on engine failure, with
    // spk_last_error's text. The reply is the raw JSON object with "medium",
    // "scene", "suggested" and "fit" keys.
    std::string sceneLatitude(const std::string& request_json = "") const;
    // Where the film's gates sit on the canvas the last render drew
    // (`spk_overscan_geometry`): valid only with a film edge, and for a
    // half-frame pair the reply's "gates" array holds both. The reply is the
    // raw JSON object -- the caller parses it, keeping this host thin. Never
    // renders. Throws std::runtime_error on no open session or engine
    // failure, like sceneLatitude.
    std::string overscanGeometry() const;
    // One hole's exposure reading (PAIR-WIRING §4): opens a THROWAWAY session
    // on the supplied picture -- the live session is never touched, and no
    // frame is kept -- under the caller's metering wire (auto exposure on,
    // exposure_compensation_ev 0, film_format_mm 24, no film edge, no date),
    // asks spk_solve("exposure") and answers solved_params.
    // exposure_compensation_ev: what the meter would place the picture at.
    // A picture above 1,024 px on the long edge is downscaled first (box
    // average), because the meter is a 1,024 px session. `input` is borrowed
    // for the duration of the call. The params delta is the engine's own JSON
    // wire, validated by spk_open's schema like any other. Throws
    // std::runtime_error on an unusable image, a refused open, or a failed
    // solve, with spk_last_error's text.
    double solveExposure(const spk_image& input, const std::string& params_delta_json) const;
    // Exports exactly the supplied frame, even after a newer one was opened.
    // Existing files are never overwritten. Returns only after the writer
    // has completed and closed the file; errors retain its cleanup details.
    void export_tiff(const Frame& frame, const std::filesystem::path& destination) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace spk::desktop
