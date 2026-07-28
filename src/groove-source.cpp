// Groovalizer for OBS — audio-reactive visualizer source.
//
// Registers one input source ("groove_visualizer") that renders a GrooveCore
// scene driven by any OBS audio source the user picks. Everything runs on the
// GPU through libobs' graphics abstraction, so it works identically on the D3D11
// and OpenGL backends and costs a fraction of a browser source.

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>
#include <util/platform.h>
#include <util/threading.h>
#include <media-io/audio-io.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "groove_analyzer.hpp"
#ifdef GROOVE_MILKDROP
#include "groove-backend.hpp"
// No GL headers here any more. Getting projectM's OpenGL output into an OBS
// texture is platform-specific — a bound framebuffer on macOS/Linux, a shared
// D3D11 texture or a readback on Windows — and all of that lives behind
// GLBridge. This file used to include <OpenGL/gl3.h> directly, which meant the
// MilkDrop path could not even compile off macOS.
#include "groove-gl-bridge.hpp"
#include <algorithm>
#include <filesystem>
#include <memory>
namespace groove { std::unique_ptr<VisualBackend> make_milk_backend(); }

/// List .milk files under a directory, recursively.
///
/// Deliberately independent of the backend. Populating the preset dropdown is
/// pure filesystem work, and tying it to the projectM instance meant the list
/// could only fill in one frame *after* the settings changed — so the folder
/// had to be picked, the dialog closed, and reopened before anything appeared.
static std::vector<std::pair<std::string, std::string>> scan_milk_dir(const char* dir) {
    std::vector<std::pair<std::string, std::string>> out;   // {label, path}
    if (!dir || !*dir) return out;
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (auto it = fs::recursive_directory_iterator(
             dir, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        auto ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(::tolower(c)); });
        if (ext != ".milk") continue;
        out.emplace_back(it->path().stem().string(), it->path().string());
        if (out.size() >= 6000) break;   // a pathological tree should not hang the UI
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// Refill the preset dropdown from whatever folder is currently selected.
static void refresh_milk_list(obs_properties_t* props, obs_data_t* settings) {
    obs_property_t* list = obs_properties_get(props, "milk_preset");
    if (!list) return;
    obs_property_list_clear(list);
    auto found = scan_milk_dir(obs_data_get_string(settings, "milk_dir"));
    if (found.empty()) {
        obs_property_list_add_string(list, obs_module_text("MilkPreset.Empty"), "");
        return;
    }
    for (const auto& [label, path] : found) {
        obs_property_list_add_string(list, label.c_str(), path.c_str());
    }
    // Nothing chosen yet: pick the first so something renders immediately.
    const char* cur = obs_data_get_string(settings, "milk_preset");
    if (!cur || !*cur) obs_data_set_string(settings, "milk_preset", found[0].second.c_str());
}

/// Show only the controls that apply to the chosen visual source.
static bool on_visual_source_changed(void*, obs_properties_t* props,
                                     obs_property_t*, obs_data_t* settings) {
    const bool milk = strcmp(obs_data_get_string(settings, "visual_source"), "milk") == 0;
    for (const char* id : {"milk_dir", "milk_preset"}) {
        if (obs_property_t* pr = obs_properties_get(props, id)) {
            obs_property_set_visible(pr, milk);
        }
    }
    for (const char* id : {"scene", "theme", "intensity"}) {
        if (obs_property_t* pr = obs_properties_get(props, id)) {
            obs_property_set_visible(pr, !milk);
        }
    }
    if (milk) refresh_milk_list(props, settings);
    return true;   // redraw the properties UI
}

static bool on_milk_dir_changed(void*, obs_properties_t* props,
                                obs_property_t*, obs_data_t* settings) {
    refresh_milk_list(props, settings);
    return true;
}
#endif

// ── Scene and theme catalogs (mirrors shared/groove-spec/scenes.json) ────────

struct SceneDef {
    const char* id;
    const char* name;
};

static const SceneDef kScenes[] = {
    // <generated:scenes>
    {"morphogen", "Morphogen"},
    {"slow_bloom", "Slow Bloom"},
    {"nacre", "Nacre"},
    {"lichen_creep", "Lichen Creep"},
    {"apollonian_drift", "Apollonian Drift"},
    {"julia_tide", "Julia Tide"},
    {"caustic_drift", "Caustic Drift"},
    {"droste_helix", "Droste Helix"},
    {"droste_shell", "Droste Shell"},
    {"gasket_veil", "Gasket Veil"},
    {"mandel_shore", "Mandelbrot Shore"},
    {"newton_basin", "Newton Basin"},
    {"newton_flow", "Newton Flow"},
    {"orchid_fold", "Orchid Fold"},
    {"poincare_lace", "Poincaré Lace"},
    {"quasi_lattice", "Quasi Lattice"},
    {"quasi_mandala", "Quasi Mandala"},
    {"sierpin_fold", "Sierpinski Fold"},
    {"smoke_veil", "Smoke Veil"},
    {"suminagashi", "Suminagashi"},
    {"kleinian_veil", "Kleinian Veil"},
    {"lyapunov_reef", "Lyapunov Reef"},
// </generated:scenes>
};
static constexpr int kSceneCount = sizeof(kScenes) / sizeof(kScenes[0]);

struct ThemeDef {
    const char* id;
    const char* name;
    // Up to five stops. Three was the ceiling on how rich a theme could look —
    // the ramp is a two-colour gradient plus a return leg. Unset stops repeat
    // the last real colour so the shader always reads five slots.
    uint32_t a, b, c;  // 0xRRGGBB
    uint32_t d = 0xFFFFFFFF;   // sentinel: "same as c"
    uint32_t e = 0xFFFFFFFF;   // sentinel: "same as d"
    int stops = 3;
};

static const ThemeDef kThemes[] = {
    {"groove",      "Groove",      0xa855f7, 0x22d3ee, 0xf0abfc},
    {"ember",       "Ember",       0xf97316, 0xdc2626, 0xfbbf24},
    {"abyss",       "Abyss",       0x0ea5e9, 0x1e1b4b, 0x67e8f9},
    {"chlorophyll", "Chlorophyll", 0x10b981, 0x065f46, 0xa3e635},
    {"vaporwave",   "Vaporwave",   0xff71ce, 0x01cdfe, 0x05ffa1},
    {"mono",        "Mono",        0xffffff, 0x9ca3af, 0x374151},
    {"sunset",      "Sunset",      0xfb7185, 0xf59e0b, 0x7c3aed},
    {"ultraviolet", "Ultraviolet", 0x7c3aed, 0xc026d3, 0x2563eb},
    {"gold",        "Gold Leaf",   0xfcd34d, 0xb45309, 0xfef3c7},
    {"toxic",       "Toxic",       0x84cc16, 0xfacc15, 0x065f46},
    {"ice",         "Ice",         0xe0f2fe, 0x38bdf8, 0x1e40af},
    {"bloodmoon",   "Blood Moon",  0xef4444, 0x450a0a, 0xfca5a5},
    {"custom",      "Custom",      0xa855f7, 0x22d3ee, 0xf0abfc},
    {"nocturne",   "Nocturne",   0x141a3a, 0x1f6f8b, 0xe8b04b, 0xd96c6c, 0x6b4ea8, 5},
    {"kelp",       "Kelp",       0x06302b, 0x2f7a5c, 0xa8c256, 0xe8d9a0, 0x8c4a2f, 5},
    {"opal",       "Opal",       0xbfe3f2, 0x9fe6c8, 0xd4c2f0, 0xf7c9a8, 0xf2e3a8, 5},
    {"ashfall",    "Ashfall",    0x1c1b1e, 0x4a4a55, 0xd9622b, 0xe8e0d2, 0x7a1f1f, 5},
    {"borealis",   "Borealis",   0x081633, 0x0f8a6a, 0x3fd8d8, 0xc44fd0, 0x5b2ea8, 5},
    {"terracotta", "Terracotta", 0x8c4326, 0xc97b4a, 0xf0dcc0, 0x7f8f5a, 0x40241a, 5},
    {"spectra",    "Spectra",    0xe03c31, 0xf2b134, 0x4caf50, 0x1e9de3, 0x7b3fa0, 5},
    {"driftwood",  "Driftwood",  0x2b2a26, 0x6e6553, 0xb8a888, 0xdfd6c0, 0x3f5148, 5},
};
static constexpr int kThemeCount = sizeof(kThemes) / sizeof(kThemes[0]);

// OBS colour pickers hand back 0xAABBGGRR; scenes want linear-ish RGB.
static void unpack_obs_color(uint32_t abgr, vec3* out) {
    const float r = float(abgr & 0xFF) / 255.0f;
    const float g = float((abgr >> 8) & 0xFF) / 255.0f;
    const float b = float((abgr >> 16) & 0xFF) / 255.0f;
    // sRGB -> linear. Scenes composite in linear space (SPEC.md §7).
    auto lin = [](float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); };
    vec3_set(out, lin(r), lin(g), lin(b));
}

