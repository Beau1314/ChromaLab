// Explicit boundaries for effects added upstream after the first Windows port.
// Included only by Windows call sites; macOS retains its upstream behavior.
// Four families have left this list as phase 2 progressed, all ordinary
// engine parameters here now: the Cineon Digital Intermediate
// (`spk_di_encode`), RFC-023's Scene Latitude Mapping (`spk_scene_latitude` /
// `spk_scene_curve`), RFC-024's contrast mask (`spk_mask_reduce` /
// `spk_mask_epilogue` -- the host half is portable C++ once `for_each_line`
// runs on the worker pool instead of GCD; see line_pool.hpp), and 2026-11-06's
// slide HDR scan (`spk_scan_hdr_expand`; the table solve was always portable
// CPU double in core/scan_hdr.cpp).
#pragma once
#include "params.hpp"

namespace spk {

inline const char* windows_unported_error(bool overscan, bool date) {
    if (overscan) return "overscan is not implemented by the Windows Vulkan backend yet";
    if (date) return "date imprint is not implemented by the Windows Vulkan backend yet";
    return nullptr;
}

inline const char* windows_unported_error(const Params& p) {
    return windows_unported_error(p.film_render.overscan.active,
                                  p.film_render.date_imprint.active);
}

// Validate an already schema-checked delta before it mutates the session.
// A valid Windows session cannot already have any of these flags enabled.
inline const char* windows_unported_delta_error(const Json& delta) {
    return windows_unported_error(delta.at("overscan_active").as_bool(),
                                  delta.at("date_imprint_active").as_bool());
}

}  // namespace spk
