// Getting libprojectM's OpenGL output into an OBS texture.
//
// This is the whole Windows problem in one interface.
//
// On macOS and Linux, OBS's graphics backend *is* OpenGL, so the trick is
// trivial: gs_texrender_begin() binds a GL framebuffer targeting an OBS
// texture, we ask GL what got bound, and projectM draws straight into it.
// Zero copy, no extra context, nothing to synchronise.
//
// On Windows, OBS defaults to Direct3D 11. There is no GL framebuffer to
// hand over and no GL context at all, so the same code cannot merely be
// ported — the strategy has to change. The Windows bridge creates its own
// private WGL context and shares a texture between it and OBS's D3D11
// device, so projectM keeps rendering to a GL framebuffer while OBS keeps
// seeing a D3D11 texture, with neither side aware of the other.
//
// Both strategies are hidden behind begin()/end(). Call sites do not branch
// on platform.
//
//     uint32_t fbo = bridge->begin(cx, cy);
//     if (fbo) { projectM renders into fbo; }
//     gs_texture_t* tex = bridge->end();
//
// end() must be called if and only if begin() returned non-zero, and both
// must run on the graphics thread.

#pragma once

#include <memory>
#include <string>

#include <graphics/graphics.h>

namespace groove {

class GLBridge {
public:
    virtual ~GLBridge() = default;

    /// Prepare a render target `cx` by `cy` and return the OpenGL framebuffer
    /// name projectM should draw into. Returns 0 if unavailable, in which case
    /// end() must NOT be called and status() explains why.
    virtual uint32_t begin(uint32_t cx, uint32_t cy) = 0;

    /// Close out the frame. Returns the OBS texture holding the result, or
    /// nullptr if the frame could not be completed.
    virtual gs_texture_t* end() = 0;

    /// Human-readable reason the bridge is unusable. Empty when healthy.
    /// Surfaced in the source's status readout, because "black screen" is a
    /// terrible way to learn your driver lacks an extension.
    virtual std::string status() const { return {}; }

    /// How the frame reaches OBS, for the log and the properties panel.
    /// "shared texture" on the zero-copy paths, "readback" on the fallback.
    virtual const char* transport() const = 0;
};

/// Build the bridge for this platform. Never returns null — a bridge that
/// cannot work still reports a status, so the failure is explained rather
/// than silent.
std::unique_ptr<GLBridge> make_gl_bridge();

}  // namespace groove