static void unpack_hex_color(uint32_t rgb, vec3* out) {
    const float r = float((rgb >> 16) & 0xFF) / 255.0f;
    const float g = float((rgb >> 8) & 0xFF) / 255.0f;
    const float b = float(rgb & 0xFF) / 255.0f;
    auto lin = [](float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); };
    vec3_set(out, lin(r), lin(g), lin(b));
}

// ── Source state ─────────────────────────────────────────────────────────────

struct groove_source {
    obs_source_t* self = nullptr;

    // Settings
    std::string scene_id = "slow_bloom";
    std::string audio_source_name;
    uint32_t cx = 1920, cy = 1080;
    float intensity = 0.5f;
    float speed = 1.0f;
    float reactivity = 1.0f;
    float beat_sensitivity = 1.0f;
    float freq_lo = 0.0f, freq_hi = 1.0f;
    float hue_shift = 0.0f, saturation = 1.0f, brightness = 1.0f, contrast = 1.0f;
    float trail = 0.35f, bloom = 0.6f, vignette = 0.7f;
    // Overlay controls. transparent_bg keys the black a scene sits on out to
    // real alpha, which is what makes this usable as a stream overlay rather
    // than a full-frame background.
    float opacity = 1.0f;
    bool transparent_bg = false;
    float alpha_gain = 2.0f;
    float alpha_floor = 0.04f;
    float exposure = 1.0f;

#ifdef GROOVE_MILKDROP
    // MilkDrop backend. Only the *engine* is built in; presets come from a
    // folder the user chooses, which is both the licence-clean design and what
    // people with an existing .milk library actually want.
    std::unique_ptr<groove::VisualBackend> milk;
    std::unique_ptr<groove::GLBridge> bridge;
    bool bridge_warned = false;
    bool use_milk = false;
    std::string milk_dir;
    std::string milk_preset;
    bool milk_dirty = false;
#endif
    int octaves = 5;
    bool bloom_enabled = true;
    vec3 colA{}, colB{}, colC{}, colD{}, colE{};
    float colCount = 3.0f;

    // Audio plumbing. The capture callback runs on the audio thread; the render
    // path runs on the graphics thread, so every touch of the analyzer is locked.
    obs_weak_source_t* audio_weak = nullptr;
    std::mutex audio_mutex;
    groove::Analyzer analyzer;
    groove::Frame frame;
    uint64_t last_render_ns = 0;
    uint64_t last_audio_ns = 0;

    // GPU resources
    gs_effect_t* scene_effect = nullptr;
    std::string scene_effect_id;   // which scene scene_effect was built from
    gs_effect_t* post_effect = nullptr;
    gs_texture_t* tex_spectrum = nullptr;
    gs_texture_t* tex_wave = nullptr;
    gs_texture_t* tex_black = nullptr;
    // Ping-ponged so a scene can sample its own previous frame via feedback().
    // Without it a scene is a pure function of (uv, time, audio): it can
    // oscillate but nothing can persist, accumulate or emerge.
    gs_texrender_t* tr_scene_pp[2] = {nullptr, nullptr};
    int scene_index = 0;
    bool scene_primed = false;
    gs_texrender_t* tr_history[2] = {nullptr, nullptr};
    gs_texrender_t* tr_bloom[2] = {nullptr, nullptr};
    int history_index = 0;
    bool history_primed = false;
};

// ── Audio capture ────────────────────────────────────────────────────────────

