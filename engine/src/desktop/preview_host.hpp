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
    // suggestion defaults (`request_json` NULL): the measured medium, the
    // scene statistic, the suggested pull-backs and the solved fit. Never
    // renders -- the session's pixels are unchanged. Windows supports this
    // analysis; only applying the fit's mapping is refused (see
    // WINDOWS_FEATURE_COVERAGE.md). Throws std::runtime_error when no session
    // is open (or a failed render discarded it) and on engine failure, with
    // spk_last_error's text. The reply is the raw JSON object with "medium",
    // "scene", "suggested" and "fit" keys.
    std::string sceneLatitude() const;
    // Exports exactly the supplied frame, even after a newer one was opened.
    // Existing files are never overwritten. Returns only after the writer
    // has completed and closed the file; errors retain its cleanup details.
    void export_tiff(const Frame& frame, const std::filesystem::path& destination) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace spk::desktop
