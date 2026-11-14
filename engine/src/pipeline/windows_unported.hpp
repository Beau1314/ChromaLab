// Explicit boundaries for effects added upstream after the first Windows port.
// Included only by Windows call sites; macOS retains its upstream behavior.
// Three families have left this list as phase 2 progressed, all ordinary
// engine parameters here now: the Cineon Digital Intermediate
// (`spk_di_encode`), RFC-023's Scene Latitude Mapping (`spk_scene_latitude` /
// `spk_scene_curve`), and RFC-024's contrast mask (`spk_mask_reduce` /
// `spk_mask_epilogue` -- the host half is portable C++ once `for_each_line`
// runs on the worker pool instead of GCD; see line_pool.hpp).
#pragma once
#include "params.hpp"

namespace spk {

inline const char* windows_unported_error(bool overscan, bool date, bool scan_hdr = false) {
    if (overscan) return "overscan is not implemented by the Windows Vulkan backend yet";
    if (date) return "date imprint is not implemented by the Windows Vulkan backend yet";
    // `scan_hdr` (2026-11-06): `spk_scan_hdr_expand` is Metal only.
    if (scan_hdr) return "scan_hdr is not implemented by the Windows Vulkan backend yet";
    return nullptr;
}

inline const char* windows_unported_error(const Params& p) {
    return windows_unported_error(p.film_render.overscan.active,
                                  p.film_render.date_imprint.active,
                                  p.io.scan_film && (p.io.scan_hdr_stretch || p.io.scan_hdr_headroom > 1.0));
}

// Validate an already schema-checked delta before it mutates the session.
// A valid Windows session cannot already have any of these flags enabled.
inline const char* windows_unported_delta_error(const Json& delta) {
    return windows_unported_error(delta.at("overscan_active").as_bool(),
                                  delta.at("date_imprint_active").as_bool(),
                                  // Conservative: the delta alone does not say the film is a
                                  // slide scanned, so any request for the variant is refused.
                                  delta.at("scan_hdr_stretch").as_bool() ||
                                      delta.at("scan_hdr_headroom").as_double(0.0) > 1.0);
}

}  // namespace spk