static void audio_capture_cb(void* param, obs_source_t* /*src*/,
                             const struct audio_data* data, bool muted) {
    auto* gs = static_cast<groove_source*>(param);
    if (!data || data->frames == 0) return;

    std::lock_guard<std::mutex> lock(gs->audio_mutex);
    gs->last_audio_ns = os_gettime_ns();

    if (muted) {
        gs->analyzer.pushSilence();
        return;
    }

    const audio_t* audio = obs_get_audio();
    const uint32_t channels = audio ? audio_output_get_channels(audio) : 2;
    const uint32_t rate = audio ? audio_output_get_sample_rate(audio) : 48000;
    gs->analyzer.setSampleRate(float(rate));

    const float* planes[MAX_AV_PLANES] = {nullptr};
    uint32_t used = 0;
    for (uint32_t c = 0; c < channels && c < MAX_AV_PLANES; c++) {
        planes[used++] = reinterpret_cast<const float*>(data->data[c]);
    }
    if (used == 0) return;

    gs->analyzer.pushAudio(planes, int(used), int(data->frames));

#ifdef GROOVE_MILKDROP
    // projectM runs its own analysis and wants PCM, not our features. Mono
    // downmix of the first plane is enough — it only needs the envelope.
    if (gs->milk && planes[0]) {
        gs->milk->push_audio(planes[0], size_t(data->frames));
    }
#endif
}

static void detach_audio(groove_source* gs) {
    if (!gs->audio_weak) return;
    obs_source_t* src = obs_weak_source_get_source(gs->audio_weak);
    if (src) {
        obs_source_remove_audio_capture_callback(src, audio_capture_cb, gs);
        obs_source_release(src);
    }
    obs_weak_source_release(gs->audio_weak);
    gs->audio_weak = nullptr;
}

static void attach_audio(groove_source* gs, const char* name) {
    detach_audio(gs);
    if (!name || !*name) return;

    obs_source_t* src = obs_get_source_by_name(name);
    if (!src) {
        blog(LOG_WARNING, "[groovalizer] audio source '%s' not found (yet)", name);
        return;
    }
    obs_source_add_audio_capture_callback(src, audio_capture_cb, gs);
    gs->audio_weak = obs_source_get_weak_source(src);
    obs_source_release(src);
    blog(LOG_INFO, "[groovalizer] listening to '%s'", name);
}

// ── GPU helpers ──────────────────────────────────────────────────────────────

static void load_scene_effect(groove_source* gs) {
    if (gs->scene_effect_id == gs->scene_id && gs->scene_effect) return;

    std::string rel = "shaders/" + gs->scene_id + ".effect";
    char* path = obs_module_file(rel.c_str());
    if (!path) {
        blog(LOG_ERROR, "[groovalizer] missing shader %s", rel.c_str());
        return;
    }

    char* errors = nullptr;
    gs_effect_t* fx = gs_effect_create_from_file(path, &errors);
    bfree(path);

    if (!fx) {
        blog(LOG_ERROR, "[groovalizer] failed to compile %s: %s", rel.c_str(),
             errors ? errors : "unknown");
        bfree(errors);
        return;
    }
    bfree(errors);

    if (gs->scene_effect) gs_effect_destroy(gs->scene_effect);
    gs->scene_effect = fx;
    gs->scene_effect_id = gs->scene_id;
}

static void ensure_gpu(groove_source* gs) {
    if (!gs->tex_spectrum) {
        gs->tex_spectrum = gs_texture_create(groove::kSpectrumBins, 1, GS_R32F, 1, nullptr, GS_DYNAMIC);
        gs->tex_wave = gs_texture_create(groove::kWaveformBins, 1, GS_R32F, 1, nullptr, GS_DYNAMIC);
    }
    if (!gs->tex_black) {
        // 1x1 black stands in for the bloom texture when bloom is disabled, so the
        // composite shader never samples a null texture.
        static const uint8_t black[4] = {0, 0, 0, 255};
        const uint8_t* data = black;
        gs->tex_black = gs_texture_create(1, 1, GS_RGBA, 1, &data, 0);
    }
    if (!gs->tr_scene_pp[0]) {
        // 16F intermediates: scenes intentionally return values above 1.0 so the
        // bright-pass has something to find. An 8-bit target would clip them away.
        gs->tr_scene_pp[0] = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
        gs->tr_scene_pp[1] = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
        gs->tr_history[0] = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
        gs->tr_history[1] = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
        gs->tr_bloom[0] = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
        gs->tr_bloom[1] = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
    }
    if (!gs->post_effect) {
        char* path = obs_module_file("shaders/post.effect");
        if (path) {
            char* errors = nullptr;
            gs->post_effect = gs_effect_create_from_file(path, &errors);
            if (!gs->post_effect) {
                blog(LOG_ERROR, "[groovalizer] post.effect failed: %s", errors ? errors : "?");
            }
            bfree(errors);
            bfree(path);
        }
    }
    load_scene_effect(gs);
}

static void set_param_f(gs_effect_t* fx, const char* name, float v) {
    gs_eparam_t* p = gs_effect_get_param_by_name(fx, name);
    if (p) gs_effect_set_float(p, v);
}

static void set_param_i(gs_effect_t* fx, const char* name, int v) {
    gs_eparam_t* p = gs_effect_get_param_by_name(fx, name);
    if (p) gs_effect_set_int(p, v);
}

static void set_param_v2(gs_effect_t* fx, const char* name, float x, float y) {
    gs_eparam_t* p = gs_effect_get_param_by_name(fx, name);
    if (p) { vec2 v; vec2_set(&v, x, y); gs_effect_set_vec2(p, &v); }
}

static void set_param_v3(gs_effect_t* fx, const char* name, const vec3* v) {
    gs_eparam_t* p = gs_effect_get_param_by_name(fx, name);
    if (p) gs_effect_set_vec3(p, v);
}

static void set_param_tex(gs_effect_t* fx, const char* name, gs_texture_t* t) {
    gs_eparam_t* p = gs_effect_get_param_by_name(fx, name);
    if (p) gs_effect_set_texture(p, t);
}

/** Render `fn` into a texrender at cx x cy and return its texture. */
template <typename Fn>
static gs_texture_t* render_to(gs_texrender_t* tr, uint32_t cx, uint32_t cy, Fn&& fn) {
    gs_texrender_reset(tr);
    if (!gs_texrender_begin(tr, cx, cy)) return nullptr;

    vec4 clear;
    vec4_zero(&clear);
    gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
    gs_ortho(0.0f, float(cx), 0.0f, float(cy), -100.0f, 100.0f);
    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

    fn();

    gs_blend_state_pop();
    gs_texrender_end(tr);
    return gs_texrender_get_texture(tr);
}

