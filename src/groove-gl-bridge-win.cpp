// GLBridge for Windows, where OBS renders with Direct3D 11 and libprojectM
// renders with OpenGL.
//
// There is no framebuffer to hand over here, and no GL context at all, so the
// macOS/Linux strategy does not port. This file creates a private WGL context
// and gets pixels across the API boundary by one of two routes:
//
//   1. WGL_NV_DX_interop2 — the good one. Despite the NV in the name this is
//      implemented by AMD and Intel too. It lets a single piece of GPU memory
//      be addressed as both an OBS D3D11 texture and a GL texture, so
//      projectM renders directly into what OBS is about to composite. No copy.
//
//   2. glReadPixels + gs_texture_set_image — the fallback. A full GPU->CPU->GPU
//      round trip every frame, roughly 500 MB/s at 1080p60. Ugly, but it works
//      on any driver, and a visualiser that costs some bandwidth is strictly
//      better than one that shows a black rectangle.
//
// The bridge tries 1, and drops to 2 on ANY failure rather than giving up.
// That matters more than usual here: interop registration has real driver
// variance, and this file cannot be run on the machine it was written on. The
// design goal is that the uncertain path fails *detectably* into a working
// one, and that transport() reports which route is live so a bug report says
// "readback" instead of "it's slow".
//
// ── UNVERIFIED ──────────────────────────────────────────────────────────────
// No part of this file has been executed. It is written against the WGL and
// libobs documentation and compiled in CI; it has never rendered a frame.
// Before any release, someone with a Windows OBS must confirm: that a preset
// appears at all, which transport the log reports, that other sources in the
// scene still render correctly afterwards, and that quitting OBS with the
// source active does not crash. Treat green CI as "it builds", nothing more.

#include "groove-gl-bridge.hpp"

#if defined(_WIN32)

#include "groove-gl.hpp"

#include <obs-module.h>

#include <vector>

// ── WGL_NV_DX_interop2 ───────────────────────────────────────────────────────
#define WGL_ACCESS_READ_ONLY_NV     0x0000
#define WGL_ACCESS_READ_WRITE_NV    0x0001
#define WGL_ACCESS_WRITE_DISCARD_NV 0x0002

typedef HANDLE(WINAPI* PFNWGLDXOPENDEVICENV)(void*);
typedef BOOL(WINAPI* PFNWGLDXCLOSEDEVICENV)(HANDLE);
typedef HANDLE(WINAPI* PFNWGLDXREGISTEROBJECTNV)(HANDLE, void*, GLuint, GLenum, GLenum);
typedef BOOL(WINAPI* PFNWGLDXUNREGISTEROBJECTNV)(HANDLE, HANDLE);
typedef BOOL(WINAPI* PFNWGLDXLOCKOBJECTSNV)(HANDLE, GLint, HANDLE*);
typedef BOOL(WINAPI* PFNWGLDXUNLOCKOBJECTSNV)(HANDLE, GLint, HANDLE*);

typedef HGLRC(WINAPI* PFNWGLCREATECONTEXTATTRIBSARB)(HDC, HGLRC, const int*);

#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB  0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001

namespace groove {

namespace {

class WinBridge final : public GLBridge {
public:
    ~WinBridge() override { teardown(); }

    uint32_t begin(uint32_t cx, uint32_t cy) override {
        if (open_) return 0;
        if (!ensure_context()) return 0;
        if (!ensure_target(cx, cy)) return 0;

        // Remember what was current so OBS's own context (if any) is restored
        // exactly. Never assume the previous context was null.
        prev_dc_ = wglGetCurrentDC();
        prev_rc_ = wglGetCurrentContext();
        if (!wglMakeCurrent(dc_, rc_)) {
            fail("Could not make the Groovalizer OpenGL context current.");
            return 0;
        }

        if (interop_ && !wglDXLockObjectsNV_(dx_device_, 1, &dx_object_)) {
            // A lock failure mid-session usually means a device reset. Drop to
            // readback for the rest of the session rather than dying.
            blog(LOG_WARNING, "[groovalizer] DX interop lock failed; "
                              "falling back to readback for this session");
            release_interop();
            if (!ensure_target(cx, cy)) { restore_context(); return 0; }
        }

        gg_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_);
        glViewport(0, 0, GLsizei(cx), GLsizei(cy));

        open_ = true;
        return fbo_;
    }

    gs_texture_t* end() override {
        if (!open_) return nullptr;
        open_ = false;

        gs_texture_t* result = nullptr;

        if (interop_) {
            gg_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            if (wglDXUnlockObjectsNV_(dx_device_, 1, &dx_object_)) {
                result = tex_;
            }
        } else {
            // Readback. glReadPixels stalls the pipeline waiting on the draw
            // to finish; that is the cost of this path and why it is not the
            // default. RGBA8 rather than 16F on purpose — the milk path goes
            // straight to Present without the HDR bloom stages, so the extra
            // range would only double the bandwidth of the slowest path.
            glReadPixels(0, 0, GLsizei(cx_), GLsizei(cy_), GL_RGBA,
                         GL_UNSIGNED_BYTE, staging_.data());
            gg_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            restore_context();
            gs_texture_set_image(tex_, staging_.data(), cx_ * 4, false);
            return tex_;
        }

        restore_context();
        return result;
    }

