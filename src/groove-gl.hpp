// Platform OpenGL entry points for the MilkDrop backend.
//
// Only compiled when GROOVE_MILKDROP is on. libprojectM renders through
// OpenGL, so every path that touches it needs GL symbols — but the three
// platforms disagree about where those come from:
//
//   macOS   <OpenGL/gl3.h>, everything through GL 4.1 linked directly.
//   Linux   <GL/gl.h> + <GL/glext.h>, resolved by libGL at link time.
//   Windows opengl32.lib exports GL 1.1 and NOTHING ELSE. Every function
//           added after 1997 — glBindFramebuffer, glUseProgram,
//           glBindVertexArray, glActiveTexture — must be fetched at runtime
//           with wglGetProcAddress against a current context.
//
// That last point is why this header exists. The previous code included
// <OpenGL/gl3.h> unconditionally, so the milk path could not compile off
// macOS at all.
//
// Call sites use the gg_ prefix uniformly. On macOS and Linux those are
// #defines onto the real symbols and cost nothing; on Windows they are
// function pointers filled in by groove_gl_load(). Anything already in
// GL 1.1 (glGetIntegerv, glViewport, glBindTexture, glEnable, glIsEnabled,
// glGenTextures, glReadPixels) is called directly on all three, because
// Windows does export those.

#pragma once

#include <cstdint>

#if defined(_WIN32)
  // Keep windows.h from dragging in winsock and the min/max macros, which
  // collide with std::min/std::max used throughout this plugin.
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #include <windows.h>
  #include <GL/gl.h>

  // <GL/gl.h> on Windows is stuck at 1.1, so the modern tokens and the
  // pointer typedefs are not there either. Declare exactly what we use
  // rather than taking a dependency on glad or GLEW — this is a dozen
  // symbols, and a loader library would be a new build-time dependency on
  // the one platform whose build is already the hardest to get right.
  #ifndef GL_FRAMEBUFFER
  #define GL_FRAMEBUFFER                    0x8D40
  #define GL_DRAW_FRAMEBUFFER               0x8CA9
  #define GL_DRAW_FRAMEBUFFER_BINDING       0x8CA6
  #define GL_COLOR_ATTACHMENT0              0x8CE0
  #define GL_FRAMEBUFFER_COMPLETE           0x8CD5
  #define GL_ARRAY_BUFFER                   0x8892
  #define GL_ARRAY_BUFFER_BINDING           0x8894
  #define GL_VERTEX_ARRAY_BINDING           0x85B5
  #define GL_CURRENT_PROGRAM                0x8B8D
  #define GL_ACTIVE_TEXTURE                 0x84E0
  #define GL_TEXTURE0                       0x84C0
  #define GL_CLAMP_TO_EDGE                  0x812F
  #define GL_BGRA                           0x80E1
  #endif

  typedef void (APIENTRY* GG_PFNGLBINDFRAMEBUFFER)(GLenum, GLuint);
  typedef void (APIENTRY* GG_PFNGLGENFRAMEBUFFERS)(GLsizei, GLuint*);
  typedef void (APIENTRY* GG_PFNGLDELETEFRAMEBUFFERS)(GLsizei, const GLuint*);
  typedef void (APIENTRY* GG_PFNGLFRAMEBUFFERTEXTURE2D)(GLenum, GLenum, GLenum, GLuint, GLint);
  typedef GLenum (APIENTRY* GG_PFNGLCHECKFRAMEBUFFERSTATUS)(GLenum);
  typedef void (APIENTRY* GG_PFNGLUSEPROGRAM)(GLuint);
  typedef void (APIENTRY* GG_PFNGLBINDVERTEXARRAY)(GLuint);
  typedef void (APIENTRY* GG_PFNGLBINDBUFFER)(GLenum, GLuint);
  typedef void (APIENTRY* GG_PFNGLACTIVETEXTURE)(GLenum);

  extern GG_PFNGLBINDFRAMEBUFFER        gg_glBindFramebuffer;
  extern GG_PFNGLGENFRAMEBUFFERS        gg_glGenFramebuffers;
  extern GG_PFNGLDELETEFRAMEBUFFERS     gg_glDeleteFramebuffers;
  extern GG_PFNGLFRAMEBUFFERTEXTURE2D   gg_glFramebufferTexture2D;
  extern GG_PFNGLCHECKFRAMEBUFFERSTATUS gg_glCheckFramebufferStatus;
  extern GG_PFNGLUSEPROGRAM             gg_glUseProgram;
  extern GG_PFNGLBINDVERTEXARRAY        gg_glBindVertexArray;
  extern GG_PFNGLBINDBUFFER             gg_glBindBuffer;
  extern GG_PFNGLACTIVETEXTURE          gg_glActiveTexture;

  /// Resolve the pointers above. Requires a current GL context. Idempotent;
  /// returns false if any entry point is missing, which means the driver is
  /// too old for projectM and the caller should refuse rather than crash.
  bool groove_gl_load();

#else
  #if defined(__APPLE__)
    #include <OpenGL/gl3.h>
  #else
    // Without this, <GL/glext.h> declares the enums and the function-pointer
    // typedefs but NOT the prototypes, so every post-1.1 call is an
    // undeclared identifier even though libGL exports it. Linux only —
    // defining it is meaningless on the other two.
    #ifndef GL_GLEXT_PROTOTYPES
    #define GL_GLEXT_PROTOTYPES 1
    #endif
    #include <GL/gl.h>
    #include <GL/glext.h>
  #endif

  #define gg_glBindFramebuffer        glBindFramebuffer
  #define gg_glGenFramebuffers        glGenFramebuffers
  #define gg_glDeleteFramebuffers     glDeleteFramebuffers
  #define gg_glFramebufferTexture2D   glFramebufferTexture2D
  #define gg_glCheckFramebufferStatus glCheckFramebufferStatus
  #define gg_glUseProgram             glUseProgram
  #define gg_glBindVertexArray        glBindVertexArray
  #define gg_glBindBuffer             glBindBuffer
  #define gg_glActiveTexture          glActiveTexture

  inline bool groove_gl_load() { return true; }
#endif