// ── Source callbacks ─────────────────────────────────────────────────────────

static const char* groove_get_name(void*) {
    return obs_module_text("Groovalizer");
}

static void groove_update(void* data, obs_data_t* s) {
    auto* gs = static_cast<groove_source*>(data);

    gs->scene_id = obs_data_get_string(s, "scene");
    if (gs->scene_id.empty()) gs->scene_id = "slow_bloom";

    gs->cx = std::max<uint32_t>(16, uint32_t(obs_data_get_int(s, "width")));
    gs->cy = std::max<uint32_t>(16, uint32_t(obs_data_get_int(s, "height")));

    gs->intensity = float(obs_data_get_double(s, "intensity"));
    gs->speed = float(obs_data_get_double(s, "speed"));
    gs->reactivity = float(obs_data_get_double(s, "reactivity"));
    gs->beat_sensitivity = float(obs_data_get_double(s, "beat_sensitivity"));
    gs->freq_lo = float(obs_data_get_double(s, "freq_lo")) / 100.0f;
    gs->freq_hi = float(obs_data_get_double(s, "freq_hi")) / 100.0f;
    // A collapsed or inverted range would divide the visuals by zero energy.
    if (gs->freq_hi < gs->freq_lo + 0.05f) gs->freq_hi = gs->freq_lo + 0.05f;

    gs->hue_shift = float(obs_data_get_double(s, "hue_shift"));
    gs->saturation = float(obs_data_get_double(s, "saturation"));
    gs->brightness = float(obs_data_get_double(s, "brightness"));
    gs->contrast = float(obs_data_get_double(s, "contrast"));
    gs->trail = float(obs_data_get_double(s, "trail"));
    gs->bloom = float(obs_data_get_double(s, "bloom"));
    gs->vignette = float(obs_data_get_double(s, "vignette"));
    gs->opacity = float(obs_data_get_double(s, "opacity") / 100.0);
    gs->transparent_bg = obs_data_get_bool(s, "transparent_bg");
    gs->alpha_gain = float(obs_data_get_double(s, "alpha_gain"));
    gs->alpha_floor = float(obs_data_get_double(s, "alpha_floor"));
    gs->exposure = float(obs_data_get_double(s, "exposure"));

#ifdef GROOVE_MILKDROP
    {
        const char* vsrc = obs_data_get_string(s, "visual_source");
        const bool want = vsrc && strcmp(vsrc, "milk") == 0;
        const char* dir = obs_data_get_string(s, "milk_dir");
        const char* preset = obs_data_get_string(s, "milk_preset");
        if (want != gs->use_milk) { gs->use_milk = want; gs->scene_primed = false; }
        if (dir && gs->milk_dir != dir)   { gs->milk_dir = dir;   gs->milk_dirty = true; }
        if (preset && gs->milk_preset != preset) {
            gs->milk_preset = preset;
            gs->milk_dirty = true;
        }
    }
#endif
    gs->bloom_enabled = obs_data_get_bool(s, "bloom_enabled");

    const int quality = int(obs_data_get_int(s, "quality"));
    gs->octaves = std::clamp(quality, 3, 6);

    // Theme.
    const char* theme_id = obs_data_get_string(s, "theme");
    bool custom = theme_id && strcmp(theme_id, "custom") == 0;
    if (custom) {
        unpack_obs_color(uint32_t(obs_data_get_int(s, "color_a")), &gs->colA);
        unpack_obs_color(uint32_t(obs_data_get_int(s, "color_b")), &gs->colB);
        unpack_obs_color(uint32_t(obs_data_get_int(s, "color_c")), &gs->colC);
        // The custom-colour path exposes three pickers, so it stays three-stop.
        gs->colD = gs->colC;
        gs->colE = gs->colC;
        gs->colCount = 3.0f;
    } else {
        const ThemeDef* t = &kThemes[0];
        for (int i = 0; i < kThemeCount; i++) {
            if (theme_id && strcmp(kThemes[i].id, theme_id) == 0) { t = &kThemes[i]; break; }
        }
        unpack_hex_color(t->a, &gs->colA);
        unpack_hex_color(t->b, &gs->colB);
        unpack_hex_color(t->c, &gs->colC);
        unpack_hex_color(t->d == 0xFFFFFFFF ? t->c : t->d, &gs->colD);
        unpack_hex_color(t->e == 0xFFFFFFFF ? (t->d == 0xFFFFFFFF ? t->c : t->d) : t->e,
                         &gs->colE);
        gs->colCount = float(t->stops);
    }

    {
        std::lock_guard<std::mutex> lock(gs->audio_mutex);
        gs->analyzer.reactivity = gs->reactivity;
        gs->analyzer.speed = gs->speed;
        gs->analyzer.autoGain = obs_data_get_bool(s, "auto_gain");
        gs->analyzer.beatDetector().sensitivity = gs->beat_sensitivity;
    }

    const char* audio_name = obs_data_get_string(s, "audio_source");
    if (gs->audio_source_name != (audio_name ? audio_name : "")) {
        gs->audio_source_name = audio_name ? audio_name : "";
        attach_audio(gs, audio_name);
    }

    // Force the scene effect to reload if the scene changed.
    if (gs->scene_effect_id != gs->scene_id) {
        obs_enter_graphics();
        load_scene_effect(gs);
        obs_leave_graphics();
    }
}

static void* groove_create(obs_data_t* settings, obs_source_t* source) {
    auto* gs = new groove_source();
    gs->self = source;
    unpack_hex_color(kThemes[0].a, &gs->colA);
    unpack_hex_color(kThemes[0].b, &gs->colB);
    unpack_hex_color(kThemes[0].c, &gs->colC);

    obs_enter_graphics();
    ensure_gpu(gs);
    obs_leave_graphics();

    groove_update(gs, settings);
    gs->last_render_ns = os_gettime_ns();
    return gs;
}