    std::string status() const override { return status_; }

    const char* transport() const override {
        return interop_ ? "shared texture (WGL_NV_DX_interop2)" : "readback";
    }

private:
    void restore_context() { wglMakeCurrent(prev_dc_, prev_rc_); }

    void fail(const char* why) {
        status_ = why;
        blog(LOG_WARNING, "[groovalizer] %s", why);
    }

    // ── Context ─────────────────────────────────────────────────────────────
    bool ensure_context() {
        if (rc_) return true;
        if (!status_.empty()) return false;  // already failed; do not retry per frame

        WNDCLASSA wc = {};
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "GroovalizerGL";
        RegisterClassA(&wc);  // benign if already registered

        wnd_ = CreateWindowExA(0, "GroovalizerGL", "", WS_OVERLAPPED, 0, 0, 1, 1,
                               nullptr, nullptr, wc.hInstance, nullptr);
        if (!wnd_) { fail("Could not create the hidden OpenGL window."); return false; }

        dc_ = GetDC(wnd_);
        PIXELFORMATDESCRIPTOR pfd = {};
        pfd.nSize = sizeof(pfd);
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        const int pf = ChoosePixelFormat(dc_, &pfd);
        if (!pf || !SetPixelFormat(dc_, pf, &pfd)) {
            fail("No suitable OpenGL pixel format.");
            return false;
        }

        // Two-step context creation: a legacy context only so that
        // wglCreateContextAttribsARB can be resolved, then the real core
        // context. wglGetProcAddress returns null without something current.
        HGLRC tmp = wglCreateContext(dc_);
        if (!tmp || !wglMakeCurrent(dc_, tmp)) {
            fail("Could not create a bootstrap OpenGL context.");
            return false;
        }

        auto createAttribs = reinterpret_cast<PFNWGLCREATECONTEXTATTRIBSARB>(
            wglGetProcAddress("wglCreateContextAttribsARB"));
        if (createAttribs) {
            const int attribs[] = {
                WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
                WGL_CONTEXT_MINOR_VERSION_ARB, 3,
                WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
                0
            };
            rc_ = createAttribs(dc_, nullptr, attribs);
        }

        if (rc_) {
            wglMakeCurrent(dc_, rc_);
            wglDeleteContext(tmp);
        } else {
            // No 3.3 core available. Keep the legacy context and let
            // groove_gl_load() below decide whether it is good enough.
            rc_ = tmp;
        }

        if (!groove_gl_load()) {
            fail("This GPU driver is missing OpenGL 3.3 entry points required "
                 "by the MilkDrop engine.");
            return false;
        }

        gg_glGenFramebuffers(1, &fbo_);
        setup_interop();

        blog(LOG_INFO, "[groovalizer] Windows GL bridge up, transport: %s", transport());
        return true;
    }

    void setup_interop() {
        // OBS on D3D11 hands back an ID3D11Device here. On an OpenGL OBS this
        // is a GL context pointer and interop is neither possible nor needed —
        // but that build uses the native bridge, so reaching here at all means
        // D3D11.
        void* device = gs_get_device_obj();
        if (!device) return;

        wglDXOpenDeviceNV_ = reinterpret_cast<PFNWGLDXOPENDEVICENV>(
            wglGetProcAddress("wglDXOpenDeviceNV"));
        wglDXCloseDeviceNV_ = reinterpret_cast<PFNWGLDXCLOSEDEVICENV>(
            wglGetProcAddress("wglDXCloseDeviceNV"));
        wglDXRegisterObjectNV_ = reinterpret_cast<PFNWGLDXREGISTEROBJECTNV>(
            wglGetProcAddress("wglDXRegisterObjectNV"));
        wglDXUnregisterObjectNV_ = reinterpret_cast<PFNWGLDXUNREGISTEROBJECTNV>(
            wglGetProcAddress("wglDXUnregisterObjectNV"));
        wglDXLockObjectsNV_ = reinterpret_cast<PFNWGLDXLOCKOBJECTSNV>(
            wglGetProcAddress("wglDXLockObjectsNV"));
        wglDXUnlockObjectsNV_ = reinterpret_cast<PFNWGLDXUNLOCKOBJECTSNV>(
            wglGetProcAddress("wglDXUnlockObjectsNV"));

        if (!wglDXOpenDeviceNV_ || !wglDXRegisterObjectNV_ || !wglDXLockObjectsNV_ ||
            !wglDXUnlockObjectsNV_ || !wglDXCloseDeviceNV_ || !wglDXUnregisterObjectNV_) {
            blog(LOG_INFO, "[groovalizer] WGL_NV_DX_interop2 unavailable; using readback");
            return;
        }

        dx_device_ = wglDXOpenDeviceNV_(device);
        if (!dx_device_) {
            blog(LOG_INFO, "[groovalizer] wglDXOpenDeviceNV failed; using readback");
            return;
        }
        interop_ = true;
    }

