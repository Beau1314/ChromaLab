// The pair wire's host side: the split/b_* render settings, the scene
// latitude request passthrough and the overscan gate rectangles. Pure in the
// CTest sense -- no RAW fixture. The synthetic pair session opens, through
// the C API, exactly the wire open_raw now sends; the buffer is composed the
// way engine/tests/overscan_checks.py composes its half-frame pair (a zeros
// gap between two gradient frames, the second one advance along).
#include "desktop/preview_host.hpp"
#include "core/json.hpp"
#include "core/params.hpp"
#include "spektralab/spk_engine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using spk::Json;
using namespace spk::desktop;

namespace {

unsigned checks = 0;

void check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

void check(bool condition, std::string message) {
    check(condition, message.c_str());
}

template<class F> std::string rejected(F&& function, const char* message) {
    try {
        function();
    } catch (const std::exception& error) {
        check(*error.what() != '\0', "rejection omitted diagnostic");
        return error.what();
    }
    throw std::runtime_error(message);
}

std::string path_text(const fs::path& path) {
    const auto utf8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

Json parse(const std::string& text) {
    Json value;
    std::string error;
    check(Json::parse(text, value, error), "engine reply is not JSON: " + error);
    return value;
}

// One half frame's picture: a deterministic gradient, no noise. The gates
// come from the wire's format and the buffer's shape, not from the pixels;
// the pattern keeps the frames pictures rather than blackouts.
std::vector<float> frame(int w, int h, double scale) {
    const double tint[3] = {1.0, 0.9, 0.7};
    std::vector<float> image(std::size_t(w) * h * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c)
                image[(std::size_t(y) * w + x) * 3 + c] =
                    float(scale * (0.05 + 0.4 * (double(x) / w) * tint[c] + 0.3 * (double(y) / h)));
    return image;
}

void pair_defaults() {
    // A desktop default must agree with the engine's own pair rows
    // (params.cpp camera.scene_latitude.split/b_*), like every other field.
    const RenderSettings settings;
    const auto& sl = spk::Params{}.camera.scene_latitude;
    check(settings.scene_latitude_split == sl.split &&
              settings.scene_latitude_b_active == sl.b_active &&
              settings.scene_latitude_b_highlight_knee == sl.b_highlight_knee &&
              settings.scene_latitude_b_highlight_room == sl.b_highlight_room &&
              settings.scene_latitude_b_shadow_knee == sl.b_shadow_knee &&
              settings.scene_latitude_b_shadow_room == sl.b_shadow_room,
          "desktop pair wire defaults differ from the engine's rows");
    auto changed = settings;
    changed.scene_latitude_split = 0.5;
    changed.scene_latitude_b_active = true;
    changed.scene_latitude_b_highlight_knee = 0.5;
    changed.scene_latitude_b_highlight_room = 1.0;
    changed.scene_latitude_b_shadow_knee = -0.5;
    changed.scene_latitude_b_shadow_room = 1.0;
    check(changed != settings, "pair wire fields do not take part in operator==");
    auto restored = changed;
    restored.scene_latitude_split = settings.scene_latitude_split;
    restored.scene_latitude_b_active = settings.scene_latitude_b_active;
    restored.scene_latitude_b_highlight_knee = settings.scene_latitude_b_highlight_knee;
    restored.scene_latitude_b_highlight_room = settings.scene_latitude_b_highlight_room;
    restored.scene_latitude_b_shadow_knee = settings.scene_latitude_b_shadow_knee;
    restored.scene_latitude_b_shadow_room = settings.scene_latitude_b_shadow_room;
    check(restored == settings, "pair wire roundtrip through operator== differs");
}

void no_session(const fs::path& resources) {
    PreviewHost host(resources);
    const auto latitude = rejected([&] { host.sceneLatitude(); },
                                   "scene latitude without a session accepted");
    const auto empty_request = rejected([&] { host.sceneLatitude(""); },
                                        "scene latitude with an empty request accepted");
    check(latitude == empty_request,
          "an empty request string changed the no-session behaviour");
    rejected([&] { host.overscanGeometry(); }, "overscan geometry without a session accepted");
    rejected([&] { host.render({}); }, "render without an open accepted");
}

void synthetic_pair(const fs::path& resources) {
    constexpr int hw = 450, hh = 600;                 // one half frame, 18 x 24 mm
    constexpr int adv = 475;                          // 19 mm of film at 18 mm / 450 px
    const auto first = frame(hw, hh, 1.0);
    const auto second = frame(hw, hh, 0.6);
    // Zeros are the unexposed gap: the second frame starts one advance along.
    std::vector<float> both(std::size_t(adv + hw) * hh * 3, 0.0f);
    for (int y = 0; y < hh; ++y) {
        const auto row = both.begin() + std::size_t(y) * (adv + hw) * 3;
        std::copy_n(&first[std::size_t(y) * hw * 3], std::size_t(hw) * 3, row);
        std::copy_n(&second[std::size_t(y) * hw * 3], std::size_t(hw) * 3, row + std::size_t(adv) * 3);
    }
    const spk_image input{both.data(), std::uint32_t(adv + hw), std::uint32_t(hh), 3};

    // The wire, the same keys open_raw now sends for a pair (PAIR-WIRING §3):
    // the piece's own format, the half-frame pair on a film edge, and the
    // split/b_* rows that give the second frame its own Scene Placement.
    Json params = Json::object();
    params.set("film_stock", Json(std::string("kodak_portra_400")));
    params.set("print_stock", Json(std::string("kodak_supra_endura")));
    params.set("grain_active", Json(false));
    params.set("glare_active", Json(false));
    params.set("auto_exposure", Json(false));
    params.set("exposure_compensation_ev", Json(0.0));
    params.set("film_format_mm", Json(37.0));
    params.set("overscan_active", Json(true));
    params.set("overscan_format", Json(std::string("135_half")));
    params.set("overscan_pair", Json(true));
    params.set("overscan_edge_text", Json(std::string("KODAK PORTRA 400")));
    const double split = (adv - 0.5 * (adv - hw)) / double(adv + hw);  // the gap's middle
    params.set("scene_latitude_split", Json(split));
    params.set("scene_latitude_b_active", Json(true));
    params.set("scene_latitude_b_highlight_knee", Json(0.5));
    params.set("scene_latitude_b_highlight_room", Json(1.0));
    params.set("scene_latitude_b_shadow_knee", Json(-0.5));
    params.set("scene_latitude_b_shadow_room", Json(1.0));

    spk_engine* engine = spk_engine_create(path_text(resources).c_str(), nullptr);
    check(engine, "engine creation failed");
    spk_session* session = spk_open(engine, &input, params.dump().c_str(), nullptr);
    check(session != nullptr,
          std::string("the pair wire was refused at open: ") + spk_last_error(engine));
    spk_result result{};
    check(spk_render(session, "live", &result) == SPK_OK,
          std::string("the pair did not render: ") + spk_last_error(engine));
    spk_result_free(&result);
    char* json = nullptr;
    check(spk_overscan_geometry(session, &json) == SPK_OK && json != nullptr,
          std::string("no overscan geometry reply: ") + spk_last_error(engine));
    const std::string reply(json);
    spk_string_free(json);
    spk_session_release(session);
    spk_engine_destroy(engine);

    const Json geometry = parse(reply);
    check(geometry.at("valid").as_bool(), "the pair's gate geometry is not valid");
    const auto& gates = geometry.at("gates").items();
    check(gates.size() == 2, "a pair on its strip does not show two gates");
    const double canvas_w = geometry.at("canvas_w").as_double();
    const double canvas_h = geometry.at("canvas_h").as_double();
    check(canvas_w > 0 && canvas_h > 0, "the geometry's canvas is empty");
    for (const auto& gate : gates) {
        check(gate.items().size() == 8, "a gate is not four corners");
        const double w = (gate.items()[2].as_double() - gate.items()[0].as_double()) * canvas_w;
        const double h = (gate.items()[7].as_double() - gate.items()[1].as_double()) * canvas_h;
        check(std::abs(w - hw) < 3.0 && std::abs(h - hh) < 3.0,
              "a gate is not one half frame of the piece");
    }
    // The reply lists the gates in the film's order for a level pair: the
    // second sits one advance along.
    const double step =
        (gates[1].items()[0].as_double() - gates[0].items()[0].as_double()) * canvas_w;
    check(std::abs(step - adv) < 2.0,
          "the two gates are not one advance apart on the canvas");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const fs::path resources = argc > 1 ? fs::path(argv[1]) : fs::path("resources");
        pair_defaults();
        no_session(resources);
        synthetic_pair(resources);
        std::cout << "Pair host boundary: " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