static void groove_destroy(void* data) {
    auto* gs = static_cast<groove_source*>(data);
    detach_audio(gs);

    obs_enter_graphics();
    if (gs->scene_effect) gs_effect_destroy(gs->scene_effect);
    if (gs->post_effect) gs_effect_destroy(gs->post_effect);
    if (gs->tex_spectrum) gs_texture_destroy(gs->tex_spectrum);
    if (gs->tex_wave) gs_texture_destroy(gs->tex_wave);
    if (gs->tex_black) gs_texture_destroy(gs->tex_black);
    for (auto*& tr : gs->tr_scene_pp) if (tr) gs_texrender_destroy(tr);
    for (auto*& tr : gs->tr_history) if (tr) gs_texrender_destroy(tr);
    for (auto*& tr : gs->tr_bloom) if (tr) gs_texrender_destroy(tr);
    obs_leave_graphics();

    delete gs;
}

static uint32_t groove_get_width(void* data) { return static_cast<groove_source*>(data)->cx; }
static uint32_t groove_get_height(void* data) { return static_cast<groove_source*>(data)->cy; }

static void groove_video_render(void* data, gs_effect_t*) {
    auto* gs = static_cast<groove_source*>(data);
    ensure_gpu(gs);
    if (!gs->post_effect) return;

#ifdef GROOVE_MILKDROP
    // projectM resolves GL entry points on creation, so it must be constructed
    // on the graphics thread with a context current — not in groove_create.
    if (gs->use_milk && !gs->milk) {
        gs->milk = groove::make_milk_backend();
        gs->milk_dirty = true;
    }
    if (gs->milk && gs->milk_dirty) {
        gs->milk_dirty = false;
        gs->milk->set_source_directory(gs->milk_dir);
        if (!gs->milk_preset.empty()) gs->milk->load(gs->milk_preset);
        const std::string why = gs->milk->status();
        if (!why.empty()) blog(LOG_WARNING, "[groovalizer] %s", why.c_str());
    }
#endif
    if (!gs->use_milk && !gs->scene_effect) return;

    const uint32_t cx = gs->cx, cy = gs->cy;

    // ── Analysis ─────────────────────────────────────────────────────────────
    const uint64_t now = os_gettime_ns();
    float dt = float(double(now - gs->last_render_ns) / 1.0e9);
    gs->last_render_ns = now;
    if (dt <= 0.0f || dt > 0.25f) dt = 1.0f / 60.0f;

    {
        std::lock_guard<std::mutex> lock(gs->audio_mutex);
        gs->analyzer.analyze(gs->frame, dt);
    }
    const groove::Frame& f = gs->frame;

    // ── Upload audio textures ────────────────────────────────────────────────
    gs_texture_set_image(gs->tex_spectrum,
                         reinterpret_cast<const uint8_t*>(f.spectrum.data()),
                         groove::kSpectrumBins * sizeof(float), false);

    // The wave texture is R32F in 0..1; the shader maps it back to -1..1.
    static thread_local std::array<float, groove::kWaveformBins> wave01;
    for (int i = 0; i < groove::kWaveformBins; i++) {
        wave01[i] = std::clamp(f.waveform[i] * 0.5f + 0.5f, 0.0f, 1.0f);
    }
    gs_texture_set_image(gs->tex_wave, reinterpret_cast<const uint8_t*>(wave01.data()),
                         groove::kWaveformBins * sizeof(float), false);

    // ── Pass 1: scene ────────────────────────────────────────────────────────
    gs_effect_t* fx = gs->scene_effect;
    // Previous scene frame, or black until one exists — a scene that feeds back
    // must start from nothing rather than from whatever the last scene left.
    gs_texture_t* scene_prev =
        gs->scene_primed ? gs_texrender_get_texture(gs->tr_scene_pp[1 - gs->scene_index])
                         : gs->tex_black;
    if (!scene_prev) scene_prev = gs->tex_black;

#ifdef GROOVE_MILKDROP
    if (gs->use_milk && gs->milk) {
        if (!gs->bridge) gs->bridge = groove::make_gl_bridge();

        // The bridge owns the target and the platform strategy. On macOS and
        // Linux `fbo` is an OBS framebuffer projectM draws straight into; on
        // Windows it belongs to a private WGL context backed by either a
        // shared D3D11 texture or a readback buffer.
        const uint32_t fbo = gs->bridge->begin(cx, cy);
        if (!fbo) {
            // Once, not sixty times a second. The bridge does not retry after
            // a hard failure, so this reports the reason and then goes quiet.
            if (!gs->bridge_warned) {
                gs->bridge_warned = true;
                const std::string why = gs->bridge->status();
                blog(LOG_WARNING, "[groovalizer] MilkDrop unavailable: %s",
                     why.empty() ? "the render target could not be prepared" : why.c_str());
            }
            return;
        }

        groove::RenderRequest req;
        req.frame = &f;
        req.width = cx;
        req.height = cy;
        req.dt = dt;
        req.intensity = gs->intensity;
        req.speed = gs->speed;
        req.beat_sensitivity = gs->beat_sensitivity;
        gs->milk->render(req, fbo);

        gs_texture_t* milk_tex = gs->bridge->end();
        if (!milk_tex) return;

        // `pfx` proper is declared further down for the scene path; this branch
        // returns before reaching it, so bind the post effect here.
        gs_effect_t* pfx = gs->post_effect;
        // Straight to Present: projectM output is already display-referred, so
        // the bloom and trail stages would double up on what the preset does.
        set_param_tex(pfx, "gScene", milk_tex);
        set_param_f(pfx, "pHueShift", gs->hue_shift);
        set_param_f(pfx, "pSaturation", gs->saturation);
        set_param_f(pfx, "pBrightness", gs->brightness);
        set_param_f(pfx, "pContrast", gs->contrast);
        set_param_f(pfx, "pVignette", gs->vignette);
        set_param_f(pfx, "pOpacity", gs->opacity);
        set_param_f(pfx, "pAlphaMode", gs->transparent_bg ? 1.0f : 0.0f);
        set_param_f(pfx, "pAlphaGain", gs->alpha_gain);
        set_param_f(pfx, "pAlphaFloor", gs->alpha_floor);
        set_param_f(pfx, "pExposure", gs->exposure);
        gs_blend_state_push();
        gs_reset_blend_state();
        gs_enable_blending(gs->transparent_bg);
        if (gs->transparent_bg) gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
        while (gs_effect_loop(pfx, "Present")) gs_draw_sprite(nullptr, 0, cx, cy);
        gs_blend_state_pop();
        return;
    }
#endif

    gs_texture_t* scene_tex = render_to(gs->tr_scene_pp[gs->scene_index], cx, cy, [&] {
        set_param_tex(fx, "gFeedback", scene_prev);
        set_param_v2(fx, "gResolution", float(cx), float(cy));
        set_param_f(fx, "gTime", f.time);
        set_param_f(fx, "gAspect", float(cx) / float(std::max<uint32_t>(cy, 1)));
        set_param_f(fx, "gBass", f.bass);
        set_param_f(fx, "gMid", (f.lowMid + f.mid) * 0.5f);
        set_param_f(fx, "gTreble", (f.highMid + f.treble) * 0.5f);
        set_param_f(fx, "gLevel", f.level);
        set_param_f(fx, "gBeat", f.beat);
        set_param_f(fx, "gPulse", f.pulse);
        // Persistent state — integrated by the analyzer so OBS at 30 or 60 fps
        // behaves identically to the phone and TV builds.
        set_param_f(fx, "gSpin", f.spin);
        set_param_f(fx, "gTurn", f.turn);
        set_param_f(fx, "gWanderX", f.wanderX);
        set_param_f(fx, "gWanderY", f.wanderY);
        set_param_f(fx, "gSlow", f.slow);
        set_param_f(fx, "gIntensity", gs->intensity);
        set_param_f(fx, "gReactivity", gs->reactivity);
        set_param_f(fx, "gFreqLo", gs->freq_lo);
        set_param_f(fx, "gFreqHi", gs->freq_hi);
        set_param_v3(fx, "gColA", &gs->colA);
        set_param_v3(fx, "gColB", &gs->colB);
        set_param_v3(fx, "gColC", &gs->colC);
        set_param_v3(fx, "gColD", &gs->colD);
        set_param_v3(fx, "gColE", &gs->colE);
        set_param_f(fx, "gColCount", gs->colCount);
        set_param_i(fx, "gOctaves", gs->octaves);
        set_param_tex(fx, "gSpectrum", gs->tex_spectrum);
        set_param_tex(fx, "gWave", gs->tex_wave);

        while (gs_effect_loop(fx, "Draw")) gs_draw_sprite(nullptr, 0, cx, cy);
    });
    if (!scene_tex) return;

    // ── Pass 2: bloom (bright pass at quarter res, then separable blur) ──────
    gs_effect_t* pfx = gs->post_effect;
    gs_texture_t* bloom_tex = gs->tex_black;

    if (gs->bloom_enabled && gs->bloom > 0.001f) {
        const uint32_t bx = std::max<uint32_t>(cx / 4, 1), by = std::max<uint32_t>(cy / 4, 1);

        gs_texture_t* bright = render_to(gs->tr_bloom[0], bx, by, [&] {
            set_param_tex(pfx, "gScene", scene_tex);
            set_param_f(pfx, "pThreshold", 1.0f);
            while (gs_effect_loop(pfx, "Bright")) gs_draw_sprite(nullptr, 0, bx, by);
        });

        if (bright) {
            gs_texture_t* h = render_to(gs->tr_bloom[1], bx, by, [&] {
                set_param_tex(pfx, "gScene", bright);
                set_param_v2(pfx, "pTexel", 1.0f / float(bx), 1.0f / float(by));
                while (gs_effect_loop(pfx, "BlurH")) gs_draw_sprite(nullptr, 0, bx, by);
            });
            if (h) {
                bloom_tex = render_to(gs->tr_bloom[0], bx, by, [&] {
                    set_param_tex(pfx, "gScene", h);
                    set_param_v2(pfx, "pTexel", 1.0f / float(bx), 1.0f / float(by));
                    while (gs_effect_loop(pfx, "BlurV")) gs_draw_sprite(nullptr, 0, bx, by);
                });
                if (!bloom_tex) bloom_tex = gs->tex_black;
            }
        }
    }

    // ── Pass 3: composite into the history ping-pong ─────────────────────────
    const int cur = gs->history_index;
    const int prev = 1 - cur;
    gs_texture_t* prev_tex = gs->history_primed
                                 ? gs_texrender_get_texture(gs->tr_history[prev])
                                 : gs->tex_black;
    if (!prev_tex) prev_tex = gs->tex_black;

    gs_texture_t* out_tex = render_to(gs->tr_history[cur], cx, cy, [&] {
        set_param_tex(pfx, "gScene", scene_tex);
        set_param_tex(pfx, "gPrev", prev_tex);
        set_param_tex(pfx, "gBloom", bloom_tex);
        set_param_f(pfx, "pTrail", gs->trail);
        set_param_f(pfx, "pBloomAmount", gs->bloom_enabled ? gs->bloom : 0.0f);
        while (gs_effect_loop(pfx, "Composite")) gs_draw_sprite(nullptr, 0, cx, cy);
    });

    gs->history_index = prev;
    gs->history_primed = true;
    if (!out_tex) return;

    // ── Present — the only place grading is applied ──────────────────────────
    set_param_tex(pfx, "gScene", out_tex);
    // The grade parameters MUST be set here, not with the composite ones above.
    // libobs sizes an effect parameter per compiled technique, and these five
    // are used only by Present — setting them while Composite is the active
    // technique fails silently with "invalid size 0, expected 4", leaving them
    // at zero. That drives brightness and saturation to 0 and then contrast 0
    // pins every pixel to exactly 0.5, so the source renders flat mid-grey.
    set_param_f(pfx, "pHueShift", gs->hue_shift);
    set_param_f(pfx, "pSaturation", gs->saturation);
    set_param_f(pfx, "pBrightness", gs->brightness);
    set_param_f(pfx, "pContrast", gs->contrast);
    set_param_f(pfx, "pVignette", gs->vignette);
    set_param_f(pfx, "pOpacity", gs->opacity);
    set_param_f(pfx, "pAlphaMode", gs->transparent_bg ? 1.0f : 0.0f);
    set_param_f(pfx, "pAlphaGain", gs->alpha_gain);
    set_param_f(pfx, "pAlphaFloor", gs->alpha_floor);
    set_param_f(pfx, "pExposure", gs->exposure);

    // This source is OBS_SOURCE_CUSTOM_DRAW, so libobs does not configure blend
    // state for us — whatever the previous source left bound is what we would
    // inherit. Set it explicitly so the alpha produced above actually composites
    // against the sources underneath instead of overwriting them.
    gs_blend_state_push();
    gs_reset_blend_state();
    gs_enable_blending(gs->transparent_bg);
    if (gs->transparent_bg) {
        gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
    }
    while (gs_effect_loop(pfx, "Present")) gs_draw_sprite(nullptr, 0, cx, cy);
    gs_blend_state_pop();
}

