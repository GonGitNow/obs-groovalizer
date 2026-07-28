// GLBridge for platforms where OBS itself runs on OpenGL: macOS and Linux.
//
// Nothing is shared or copied here. gs_texrender_begin() binds a GL
// framebuffer that targets an OBS texture; we read that binding back out of
// GL and hand it to projectM, which renders into OBS's own storage.
//
// The delicate part is not the handover, it is the cleanup. projectM sets its
// own program, VAO, buffer bindings, active texture unit, viewport and enable
// bits, and restores none of them. libobs assumes it owns all of that, so
// leaving projectM's state behind does not corrupt the visualiser — it
// corrupts every *other* source in the scene, which is a far more confusing
// bug to receive a report about. Save before, restore after.

#include "groove-gl-bridge.hpp"

#if !defined(_WIN32)

#include "groove-gl.hpp"

#include <obs-module.h>
#include <graphics/vec4.h>

namespace groove {

namespace {

class NativeBridge final : public GLBridge {
public:
    NativeBridge() { tr_ = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE); }

    ~NativeBridge() override {
        if (tr_) {
            obs_enter_graphics();
            gs_texrender_destroy(tr_);
            obs_leave_graphics();
        }
    }

    uint32_t begin(uint32_t cx, uint32_t cy) override {
        if (!tr_ || open_) return 0;
        if (!groove_gl_load()) return 0;

        gs_texrender_reset(tr_);
        if (!gs_texrender_begin(tr_, cx, cy)) return 0;

        vec4 clear;
        vec4_zero(&clear);
        gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
        gs_ortho(0.0f, float(cx), 0.0f, float(cy), -100.0f, 100.0f);
        gs_blend_state_push();
        gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

        GLint bound = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &bound);

        // Snapshot everything projectM is about to trample.
        glGetIntegerv(GL_VIEWPORT, vp_);
        glGetIntegerv(GL_CURRENT_PROGRAM, &prog_);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao_);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buf_);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active_tex_);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex2d_);
        blend_   = glIsEnabled(GL_BLEND);
        depth_   = glIsEnabled(GL_DEPTH_TEST);
        cull_    = glIsEnabled(GL_CULL_FACE);
        scissor_ = glIsEnabled(GL_SCISSOR_TEST);

        fbo_  = uint32_t(bound);
        open_ = true;
        return fbo_;
    }

    gs_texture_t* end() override {
        if (!open_) return nullptr;
        open_ = false;

        gg_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(fbo_));
        glViewport(vp_[0], vp_[1], vp_[2], vp_[3]);
        gg_glUseProgram(GLuint(prog_));
        gg_glBindVertexArray(GLuint(vao_));
        gg_glBindBuffer(GL_ARRAY_BUFFER, GLuint(array_buf_));
        gg_glActiveTexture(GLenum(active_tex_));
        glBindTexture(GL_TEXTURE_2D, GLuint(tex2d_));
        if (blend_)   glEnable(GL_BLEND);        else glDisable(GL_BLEND);
        if (depth_)   glEnable(GL_DEPTH_TEST);   else glDisable(GL_DEPTH_TEST);
        if (cull_)    glEnable(GL_CULL_FACE);    else glDisable(GL_CULL_FACE);
        if (scissor_) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);

        gs_blend_state_pop();
        gs_texrender_end(tr_);
        return gs_texrender_get_texture(tr_);
    }

    const char* transport() const override { return "shared framebuffer"; }

private:
    gs_texrender_t* tr_ = nullptr;
    uint32_t fbo_ = 0;
    bool open_ = false;

    GLint vp_[4] = {0, 0, 0, 0};
    GLint prog_ = 0, vao_ = 0, array_buf_ = 0, active_tex_ = 0, tex2d_ = 0;
    GLboolean blend_ = 0, depth_ = 0, cull_ = 0, scissor_ = 0;
};

}  // namespace

std::unique_ptr<GLBridge> make_gl_bridge() {
    return std::make_unique<NativeBridge>();
}

}  // namespace groove

#endif  // !_WIN32