    // ── Target ──────────────────────────────────────────────────────────────
    bool ensure_target(uint32_t cx, uint32_t cy) {
        if (tex_ && cx == cx_ && cy == cy_) return true;
        release_target();
        cx_ = cx;
        cy_ = cy;

        if (interop_) {
            // GS_SHARED_TEX asks libobs for a D3D11 texture that other APIs
            // may address. Plain render targets are rejected by some drivers
            // at registration time; asking for shared up front avoids finding
            // that out per-driver.
            tex_ = gs_texture_create(cx, cy, GS_RGBA, 1, nullptr,
                                     GS_RENDER_TARGET | GS_SHARED_TEX);
            if (!tex_) { release_interop(); return ensure_target(cx, cy); }

            void* d3d_tex = gs_texture_get_obj(tex_);
            if (!d3d_tex) { release_interop(); return ensure_target(cx, cy); }

            glGenTextures(1, &gl_tex_);
            dx_object_ = wglDXRegisterObjectNV_(dx_device_, d3d_tex, gl_tex_,
                                                GL_TEXTURE_2D,
                                                WGL_ACCESS_WRITE_DISCARD_NV);
            if (!dx_object_) {
                blog(LOG_WARNING, "[groovalizer] wglDXRegisterObjectNV failed "
                                  "(0x%lx); using readback", GetLastError());
                release_interop();
                return ensure_target(cx, cy);
            }
        } else {
            // Readback: our own GL texture, plus a dynamic OBS texture and a
            // CPU staging buffer to move between them.
            glGenTextures(1, &gl_tex_);
            glBindTexture(GL_TEXTURE_2D, gl_tex_);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, GLsizei(cx), GLsizei(cy), 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glBindTexture(GL_TEXTURE_2D, 0);

            staging_.assign(size_t(cx) * cy * 4, 0);
            tex_ = gs_texture_create(cx, cy, GS_RGBA, 1, nullptr, GS_DYNAMIC);
            if (!tex_) { fail("Could not allocate the MilkDrop output texture."); return false; }
        }

        gg_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_);
        gg_glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                  GL_TEXTURE_2D, gl_tex_, 0);
        const GLenum ok = gg_glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
        gg_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        if (ok != GL_FRAMEBUFFER_COMPLETE) {
            fail("The MilkDrop render target is incomplete.");
            return false;
        }
        return true;
    }

    void release_interop() {
        if (dx_object_ && wglDXUnregisterObjectNV_)
            wglDXUnregisterObjectNV_(dx_device_, dx_object_);
        if (dx_device_ && wglDXCloseDeviceNV_) wglDXCloseDeviceNV_(dx_device_);
        dx_object_ = nullptr;
        dx_device_ = nullptr;
        interop_ = false;
        release_target();
    }

    void release_target() {
        if (dx_object_ && wglDXUnregisterObjectNV_) {
            wglDXUnregisterObjectNV_(dx_device_, dx_object_);
            dx_object_ = nullptr;
        }
        if (gl_tex_) { glDeleteTextures(1, &gl_tex_); gl_tex_ = 0; }
        if (tex_) {
            obs_enter_graphics();
            gs_texture_destroy(tex_);
            obs_leave_graphics();
            tex_ = nullptr;
        }
        staging_.clear();
        cx_ = cy_ = 0;
    }

    void teardown() {
        if (rc_) {
            wglMakeCurrent(dc_, rc_);
            release_interop();
            if (fbo_) { gg_glDeleteFramebuffers(1, &fbo_); fbo_ = 0; }
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(rc_);
            rc_ = nullptr;
        }
        if (dc_ && wnd_) ReleaseDC(wnd_, dc_);
        if (wnd_) DestroyWindow(wnd_);
        dc_ = nullptr;
        wnd_ = nullptr;
    }

    HWND  wnd_ = nullptr;
    HDC   dc_ = nullptr, prev_dc_ = nullptr;
    HGLRC rc_ = nullptr, prev_rc_ = nullptr;

    GLuint fbo_ = 0, gl_tex_ = 0;
    gs_texture_t* tex_ = nullptr;
    std::vector<uint8_t> staging_;
    uint32_t cx_ = 0, cy_ = 0;
    bool open_ = false;
    bool interop_ = false;
    std::string status_;

    HANDLE dx_device_ = nullptr;
    HANDLE dx_object_ = nullptr;

    PFNWGLDXOPENDEVICENV       wglDXOpenDeviceNV_ = nullptr;
    PFNWGLDXCLOSEDEVICENV      wglDXCloseDeviceNV_ = nullptr;
    PFNWGLDXREGISTEROBJECTNV   wglDXRegisterObjectNV_ = nullptr;
    PFNWGLDXUNREGISTEROBJECTNV wglDXUnregisterObjectNV_ = nullptr;
    PFNWGLDXLOCKOBJECTSNV      wglDXLockObjectsNV_ = nullptr;
    PFNWGLDXUNLOCKOBJECTSNV    wglDXUnlockObjectsNV_ = nullptr;
};

}  // namespace

std::unique_ptr<GLBridge> make_gl_bridge() {
    return std::make_unique<WinBridge>();
}

}  // namespace groove

#endif  // _WIN32
