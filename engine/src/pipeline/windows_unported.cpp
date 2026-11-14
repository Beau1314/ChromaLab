// Off-path adapters for upstream effects that still depend on Apple services.
// Mac build.sh globs pipeline/*.cpp, so the whole translation unit is guarded.
// The contrast mask left this file in phase 2 (RFC-024's host half is plain
// C++ once `for_each_line` runs on the worker pool; line_pool.hpp keeps the
// GCD path on Apple), so only the overscan family stands in here now.
#ifndef __APPLE__
#include "pipeline.hpp"
#include "windows_unported.hpp"

namespace spk {

bool Pipeline::overscan_wanted() const { return params_.film_render.overscan.active; }

double Pipeline::overscan_gate_long_mm() const { return params_.camera.film_format_mm; }

void Pipeline::overscan_frame(uint32_t& frame_w, uint32_t& frame_h) const {
    frame_w = frame_h = 0;
}

bool Pipeline::set_overscan_frame(uint32_t, uint32_t, std::string& error) {
    if (!overscan_wanted()) return true;
    error = windows_unported_error(false, true);
    return false;
}

// Main added spk_overscan_geometry after the Windows port's root (unpublished-revision);
// the clean textual merge left this undefined and the non-Apple engine did
// not link. Overscan is refused here, so there is never a layout to report.
std::string Pipeline::overscan_geometry_json() const { return "{\"valid\":false}"; }

bool Pipeline::node_overscan(const Image& in, Image& out, std::string& error) {
    if (params_.film_render.overscan.active || params_.film_render.date_imprint.active) {
        error = windows_unported_error(false, params_.film_render.overscan.active,
                                       params_.film_render.date_imprint.active);
        return false;
    }
    out = in;
    return true;
}

bool Pipeline::node_overscan_film_present(const Image& in, Image& out, std::string& error) {
    return node_overscan(in, out, error);
}

bool Pipeline::node_overscan_light(const Image& in, Image& out, std::string& error) {
    return node_overscan(in, out, error);
}

}  // namespace spk
#endif  // !__APPLE__
