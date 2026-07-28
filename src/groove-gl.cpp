// Windows OpenGL entry-point loader. See groove-gl.hpp for why this exists.

#include "groove-gl.hpp"

#if defined(_WIN32)

GG_PFNGLBINDFRAMEBUFFER        gg_glBindFramebuffer = nullptr;
GG_PFNGLGENFRAMEBUFFERS        gg_glGenFramebuffers = nullptr;
GG_PFNGLDELETEFRAMEBUFFERS     gg_glDeleteFramebuffers = nullptr;
GG_PFNGLFRAMEBUFFERTEXTURE2D   gg_glFramebufferTexture2D = nullptr;
GG_PFNGLCHECKFRAMEBUFFERSTATUS gg_glCheckFramebufferStatus = nullptr;
GG_PFNGLUSEPROGRAM             gg_glUseProgram = nullptr;
GG_PFNGLBINDVERTEXARRAY        gg_glBindVertexArray = nullptr;
GG_PFNGLBINDBUFFER             gg_glBindBuffer = nullptr;
GG_PFNGLACTIVETEXTURE          gg_glActiveTexture = nullptr;

namespace {

/// wglGetProcAddress only resolves functions beyond GL 1.1, and returns
/// assorted non-null error values on some drivers rather than a clean null.
/// Anything it will not give us is looked up in opengl32.dll directly.
void* gl_sym(const char* name) {
    void* p = reinterpret_cast<void*>(wglGetProcAddress(name));
    const auto bad = reinterpret_cast<intptr_t>(p);
    if (bad == 0 || bad == 1 || bad == 2 || bad == 3 || bad == -1) {
        static HMODULE gl = GetModuleHandleA("opengl32.dll");
        p = gl ? reinterpret_cast<void*>(GetProcAddress(gl, name)) : nullptr;
    }
    return p;
}

}  // namespace

bool groove_gl_load() {
    if (gg_glBindFramebuffer) return true;  // already loaded

    gg_glBindFramebuffer        = reinterpret_cast<GG_PFNGLBINDFRAMEBUFFER>(gl_sym("glBindFramebuffer"));
    gg_glGenFramebuffers        = reinterpret_cast<GG_PFNGLGENFRAMEBUFFERS>(gl_sym("glGenFramebuffers"));
    gg_glDeleteFramebuffers     = reinterpret_cast<GG_PFNGLDELETEFRAMEBUFFERS>(gl_sym("glDeleteFramebuffers"));
    gg_glFramebufferTexture2D   = reinterpret_cast<GG_PFNGLFRAMEBUFFERTEXTURE2D>(gl_sym("glFramebufferTexture2D"));
    gg_glCheckFramebufferStatus = reinterpret_cast<GG_PFNGLCHECKFRAMEBUFFERSTATUS>(gl_sym("glCheckFramebufferStatus"));
    gg_glUseProgram             = reinterpret_cast<GG_PFNGLUSEPROGRAM>(gl_sym("glUseProgram"));
    gg_glBindVertexArray        = reinterpret_cast<GG_PFNGLBINDVERTEXARRAY>(gl_sym("glBindVertexArray"));
    gg_glBindBuffer             = reinterpret_cast<GG_PFNGLBINDBUFFER>(gl_sym("glBindBuffer"));
    gg_glActiveTexture          = reinterpret_cast<GG_PFNGLACTIVETEXTURE>(gl_sym("glActiveTexture"));

    const bool ok = gg_glBindFramebuffer && gg_glGenFramebuffers &&
                    gg_glDeleteFramebuffers && gg_glFramebufferTexture2D &&
                    gg_glCheckFramebufferStatus && gg_glUseProgram &&
                    gg_glBindVertexArray && gg_glBindBuffer && gg_glActiveTexture;
    if (!ok) gg_glBindFramebuffer = nullptr;  // so a later call retries
    return ok;
}

#endif  // _WIN32
