// Visual backend interface.
//
// SPEC §8 has described this since the first draft; until now it existed only
// in the spec, and `ENABLE_MILKDROP` referenced a source file that was never
// written — so turning the flag on failed to configure. This is the real thing.
//
// Two implementations:
//   SceneBackend  — the generated .effect scenes. Always built.
//   MilkBackend   — libprojectM 4, playing MilkDrop `.milk` presets. Optional,
//                   behind GROOVE_MILKDROP.
//
// The source keeps ownership of audio analysis and of the post chain. A backend
// is responsible for exactly one thing: given a frame of audio features and a
// size, produce a linear-HDR image. Everything downstream — bloom, trails,
// grade — is shared, which is what keeps the two backends looking like the same
// product rather than two apps in a trenchcoat.

#pragma once

#include <string>
#include <vector>

#include "groove_analyzer.hpp"

namespace groove {

/// What a backend needs from the host each frame.
struct RenderRequest {
    const Frame* frame = nullptr;   ///< analysed audio features
    uint32_t width = 1920;
    uint32_t height = 1080;
    float dt = 1.0f / 60.0f;

    // Shared look controls. A backend honours what it can and ignores the rest:
    // a .milk preset carries its own colours, so themes do not apply to it.
    float intensity = 0.5f;
    float speed = 1.0f;
    float beat_sensitivity = 1.0f;
};

/// One selectable item — a scene id or a preset file path.
struct BackendItem {
    std::string id;      ///< stable identifier, stored in OBS settings
    std::string label;   ///< what the user sees
};

class VisualBackend {
public:
    virtual ~VisualBackend() = default;

    /// Human name for logs and the properties UI.
    virtual const char* name() const = 0;

    /// Everything this backend can play, for the source's dropdown.
    virtual std::vector<BackendItem> items() const = 0;

    /// Select an item. Called on the graphics thread.
    virtual void load(const std::string& id) = 0;

    /// Raw PCM, straight off the audio thread. Mono float, [-1, 1].
    ///
    /// Backends that do their own analysis need the samples, not our features:
    /// projectM runs its own beat detection and expects to be fed PCM. Our
    /// analyzer still consumes the same stream in parallel, so the status
    /// readout and the post chain keep working regardless of backend.
    virtual void push_audio(const float* samples, size_t count) = 0;

    /// Render into `fbo` at the requested size, in linear HDR.
    /// `fbo` is an OpenGL framebuffer object name. Returns false if the backend
    /// could not draw, in which case the host leaves the previous frame up
    /// rather than flashing black.
    virtual bool render(const RenderRequest& req, uint32_t fbo) = 0;

    /// True once the backend is usable. A backend that failed to initialise
    /// must report false rather than drawing nothing every frame in silence.
    virtual bool ready() const = 0;

    /// Why it is not ready, for the log and the UI. Empty when fine.
    virtual std::string status() const { return {}; }

    /// Point the backend at a user folder of content and rescan. No-op for
    /// backends whose content is compiled in.
    virtual void set_source_directory(const std::string&) {}
};

}  // namespace groove
