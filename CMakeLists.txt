cmake_minimum_required(VERSION 3.16)
project(obs-groovalizer VERSION 1.0.0 LANGUAGES CXX)

# Standalone build — deliberately does NOT depend on obs-plugintemplate's
# cmake/common/bootstrap.cmake, so a fresh clone builds with nothing but CMake
# and an OBS development package. If you want the template's signing/packaging
# CI, drop this file's contents into a template checkout instead.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(ENABLE_MILKDROP "Build the libprojectM .milk backend (experimental)" OFF)

# Compile every translation unit but link nothing, needing only headers.
#
# This exists for CI on Windows. A real link there requires libobs.lib, which
# means building obs-studio and its dependency tree — a long job for a check
# whose entire purpose is to answer "does the Windows code still compile?".
# The Windows GL bridge cannot be built or run on the machine it was written
# on, so cheap, fast, every-push compile coverage is the difference between
# knowing it is syntactically sound and hoping so.
#
# It verifies compilation ONLY. It does not prove the plugin links, loads,
# or renders anything.
option(GROOVE_COMPILE_ONLY "Compile without linking (CI syntax checking)" OFF)

if(GROOVE_COMPILE_ONLY)
  add_library(obs-groovalizer OBJECT
    src/plugin-main.cpp
    src/groove-source.cpp
  )
else()
  add_library(obs-groovalizer MODULE
    src/plugin-main.cpp
    src/groove-source.cpp
  )
endif()
target_include_directories(obs-groovalizer PRIVATE src)
target_compile_definitions(obs-groovalizer PRIVATE PLUGIN_VERSION="${PROJECT_VERSION}")

# ── libobs ───────────────────────────────────────────────────────────────────
# Preferred: a real libobs CMake package (installed by obs-studio, Homebrew,
# vcpkg, or the Linux -dev packages).
find_package(libobs QUIET)

if(GROOVE_COMPILE_ONLY)
  # Headers only, no link, any platform. OBS_SOURCE_DIR must point at an
  # obs-studio checkout whose tag matches the OBS you target.
  if(NOT DEFINED ENV{OBS_SOURCE_DIR})
    message(FATAL_ERROR "GROOVE_COMPILE_ONLY needs OBS_SOURCE_DIR set to an obs-studio checkout")
  endif()
  set(OBS_SOURCE_DIR $ENV{OBS_SOURCE_DIR})
  target_include_directories(obs-groovalizer PRIVATE
    ${OBS_SOURCE_DIR}/libobs ${CMAKE_CURRENT_BINARY_DIR}/obsconfig)
  # libobs headers reach for <simde/...> on the SIMD paths. Optional here:
  # where it is absent the few affected headers still parse.
  find_path(SIMDE_INCLUDE_DIR simde/x86/sse2.h
    PATHS /opt/homebrew/include /usr/local/include /usr/include)
  if(SIMDE_INCLUDE_DIR)
    target_include_directories(obs-groovalizer PRIVATE ${SIMDE_INCLUDE_DIR})
  endif()
  file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/obsconfig)
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/obsconfig/obsconfig.h
    "#pragma once\n#define OBS_VERSION \"32.1.2\"\n")
  set(LIBOBS_FOUND TRUE)
elseif(TARGET OBS::libobs)
  target_link_libraries(obs-groovalizer PRIVATE OBS::libobs)
