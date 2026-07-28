// MilkDrop preset backend, via libprojectM 4.
//
// Built only when GROOVE_MILKDROP is defined. See docs and SPEC §8.
//
// ── What this does and does not ship ────────────────────────────────────────
//
// The *engine* is LGPL-2.1 and links dynamically, which is fine in OBS (a GPL
// host). The *presets* are a separate question with a different answer: only
// the 48 first-party presets are bundled. Everything else comes from a folder
// the user points us at. See SPEC §8.2 — the community packs were shared
// without licence terms and carry no grant for commercial redistribution, so
// the importer is the feature and the bundle is not.
//
// ── The rendering constraint, stated up front ───────────────────────────────
//
// projectM renders through OpenGL. OBS uses OpenGL on macOS and Linux, and
// Direct3D 11 by default on Windows. `projectm_opengl_render_frame_fbo` needs a
// real GL framebuffer, so this backend requires OBS's OpenGL renderer. On a
// D3D11 OBS it reports not-ready with an explanatory status rather than drawing
// black — a readback path through a private WGL context is possible later, but
// a half-working one is worse than an honest refusal.

#include "groove-backend.hpp"

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <vector>

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/platform.h>

#include <projectM-4/projectM.h>

namespace groove {

namespace fs = std::filesystem;

namespace {

/// Presets we may legally ship. Anything else is user-supplied.
constexpr const char* kBundledDir = "presets";

bool is_milk(const fs::path& p) {
    if (!p.has_extension()) return false;
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(::tolower(c)); });
    return ext == ".milk";
}

}  // namespace

class MilkBackend final : public VisualBackend {
public:
    MilkBackend() { scan_presets(); }

    ~MilkBackend() override {
        if (pm_) projectm_destroy(pm_);
    }

    const char* name() const override { return "MilkDrop"; }

    std::vector<BackendItem> items() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return items_;
    }

    void load(const std::string& id) override {
        pending_preset_ = id;
    }

    void push_audio(const float* samples, size_t count) override {
        // projectM keeps its own ring buffer and does its own analysis, so it
        // wants PCM rather than our features. Feeding more than it can hold in
        // one call is silently truncated upstream, so chunk it.
        if (!pm_ || !samples || count == 0) return;
        const size_t max_samples = static_cast<size_t>(projectm_pcm_get_max_samples());
        if (max_samples == 0) return;

        std::lock_guard<std::mutex> lock(pcm_mutex_);
        size_t offset = 0;
        while (offset < count) {
            const size_t n = std::min(max_samples, count - offset);
            projectm_pcm_add_float(pm_, samples + offset, static_cast<unsigned int>(n),
                                   PROJECTM_MONO);
            offset += n;
        }
    }

    bool render(const RenderRequest& req, uint32_t fbo) override {
        if (!ensure_instance()) return false;

        if (req.width != width_ || req.height != height_) {
            width_ = req.width;
            height_ = req.height;
            projectm_set_window_size(pm_, width_, height_);
        }

        if (!pending_preset_.empty()) {
            // `false` = hard cut. A soft transition blends two presets, which
            // looks good when projectM is driving its own playlist but reads as
            // a bug when the user has just picked something from a list.
            projectm_load_preset_file(pm_, pending_preset_.c_str(), false);
            current_preset_ = pending_preset_;
            pending_preset_.clear();
        }
        if (current_preset_.empty()) return false;

        // Our beat-sensitivity control is nominally 0.2–3.0; projectM's is
        // roughly 0–5 with 1.0 neutral. Pass it through rather than inventing a
        // curve — both are "multiplier on a relative-energy threshold", and the
        // detectors share a MilkDrop lineage.
        projectm_set_beat_sensitivity(pm_, req.beat_sensitivity);
        if (req.dt > 1e-6f) projectm_set_fps(pm_, static_cast<int32_t>(1.0f / req.dt));

        projectm_opengl_render_frame_fbo(pm_, fbo);
        return true;
    }

    bool ready() const override { return pm_ != nullptr && status_.empty(); }

    std::string status() const override { return status_; }

private:
    bool ensure_instance() {
        if (pm_) return true;
        if (!status_.empty()) return false;   // already failed; do not retry per frame

        // projectM resolves GL entry points on creation, so this must run on the
        // graphics thread with a current context. If OBS is on D3D11 there is no
        // GL context and creation will fail — report it once, clearly.
        pm_ = projectm_create();
        if (!pm_) {
            status_ =
                "libprojectM could not start. The MilkDrop backend needs OBS's "
                "OpenGL renderer; on Windows OBS defaults to Direct3D 11.";
            blog(LOG_WARNING, "[groovalizer] %s", status_.c_str());
            return false;
        }

        projectm_set_aspect_correction(pm_, true);
        // Preset switching is ours, not projectM's: the source has a scene
        // dropdown and an auto-shuffle setting already, and two things changing
        // the preset fight each other.
        projectm_set_preset_locked(pm_, true);
        projectm_set_mesh_size(pm_, 48, 32);
        blog(LOG_INFO, "[groovalizer] libprojectM %s", projectm_get_version_string());
        return true;
    }

    /// Bundled first-party presets, plus whatever folder the user chose.
    void scan_presets() {
        std::lock_guard<std::mutex> lock(mutex_);
        items_.clear();

        if (char* dir = obs_module_file(kBundledDir)) {
            add_dir(fs::path(dir), "Groovalizer");
            bfree(dir);
        }
        if (!user_dir_.empty()) add_dir(fs::path(user_dir_), "My presets");

        std::sort(items_.begin(), items_.end(),
                  [](const BackendItem& a, const BackendItem& b) { return a.label < b.label; });
    }

    void add_dir(const fs::path& dir, const std::string& group) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) return;
        for (auto it = fs::recursive_directory_iterator(
                 dir, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec) || !is_milk(it->path())) continue;
            items_.push_back({it->path().string(),
                              group + " · " + it->path().stem().string()});
        }
    }

public:
    /// Point the backend at a user folder of `.milk` files and rescan.
    void set_source_directory(const std::string& dir) override {
        if (dir == user_dir_) return;
        user_dir_ = dir;
        scan_presets();
    }

private:
    projectm_handle pm_ = nullptr;
    uint32_t width_ = 0, height_ = 0;
    std::string current_preset_;
    std::string pending_preset_;
    std::string user_dir_;
    std::string status_;

    mutable std::mutex mutex_;
    std::mutex pcm_mutex_;
    std::vector<BackendItem> items_;
};

std::unique_ptr<VisualBackend> make_milk_backend() {
    return std::make_unique<MilkBackend>();
}

}  // namespace groove