// ── Properties ───────────────────────────────────────────────────────────────

static bool enum_audio_sources(void* param, obs_source_t* src) {
    auto* list = static_cast<obs_property_t*>(param);
    const uint32_t flags = obs_source_get_output_flags(src);
    if ((flags & OBS_SOURCE_AUDIO) != 0) {
        const char* name = obs_source_get_name(src);
        if (name && *name) obs_property_list_add_string(list, name, name);
    }
    return true;
}

static bool theme_modified(obs_properties_t* props, obs_property_t*, obs_data_t* s) {
    const char* theme = obs_data_get_string(s, "theme");
    const bool custom = theme && strcmp(theme, "custom") == 0;
    obs_property_set_visible(obs_properties_get(props, "color_a"), custom);
    obs_property_set_visible(obs_properties_get(props, "color_b"), custom);
    obs_property_set_visible(obs_properties_get(props, "color_c"), custom);
    return true;
}

static bool bloom_modified(obs_properties_t* props, obs_property_t*, obs_data_t* s) {
    const bool on = obs_data_get_bool(s, "bloom_enabled");
    obs_property_set_visible(obs_properties_get(props, "bloom"), on);
    return true;
}

static obs_properties_t* groove_get_properties(void* data) {
    obs_properties_t* p = obs_properties_create();
    // Button callbacks receive this; without it the overlay preset
    // button has no source to update.
    obs_properties_set_param(p, data, nullptr);

#ifdef GROOVE_MILKDROP
    // ── Visuals ──────────────────────────────────────────────────────────────
    // A single selector rather than a checkbox beside a second content list:
    // with both visible it is not obvious which one is actually driving the
    // picture. Choosing here hides whichever set does not apply.
    obs_property_t* vs = obs_properties_add_list(
        p, "visual_source", obs_module_text("VisualSource"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(vs, obs_module_text("VisualSource.Scenes"), "scenes");
    obs_property_list_add_string(vs, obs_module_text("VisualSource.Milk"), "milk");
    obs_property_set_modified_callback2(vs, on_visual_source_changed, data);

    obs_property_t* md = obs_properties_add_path(
        p, "milk_dir", obs_module_text("MilkDir"), OBS_PATH_DIRECTORY, nullptr, nullptr);
    obs_property_set_long_description(md, obs_module_text("MilkDir.Desc"));
    obs_property_set_modified_callback2(md, on_milk_dir_changed, data);

    obs_properties_add_list(p, "milk_preset", obs_module_text("MilkPreset"),
                            OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
#endif

    // ── Source ──
    obs_property_t* audio = obs_properties_add_list(
        p, "audio_source", obs_module_text("AudioSource"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(audio, obs_module_text("None"), "");
    obs_enum_sources(enum_audio_sources, audio);
    obs_property_set_long_description(audio, obs_module_text("AudioSource.Desc"));

    // ── Look ──
    obs_property_t* scene = obs_properties_add_list(
        p, "scene", obs_module_text("Scene"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    for (int i = 0; i < kSceneCount; i++) {
        obs_property_list_add_string(scene, kScenes[i].name, kScenes[i].id);
    }

    obs_property_t* theme = obs_properties_add_list(
        p, "theme", obs_module_text("Theme"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    for (int i = 0; i < kThemeCount; i++) {
        obs_property_list_add_string(theme, kThemes[i].name, kThemes[i].id);
    }
    obs_property_set_modified_callback(theme, theme_modified);

    obs_properties_add_color(p, "color_a", obs_module_text("ColorA"));
    obs_properties_add_color(p, "color_b", obs_module_text("ColorB"));
    obs_properties_add_color(p, "color_c", obs_module_text("ColorC"));

    obs_properties_add_float_slider(p, "intensity", obs_module_text("Intensity"), 0.0, 1.0, 0.01);

    // ── Reactivity ──
    obs_properties_add_float_slider(p, "reactivity", obs_module_text("Reactivity"), 0.0, 2.0, 0.01);
    obs_properties_add_float_slider(p, "speed", obs_module_text("Speed"), 0.1, 3.0, 0.01);
    obs_properties_add_float_slider(p, "beat_sensitivity", obs_module_text("BeatSensitivity"), 0.2, 3.0, 0.01);
    obs_property_t* ag = obs_properties_add_bool(p, "auto_gain", obs_module_text("AutoGain"));
    obs_property_set_long_description(ag, obs_module_text("AutoGain.Desc"));

    // Frequency window: narrow it for a bass-focused look, leave it wide for the
    // full spectrum. Applies to every scene, since it lives in the shared
    // spectrum() helper rather than in any one shader.
    obs_property_t* flo = obs_properties_add_float_slider(
        p, "freq_lo", obs_module_text("FreqLo"), 0.0, 95.0, 1.0);
    obs_property_set_long_description(flo, obs_module_text("FreqRange.Desc"));
    obs_properties_add_float_slider(p, "freq_hi", obs_module_text("FreqHi"), 5.0, 100.0, 1.0);

    // ── Colour grade ──
    obs_properties_add_float_slider(p, "hue_shift", obs_module_text("HueShift"), 0.0, 360.0, 1.0);
    obs_properties_add_float_slider(p, "saturation", obs_module_text("Saturation"), 0.0, 2.0, 0.01);
    obs_property_t* expo = obs_properties_add_float_slider(
        p, "exposure", obs_module_text("Exposure"), 0.1, 8.0, 0.05);
    obs_property_set_long_description(expo, obs_module_text("Exposure.Desc"));
    obs_properties_add_float_slider(p, "brightness", obs_module_text("Brightness"), 0.0, 3.0, 0.01);
    obs_properties_add_float_slider(p, "contrast", obs_module_text("Contrast"), 0.0, 2.0, 0.01);
    obs_properties_add_float_slider(p, "trail", obs_module_text("Trail"), 0.0, 1.0, 0.01);
    obs_properties_add_float_slider(p, "vignette", obs_module_text("Vignette"), 0.0, 1.0, 0.01);

    // ── Overlay ──────────────────────────────────────────────────────────────
    obs_properties_add_float_slider(p, "opacity", obs_module_text("Opacity"), 0.0, 100.0, 1.0);
    obs_property_t* tb = obs_properties_add_bool(p, "transparent_bg",
                                                 obs_module_text("TransparentBg"));
    obs_property_set_long_description(tb, obs_module_text("TransparentBg.Desc"));
    obs_property_t* alphaProp = obs_properties_add_float_slider(
        p, "alpha_gain", obs_module_text("AlphaGain"), 0.5, 8.0, 0.1);
    obs_property_set_long_description(alphaProp, obs_module_text("AlphaGain.Desc"));
    obs_property_t* afp = obs_properties_add_float_slider(
        p, "alpha_floor", obs_module_text("AlphaFloor"), 0.0, 0.30, 0.005);
    obs_property_set_long_description(afp, obs_module_text("AlphaFloor.Desc"));

    // One click to a sane overlay setup. Getting this right by hand means
    // knowing that the vignette darkens the frame edges (reads as a smudge over
    // a camera) and that the ambient floor has to be keyed out — neither is
    // obvious from the slider names.
    obs_properties_add_button(p, "overlay_preset", obs_module_text("OverlayPreset"),
        [](obs_properties_t*, obs_property_t*, void* data) -> bool {
            auto* gs = static_cast<groove_source*>(data);
            if (!gs || !gs->self) return false;
            obs_data_t* s = obs_source_get_settings(gs->self);
            obs_data_set_bool(s, "transparent_bg", true);
            obs_data_set_double(s, "vignette", 0.0);
            obs_data_set_double(s, "alpha_floor", 0.06);
            obs_data_set_double(s, "alpha_gain", 2.5);
            obs_data_set_double(s, "opacity", 100.0);
            obs_data_set_double(s, "trail", 0.2);
            obs_data_set_double(s, "exposure", 3.0);
            obs_source_update(gs->self, s);
            obs_data_release(s);
            return true;   // refresh the properties UI
        });

    obs_property_t* be = obs_properties_add_bool(p, "bloom_enabled", obs_module_text("BloomEnabled"));
    obs_property_set_modified_callback(be, bloom_modified);
    obs_properties_add_float_slider(p, "bloom", obs_module_text("Bloom"), 0.0, 2.0, 0.01);

    // ── Output ──
    obs_properties_add_int(p, "width", obs_module_text("Width"), 16, 7680, 2);
    obs_properties_add_int(p, "height", obs_module_text("Height"), 16, 4320, 2);

    obs_property_t* q = obs_properties_add_int_slider(p, "quality", obs_module_text("Quality"), 3, 6, 1);
    obs_property_set_long_description(q, obs_module_text("Quality.Desc"));


#ifdef GROOVE_MILKDROP
    // Modified callbacks only fire on change, so on first open the visibility
    // and the preset list would both be stale — which is what made this need
    // two trips through the dialog before presets appeared.
    if (auto* gsp = static_cast<groove_source*>(data); gsp && gsp->self) {
        obs_data_t* cur = obs_source_get_settings(gsp->self);
        on_visual_source_changed(nullptr, p, nullptr, cur);
        obs_data_release(cur);
    }
#endif
    return p;
}

static void groove_get_defaults(obs_data_t* s) {
    obs_data_set_default_string(s, "scene", "slow_bloom");
    obs_data_set_default_string(s, "theme", "groove");
    obs_data_set_default_string(s, "audio_source", "");
    obs_data_set_default_int(s, "width", 1920);
    obs_data_set_default_int(s, "height", 1080);
    obs_data_set_default_double(s, "intensity", 0.5);
    obs_data_set_default_double(s, "speed", 1.0);
    obs_data_set_default_double(s, "reactivity", 1.0);
    obs_data_set_default_double(s, "beat_sensitivity", 1.0);
    obs_data_set_default_bool(s, "auto_gain", true);
    obs_data_set_default_double(s, "freq_lo", 0.0);
    obs_data_set_default_double(s, "freq_hi", 100.0);
    obs_data_set_default_double(s, "hue_shift", 0.0);
    obs_data_set_default_double(s, "saturation", 1.0);
    obs_data_set_default_double(s, "brightness", 1.0);
    obs_data_set_default_double(s, "contrast", 1.0);
    obs_data_set_default_double(s, "trail", 0.35);
    obs_data_set_default_double(s, "vignette", 0.7);
    obs_data_set_default_double(s, "opacity", 100.0);
    obs_data_set_default_bool(s, "transparent_bg", false);
#ifdef GROOVE_MILKDROP
    obs_data_set_default_string(s, "visual_source", "scenes");
#endif
    obs_data_set_default_double(s, "alpha_gain", 2.0);
    obs_data_set_default_double(s, "alpha_floor", 0.04);
    obs_data_set_default_double(s, "exposure", 1.0);
    obs_data_set_default_bool(s, "bloom_enabled", true);
    obs_data_set_default_double(s, "bloom", 0.6);
    obs_data_set_default_int(s, "quality", 5);
    // OBS colour pickers are 0xAABBGGRR.
    obs_data_set_default_int(s, "color_a", 0xFFF755A8);
    obs_data_set_default_int(s, "color_b", 0xFFEED322);
    obs_data_set_default_int(s, "color_c", 0xFFFCABF0);
}

// Sources can be created before the audio source they listen to exists (scene
// collection load order is not guaranteed), so retry the binding while idle.
static void groove_video_tick(void* data, float) {
    auto* gs = static_cast<groove_source*>(data);
    if (gs->audio_source_name.empty() || gs->audio_weak) return;
    attach_audio(gs, gs->audio_source_name.c_str());
}

extern "C" struct obs_source_info groove_source_info;
struct obs_source_info groove_source_info = {};

void register_groove_source() {
    groove_source_info.id = "groove_visualizer";
    groove_source_info.type = OBS_SOURCE_TYPE_INPUT;
    groove_source_info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
    groove_source_info.get_name = groove_get_name;
    groove_source_info.create = groove_create;
    groove_source_info.destroy = groove_destroy;
    groove_source_info.update = groove_update;
    groove_source_info.get_defaults = groove_get_defaults;
    groove_source_info.get_properties = groove_get_properties;
    groove_source_info.get_width = groove_get_width;
    groove_source_info.get_height = groove_get_height;
    groove_source_info.video_render = groove_video_render;
    groove_source_info.video_tick = groove_video_tick;
    groove_source_info.icon_type = OBS_ICON_TYPE_MEDIA;

    obs_register_source(&groove_source_info);
}