else()
  # Fallback: pkg-config, which is how most Linux distributions ship it.
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(LIBOBS QUIET libobs)
  endif()

  # macOS: a released OBS.app ships no CMake package and no pkg-config file,
  # only the framework. Plugins there are loaded into the OBS process, so the
  # idiomatic build links nothing and resolves libobs symbols at load time
  # (-undefined dynamic_lookup). Headers come from a matching obs-studio
  # checkout — point OBS_SOURCE_DIR at one whose tag matches your OBS build, or
  # the struct layouts will not agree and the plugin will crash on load.
  if(APPLE AND DEFINED ENV{OBS_SOURCE_DIR} AND NOT LIBOBS_FOUND)
    set(OBS_SOURCE_DIR $ENV{OBS_SOURCE_DIR})
    message(STATUS "libobs headers from ${OBS_SOURCE_DIR}/libobs (dynamic lookup)")
    # libobs headers include <simde/...> for its SSE shims.
    find_path(SIMDE_INCLUDE_DIR simde/x86/sse2.h
      PATHS /opt/homebrew/include /usr/local/include
            /opt/homebrew/Cellar/simde/*/include)
    if(NOT SIMDE_INCLUDE_DIR)
      message(FATAL_ERROR "simde headers not found — brew install simde")
    endif()
    target_include_directories(obs-groovalizer PRIVATE
      ${OBS_SOURCE_DIR}/libobs ${CMAKE_CURRENT_BINARY_DIR}/obsconfig
      ${SIMDE_INCLUDE_DIR})
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/obsconfig)
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/obsconfig/obsconfig.h
      "#pragma once\n#define OBS_VERSION \"32.1.2\"\n")
    target_link_options(obs-groovalizer PRIVATE -undefined dynamic_lookup)
    set(LIBOBS_FOUND TRUE)
  endif()

  if(LIBOBS_FOUND)
    target_include_directories(obs-groovalizer PRIVATE ${LIBOBS_INCLUDE_DIRS})
    target_link_libraries(obs-groovalizer PRIVATE ${LIBOBS_LIBRARIES})
    target_link_directories(obs-groovalizer PRIVATE ${LIBOBS_LIBRARY_DIRS})
  else()
    message(FATAL_ERROR
      "libobs not found.\n"
      "  Linux   : install obs-studio development headers (e.g. libobs-dev)\n"
      "  macOS   : brew install obs  — or point CMAKE_PREFIX_PATH at an OBS build\n"
      "  Windows : -DCMAKE_PREFIX_PATH=<obs-studio-build>/libobs\n"
      "Alternatively set OBS_SOURCE_DIR to an obs-studio checkout.")
  endif()
endif()

# ── Optional .milk backend ───────────────────────────────────────────────────
# libprojectM gives full MilkDrop preset compatibility. Off by default: it adds
# roughly 12 MB of shader-transpiler binary and only pays for itself for users
# who already own a .milk library.
if(ENABLE_MILKDROP)
  if(GROOVE_COMPILE_ONLY)
    # Headers are enough to typecheck against. PROJECTM_INCLUDE_DIR should
    # point at <projectm-checkout>/src/api/include, which needs no build.
    if(NOT DEFINED PROJECTM_INCLUDE_DIR)
      message(FATAL_ERROR "compile-only + ENABLE_MILKDROP needs -DPROJECTM_INCLUDE_DIR=<projectm>/src/api/include")
    endif()
    target_include_directories(obs-groovalizer PRIVATE ${PROJECTM_INCLUDE_DIR})
  else()
    # Package is `projectM4`; the imported target is `libprojectM::projectM`.
    find_package(projectM4 REQUIRED)
    target_link_libraries(obs-groovalizer PRIVATE libprojectM::projectM)

    # This backend needs projectm_opengl_render_frame_fbo(), which renders into
    # a framebuffer we supply. It exists ONLY on libprojectM master — every
    # tagged release up to and including 4.1.4 has just
    # projectm_opengl_render_frame(), whose implementation ends with a
    # hardcoded glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0). That draws over
    # OBS's output instead of into our texture, and no amount of binding on our
    # side redirects it, so there is no compatibility shim to write.
    #
    # Checked here because the failure is otherwise an "undeclared identifier"
    # two hundred lines into a C++ file, which tells the builder nothing about
    # what they actually need to do.
    get_target_property(_pm_inc libprojectM::projectM INTERFACE_INCLUDE_DIRECTORIES)
    find_file(PROJECTM_RENDER_HEADER projectM-4/render_opengl.h PATHS ${_pm_inc} NO_DEFAULT_PATH)
    if(PROJECTM_RENDER_HEADER)
      file(READ ${PROJECTM_RENDER_HEADER} _pm_render_h)
      string(FIND "${_pm_render_h}" "projectm_opengl_render_frame_fbo" _pm_has_fbo)
      if(_pm_has_fbo EQUAL -1)
        message(FATAL_ERROR
          "This libprojectM is too old for the MilkDrop backend.\n"
          "  Found   : ${PROJECTM_RENDER_HEADER}\n"
          "  Missing : projectm_opengl_render_frame_fbo()\n"
          "\n"
          "Released libprojectM (<= 4.1.4, and everything in Homebrew, apt and\n"
          "vcpkg) can only render to the default framebuffer, which would draw\n"
          "over OBS rather than into the plugin's texture.\n"
          "\n"
          "Build libprojectM from master instead, or configure with\n"
          "-DENABLE_MILKDROP=OFF to build the generated scenes only.")
      endif()
    endif()
  endif()
  target_sources(obs-groovalizer PRIVATE
    src/groove-milk-backend.cpp
    src/groove-gl.cpp
    src/groove-gl-bridge-native.cpp
    src/groove-gl-bridge-win.cpp
  )
  # Both bridge files are listed on every platform, but each guards its whole
  # body behind an #if — so the Windows bridge compiles to an empty object on
  # macOS and Linux and gets NO syntax checking from those jobs. Only a real
  # Windows compiler tells you whether that file is sound, which is why the CI
  # matrix has a Windows entry at all.
  target_compile_definitions(obs-groovalizer PRIVATE GROOVE_MILKDROP=1)

  # projectM renders through OpenGL, so the plugin needs GL itself for the
  # bridge. macOS gets it from the OpenGL framework (deprecated since 10.14 but
  # entirely functional); Windows from opengl32; Linux from libGL via the
  # standard find module.
  if(APPLE)
    target_link_libraries(obs-groovalizer PRIVATE "-framework OpenGL")
    # Silence the deprecation wall from <OpenGL/gl3.h>. The alternative is
    # Metal, which projectM does not speak.
    target_compile_definitions(obs-groovalizer PRIVATE GL_SILENCE_DEPRECATION=1)
  elseif(WIN32)
    target_link_libraries(obs-groovalizer PRIVATE opengl32)
  else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(obs-groovalizer PRIVATE OpenGL::GL)
  endif()
endif()

# Everything past this point shapes a loadable module — naming, bundling,
# installing, the deploy helper. None of it applies to the compile-only CI
# target, which produces object files and stops.
if(GROOVE_COMPILE_ONLY)
  message(STATUS "compile-only: no link, no install, no bundle")
  return()
endif()

# ── Naming ───────────────────────────────────────────────────────────────────
# OBS loads plugins by bare filename with no "lib" prefix on any platform.
set_target_properties(obs-groovalizer PROPERTIES PREFIX "")

if(APPLE)
  set_target_properties(obs-groovalizer PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION "plugin"
    MACOSX_BUNDLE_BUNDLE_NAME "Groovalizer"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.groovalizer.obs-groovalizer"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
endif()

# ── Install ──────────────────────────────────────────────────────────────────
if(NOT DEFINED OBS_PLUGIN_DESTINATION)
  if(WIN32)
    set(OBS_PLUGIN_DESTINATION "obs-plugins/64bit")
    set(OBS_DATA_DESTINATION "data/obs-plugins/obs-groovalizer")
  elseif(APPLE)
    set(OBS_PLUGIN_DESTINATION ".")
    set(OBS_DATA_DESTINATION "obs-groovalizer.plugin/Contents/Resources")
  else()
    set(OBS_PLUGIN_DESTINATION "lib/obs-plugins")
    set(OBS_DATA_DESTINATION "share/obs/obs-plugins/obs-groovalizer")
  endif()
endif()

install(TARGETS obs-groovalizer
        LIBRARY DESTINATION "${OBS_PLUGIN_DESTINATION}"
        BUNDLE  DESTINATION "${OBS_PLUGIN_DESTINATION}")

install(DIRECTORY data/ DESTINATION "${OBS_DATA_DESTINATION}")

# Convenience target: copy straight into the user's OBS plugin folder.
if(APPLE)
  set(_user_plugins "$ENV{HOME}/Library/Application Support/obs-studio/plugins")
elseif(UNIX)
  set(_user_plugins "$ENV{HOME}/.config/obs-studio/plugins")
else()
  set(_user_plugins "$ENV{APPDATA}/obs-studio/plugins")
endif()

add_custom_target(install-user
  COMMAND ${CMAKE_COMMAND} -E make_directory "${_user_plugins}/obs-groovalizer/bin"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:obs-groovalizer> "${_user_plugins}/obs-groovalizer/bin/"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_CURRENT_SOURCE_DIR}/data" "${_user_plugins}/obs-groovalizer/data"
  DEPENDS obs-groovalizer
  COMMENT "Installing Groovalizer into ${_user_plugins}")
