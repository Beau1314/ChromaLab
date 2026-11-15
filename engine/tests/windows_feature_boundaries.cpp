// Exercise Windows feature refusals through the exported DLL boundary.
#include "spektralab/spk_engine.h"

#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string params(spk_session* session) {
    char* json = nullptr;
    require(spk_get_params(session, &json) == SPK_OK && json, "get_params failed");
    std::string result(json);
    spk_string_free(json);
    return result;
}

void render(spk_engine* engine, spk_session* session) {
    spk_result result{};
    const spk_status status = spk_render(session, "full", &result);
    const bool valid = status == SPK_OK && result.rgba16 && result.texture &&
                       result.width == 32 && result.height == 16;
    const std::string error = spk_last_error(engine);
    spk_result_free(&result);
    require(valid, "default render failed after a refused request: " + error);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "usage: spk_windows_feature_boundaries resources_dir\n"; return 2; }
    try {
        std::unique_ptr<spk_engine, decltype(&spk_engine_destroy)> engine(
            spk_engine_create(argv[1], nullptr), spk_engine_destroy);
        require(bool(engine), std::string("engine creation failed: ") + spk_last_error(nullptr));
        const char* capabilities = spk_capabilities(engine.get());
        require(capabilities && std::string(capabilities).find("unsupported_features") != std::string::npos,
                "Windows capabilities must disclose the unported-effects list");
        // The five ported effects (phase 2) must stay off the disclosed list:
        // the Cineon Digital Intermediate, RFC-023's Scene Latitude Mapping,
        // RFC-024's contrast mask, the slide HDR scan, and RFC-031/032's
        // overscan + date back (this task).
        require(std::string(capabilities).find("\"digital_intermediate\"") == std::string::npos,
                "ported digital intermediate still listed as unsupported");
        require(std::string(capabilities).find("scene_latitude_mapping") == std::string::npos,
                "ported scene latitude mapping still listed as unsupported");
        require(std::string(capabilities).find("\"contrast_mask\"") == std::string::npos,
                "ported contrast mask still listed as unsupported");
        require(std::string(capabilities).find("\"scan_hdr\"") == std::string::npos,
                "ported scan_hdr still listed as unsupported");
        require(std::string(capabilities).find("\"overscan\"") == std::string::npos,
                "ported overscan still listed as unsupported");
        require(std::string(capabilities).find("\"date_imprint\"") == std::string::npos,
                "ported date_imprint still listed as unsupported");

        std::vector<float> pixels(32 * 16 * 3);
        for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = 0.025f + float(i % 31) * 0.025f;
        // A ring of speculars and deep shadows pushes the scene past the
        // medium's measured boundaries, so the latitude fit below has real
        // pull-backs to solve: grey 2.0 sits about +3.4 stops about the
        // metered mid-grey (over the ~+2.4 EV highlight boundary) and grey
        // 0.0035 about -5.7 (under the ~-4.6 EV shadow boundary), while the
        // bulk stays inside. The meter is off so the scene's stop axis is the
        // pixels' own, with no placement gain shifting the extremes.
        constexpr size_t kExtremes = 12;
        for (size_t i = 0; i < kExtremes; ++i) {
            pixels[3 * i] = pixels[3 * i + 1] = pixels[3 * i + 2] = 2.0f;
            pixels[3 * (pixels.size() / 3 - 1 - i)] =
                pixels[3 * (pixels.size() / 3 - 1 - i) + 1] =
                pixels[3 * (pixels.size() / 3 - 1 - i) + 2] = 0.0035f;
        }
        const spk_image input{pixels.data(), 32, 16, 3};
        std::unique_ptr<spk_session, decltype(&spk_session_release)> session(
            spk_open(engine.get(), &input, "{\"auto_exposure\":false}", nullptr), spk_session_release);
        require(bool(session), std::string("default open failed: ") + spk_last_error(engine.get()));
        render(engine.get(), session.get());
        const std::string before = params(session.get());

        // RFC-031/032's film edge and date back were ported in the same phase
        // (this task): both toggles are ordinary engine parameters now. The
        // same apply-with-a-supported-change channel the DI and the mapping
        // use; each reverts so the later `before` comparisons hold. The
        // fixture's 32x16 frame is the 120_6x12 gate's shape exactly (its
        // 112x56 gate is 2:1), so a render with the film edge on has a real
        // canvas to build, and `spk_overscan_geometry` reports the gate it
        // laid out.
        require(spk_set_params(session.get(),
                               "{\"overscan_active\":true,\"overscan_format\":\"120_6x12\","
                               "\"overscan_edge_text\":\"KODAK PORTRA 400\",\"print_exposure\":1.25}",
                               nullptr) == SPK_OK,
                "ported overscan delta was refused");
        require(params(session.get()) != before, "overscan delta did not change params");
        // The film canvas replaces the frame, so the tier is no longer 32x16:
        // render without the fixed-size helper and check the canvas grew.
        {
            spk_result film{};
            const spk_status film_status = spk_render(session.get(), "full", &film);
            const std::string film_error = spk_last_error(engine.get());
            const bool film_ok = film_status == SPK_OK && film.rgba16 && film.texture &&
                                 (film.width != 32 || film.height != 16);
            spk_result_free(&film);
            require(film_ok, "the film edge's canvas failed to render: " + film_error);
        }
        {
            char* geometry_json = nullptr;
            const spk_status geometry_status = spk_overscan_geometry(session.get(), &geometry_json);
            const std::string geometry_error = spk_last_error(engine.get());
            const std::string geometry(geometry_json ? geometry_json : "");
            spk_string_free(geometry_json);
            require(geometry_status == SPK_OK && geometry.find("\"valid\":true") != std::string::npos &&
                        geometry.find("\"gates\":[") != std::string::npos &&
                        geometry.find("\"120_6x12\"") != std::string::npos,
                    "the overscan render must expose its gate geometry: " + geometry_error);
        }
        // An unknown format is refused at the layout (the film run), not by
        // the params channel: the delta applies, the render names the field.
        {
            require(spk_set_params(session.get(), "{\"overscan_format\":\"110\"}", nullptr) == SPK_OK,
                    "the format delta itself must apply");
            spk_result bad{};
            const spk_status bad_status = spk_render(session.get(), "full", &bad);
            spk_result_free(&bad);
            const std::string bad_error = spk_last_error(engine.get());
            require(bad_status != SPK_OK && bad_error.find("format") != std::string::npos &&
                        bad_error.find("110") != std::string::npos,
                    "an unknown overscan format must refuse the render: " + bad_error);
        }
        require(spk_set_params(session.get(),
                               "{\"overscan_active\":false,\"overscan_format\":\"135\","
                               "\"overscan_edge_text\":\"\",\"print_exposure\":1.0}",
                               nullptr) == SPK_OK,
                "overscan could not be reverted");
        require(params(session.get()) == before, "overscan round trip changed other params");
        render(engine.get(), session.get());

        require(spk_set_params(session.get(),
                               "{\"date_imprint_active\":true,\"date_imprint_text\":\"'26 11 15\","
                               "\"print_exposure\":1.25}",
                               nullptr) == SPK_OK,
                "ported date imprint delta was refused");
        require(params(session.get()) != before, "date imprint delta did not change params");
        render(engine.get(), session.get());
        require(spk_set_params(session.get(),
                               "{\"date_imprint_active\":false,\"date_imprint_text\":\"\","
                               "\"print_exposure\":1.0}",
                               nullptr) == SPK_OK,
                "date imprint could not be reverted");
        require(params(session.get()) == before, "date imprint round trip changed other params");
        render(engine.get(), session.get());

        // The ported flag is an ordinary parameter: a delta that also carries
        // a supported change applies in one request, and the session still
        // renders. Revert it so the later `before` comparisons hold.
        require(spk_set_params(session.get(),
                               "{\"digital_intermediate\":true,\"print_exposure\":1.25}",
                               nullptr) == SPK_OK,
                "ported digital_intermediate delta was refused");
        require(spk_set_params(session.get(),
                               "{\"digital_intermediate\":false,\"print_exposure\":1.0}",
                               nullptr) == SPK_OK,
                "digital_intermediate could not be reverted");
        require(params(session.get()) == before, "DI parameter round trip changed other params");
        render(engine.get(), session.get());

        // RFC-023's Scene Latitude Mapping was ported in the same phase: its
        // mapping is an ordinary engine parameter now. The toggle applies in
        // one request with a supported change; the half-frame pair's
        // second-frame flag (the reader gap that existed while the mapping was
        // refused) applies by itself through the same ordinary path; and the
        // analysis's solved fit commits verbatim through `spk_set_params`.
        // The margin turns the fixture's overshoot into real pull-backs
        // without walking either knee past the other (a full stop each side
        // keeps the highlight knee at about +1.4 EV and the shadow's inverted
        // lift solve inside the lift bound), so the committed curve is
        // non-trivial. Reverted through the engine's own defaults so the later
        // `before` comparisons hold.
        require(spk_set_params(session.get(),
                               "{\"scene_latitude_active\":true,\"print_exposure\":1.25}",
                               nullptr) == SPK_OK,
                "ported scene latitude mapping delta was refused");
        require(params(session.get()) != before, "latitude mapping delta did not change params");
        render(engine.get(), session.get());
        require(spk_set_params(session.get(), "{\"scene_latitude_b_active\":true}", nullptr) == SPK_OK,
                "the pair's second-frame flag applies through the ordinary path");
        require(params(session.get()) != before, "second-frame flag delta did not change params");
        render(engine.get(), session.get());

        {
            const char* request = "{\"margin\":1.0}";
            char* fit_json = nullptr;
            const spk_status fit_status = spk_scene_latitude(session.get(), request, &fit_json);
            const std::string fit_error = spk_last_error(engine.get());
            require(fit_status == SPK_OK && fit_json, "latitude analysis with margin failed: " + fit_error);
            const std::string reply(fit_json);
            spk_string_free(fit_json);
            // Search inside the fit object only: the suggested block before it
            // carries its own "valid". The engine's dump is compact, and the
            // params_delta is a flat object of numbers and booleans, so the
            // first '}' closes it.
            const size_t fit_at = reply.find("\"fit\":{");
            require(fit_at != std::string::npos, "the analysis reply carries no fit: " + reply);
            require(reply.find("\"valid\":true", fit_at) != std::string::npos,
                    "the wide-margin fit is not valid: " + reply);
            const size_t delta_at = reply.find("\"params_delta\":{", fit_at);
            require(delta_at != std::string::npos, "a valid fit must carry its params_delta");
            constexpr size_t kDeltaPrefix = 15;   // strlen("\"params_delta\":")
            const size_t open = delta_at + kDeltaPrefix;
            const size_t close = reply.find('}', open);
            require(close != std::string::npos, "the params_delta object is malformed");
            const std::string delta = reply.substr(open, close - open + 1);
            require(spk_set_params(session.get(), delta.c_str(), nullptr) == SPK_OK,
                    "committing the solved fit through the ordinary channel failed");
            require(params(session.get()) != before, "the committed fit did not change params");
            require(params(session.get()).find("\"scene_latitude_active\":true") != std::string::npos,
                    "the committed fit did not enable the mapping");
            render(engine.get(), session.get());
        }
        require(spk_set_params(session.get(),
                               "{\"scene_latitude_active\":false,\"scene_latitude_b_active\":false,"
                               "\"scene_latitude_norm\":\"power\","
                               "\"scene_latitude_highlight_knee\":2.0,"
                               "\"scene_latitude_highlight_room\":0.0,"
                               "\"scene_latitude_shadow_knee\":-2.0,"
                               "\"scene_latitude_shadow_room\":0.0,"
                               "\"scene_latitude_rolloff\":2.0,"
                               "\"scene_latitude_max_lift\":4.0,"
                               "\"print_exposure\":1.0}",
                               nullptr) == SPK_OK,
                "latitude mapping could not be reverted");
        require(params(session.get()) == before, "latitude parameter round trip changed other params");
        render(engine.get(), session.get());

        // The DI export renders the session's own negative as Cineon log; the
        // reply names the encoding and the request leaves the session
        // untouched. A slide film has no DI and must still be refused.
        spk_result di{};
        char* di_json = nullptr;
        const spk_status di_status = spk_render_digital_intermediate(session.get(), &di, &di_json);
        const std::string di_error = spk_last_error(engine.get());
        const bool rendered = di_status == SPK_OK && di.texture && di.rgba16 &&
                              di.width == 32 && di.height == 16 && di_json &&
                              std::string(di_json).find("cineon") != std::string::npos;
        spk_result_free(&di);
        spk_string_free(di_json);
        require(rendered, "DI render failed: " + di_error);
        require(params(session.get()) == before, "DI render mutated the session");
        render(engine.get(), session.get());

        {
            const char* slide_delta = "{\"film_stock\":\"fujifilm_provia_100f\"}";
            spk_session* slide = spk_open(engine.get(), &input, slide_delta, nullptr);
            require(bool(slide), std::string("slide session for the DI refusal failed: ") +
                                     spk_last_error(engine.get()));
            spk_result slide_di{};
            char* slide_json = nullptr;
            const spk_status slide_status =
                spk_render_digital_intermediate(slide, &slide_di, &slide_json);
            const std::string slide_error = spk_last_error(engine.get());
            const bool slide_empty = !slide_di.texture && !slide_di.rgba16 && !slide_json;
            spk_result_free(&slide_di);
            spk_string_free(slide_json);
            spk_session_release(slide);
            require(slide_status == SPK_ERR_USER && slide_empty &&
                    slide_error.find("Digital Intermediate") != std::string::npos,
                    "a slide film still must be refused the DI: " + slide_error);
        }

        // The contrast mask is an ordinary engine parameter now (RFC-024's
        // port): enabling it applies with a supported change, the render
        // carries the mask, and `spk_contrast_mask_field` returns the real
        // analysis grid -- but only once the tier's negative exists (its own
        // contract: the field is analysed on the cached negative, it does not
        // develop one). The deltas are the RFC's bounded stops.
        require(spk_set_params(session.get(),
                               "{\"contrast_mask_active\":true,\"contrast_mask_highlights\":1.25,"
                               "\"contrast_mask_shadows\":0.75,\"print_exposure\":1.25}",
                               nullptr) == SPK_OK,
                "ported contrast mask delta was refused");
        require(params(session.get()) != before, "contrast mask delta did not change params");
        render(engine.get(), session.get());
        {
            spk_session* fresh = spk_open(
                engine.get(), &input,
                "{\"contrast_mask_active\":true,\"contrast_mask_highlights\":1.25}", nullptr);
            require(bool(fresh), std::string("fresh mask session failed to open: ") +
                                     spk_last_error(engine.get()));
            const float* early = nullptr;
            uint32_t early_w = 0, early_h = 0;
            const spk_status early_status = spk_contrast_mask_field(fresh, "full", &early, &early_w, &early_h);
            spk_session_release(fresh);
            require(early_status == SPK_ERR_USER && !early && early_w == 0 && early_h == 0 &&
                        std::string(spk_last_error(engine.get())).find("render it first") != std::string::npos,
                    "the field must refuse a tier that has no negative yet");
        }
        {
            const float* field = nullptr;
            uint32_t field_w = 0, field_h = 0;
            const spk_status field_status =
                spk_contrast_mask_field(session.get(), "full", &field, &field_w, &field_h);
            require(field_status == SPK_OK && field && field_w > 0 && field_h > 0,
                    std::string("enabled contrast mask must expose its field: ") +
                        spk_last_error(engine.get()));
            const std::vector<float> first(field, field + size_t(field_w) * field_h);
            for (uint32_t i = 0; i < field_w * field_h; ++i)
                require(std::isfinite(first[i]) && std::fabs(first[i]) < 6.0,
                        "the field's deltas must be the bounded stops");
            // The grid base is extracted on the host through the worker pool
            // (line_pool.hpp): which worker takes which line, or how many
            // workers there are, may change every run, but no bit of the field
            // may. Eight re-extractions of the same session's negative must be
            // byte-identical to the first.
            for (int run = 0; run < 8; ++run) {
                const float* again = nullptr;
                uint32_t again_w = 0, again_h = 0;
                require(spk_contrast_mask_field(session.get(), "full", &again, &again_w, &again_h) ==
                                SPK_OK &&
                            again && again_w == field_w && again_h == field_h &&
                            std::memcmp(again, first.data(), first.size() * sizeof(float)) == 0,
                        "re-extracted field must be bit-identical across worker interleavings");
            }
        }
        require(spk_set_params(session.get(),
                               "{\"contrast_mask_active\":false,\"contrast_mask_highlights\":0.0,"
                               "\"contrast_mask_shadows\":0.0,\"print_exposure\":1.0}",
                               nullptr) == SPK_OK,
                "contrast mask could not be reverted");
        require(params(session.get()) == before, "mask parameter round trip changed other params");
        render(engine.get(), session.get());

        const float* mask = nullptr;
        uint32_t width = 0, height = 0;
        require(spk_contrast_mask_field(session.get(), "full", &mask, &width, &height) == SPK_OK &&
                !mask && width == 0 && height == 0, "disabled contrast mask must have an empty field");

        // The slide's HDR scan is an ordinary engine parameter now (the
        // 2026-11-06 port): the headroom applies to a positive film scanned,
        // the params reply carries the solved model, and the render runs the
        // per-pixel expansion (`spk_scan_hdr_expand` on Vulkan). A negative
        // film ignores the field harmlessly -- the engine's own rule is
        // `scan_film` on a positive film, and neither holds here.
        require(spk_set_params(session.get(),
                               "{\"scan_hdr_headroom\":4.0,\"print_exposure\":1.25}",
                               nullptr) == SPK_OK,
                "scan_hdr headroom was refused on a negative film");
        render(engine.get(), session.get());
        require(spk_set_params(session.get(),
                               "{\"scan_hdr_headroom\":0.0,\"print_exposure\":1.0}",
                               nullptr) == SPK_OK,
                "scan_hdr headroom could not be reverted");
        require(params(session.get()) == before, "scan_hdr round trip changed other params");
        render(engine.get(), session.get());

        {
            const char* slide_delta =
                "{\"film_stock\":\"fujifilm_provia_100f\",\"scan_film\":true,"
                "\"scan_hdr_headroom\":4.0}";
            spk_session* slide = spk_open(engine.get(), &input, slide_delta, nullptr);
            require(bool(slide), std::string("slide session with the HDR scan failed to open: ") +
                                     spk_last_error(engine.get()));
            // The solved model travels in the progress reply's additive block
            // (what the pipeline was built with): the headroom as requested,
            // the stretch flag off, and the per-layer lift the host solve
            // produced. The params reply only echoes the raw fields.
            const auto progress_json = [&](spk_session* s) {
                char* json = nullptr;
                require(spk_progress(s, nullptr, &json) == SPK_OK && json,
                        "progress readback failed");
                std::string result(json);
                spk_string_free(json);
                return result;
            };
            const std::string slide_progress = progress_json(slide);
            require(slide_progress.find("\"scan_hdr\"") != std::string::npos &&
                        slide_progress.find("\"headroom\":4") != std::string::npos &&
                        slide_progress.find("\"stretch\":false") != std::string::npos &&
                        slide_progress.find("\"lift_density\"") != std::string::npos,
                    "the HDR scan's solved model is missing from the progress reply");
            render(engine.get(), slide);
            // The stretch variant solves its own headroom (the clear base at
            // the page's white) and wins over the manual one; both variants
            // revert through the ordinary channel, and the inactive scan's
            // readback block disappears with it.
            require(spk_set_params(slide, "{\"scan_hdr_headroom\":0.0}", nullptr) == SPK_OK,
                    "the slide's headroom could not be reverted");
            require(progress_json(slide).find("\"scan_hdr\"") == std::string::npos,
                    "the inactive scan still reports a solved model");
            require(spk_set_params(slide, "{\"scan_hdr_stretch\":true}", nullptr) == SPK_OK,
                    "the stretch variant was refused");
            const std::string stretched = progress_json(slide);
            require(stretched.find("\"scan_hdr\"") != std::string::npos &&
                        stretched.find("\"stretch\":true") != std::string::npos,
                    "the stretch variant's model is missing from the progress reply");
            render(engine.get(), slide);
            require(spk_set_params(slide, "{\"scan_hdr_stretch\":false}", nullptr) == SPK_OK,
                    "the stretch variant could not be reverted");
            require(progress_json(slide).find("\"scan_hdr\"") == std::string::npos,
                    "the reverted scan still reports a solved model");
            render(engine.get(), slide);
            spk_session_release(slide);
        }

        // Latitude analysis uses the existing pipeline; only applying its
        // mapping is unported. This also exercises probe_medium's readback.
        char* latitude_json = nullptr;
        const spk_status latitude_status = spk_scene_latitude(session.get(), nullptr, &latitude_json);
        const std::string latitude_error = spk_last_error(engine.get());
        const bool latitude_valid = latitude_json &&
            std::string(latitude_json).find("\"medium\"") != std::string::npos &&
            std::string(latitude_json).find("\"scene\"") != std::string::npos;
        spk_string_free(latitude_json);
        require(latitude_status == SPK_OK && latitude_valid, "latitude analysis failed: " + latitude_error);
        require(params(session.get()) == before, "latitude analysis mutated the session");
        render(engine.get(), session.get());
        std::cout << "Windows feature boundaries passed (DI + scene latitude + contrast mask + overscan/"
                     "date imprint applied and reverted, overscan geometry, unknown format refused, "
                     "mask field round trip, "
                     "slide refused, mask off, HDR scan applied and reverted on a slide, "
                     "latitude analysis)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
