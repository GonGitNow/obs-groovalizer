#!/usr/bin/env bash
# Build and install the plugin into the user's OBS plugin directory.
#
#   OBS_SOURCE_DIR=/path/to/obs-studio ./install-macos.sh [--milk /path/to/projectm-install]
#
# OBS.app ships no CMake package, so headers come from an obs-studio checkout
# whose tag MUST match the installed OBS — mismatched struct layouts crash on
# load rather than failing to build.
set -euo pipefail

PLUG="$HOME/Library/Application Support/obs-studio/plugins/obs-groovalizer.plugin"
MILK_ARGS=()
PM_PREFIX=""
if [[ "${1:-}" == "--milk" ]]; then
  PM_PREFIX="${2:?--milk needs a libprojectM install prefix}"
  MILK_ARGS=(-DENABLE_MILKDROP=ON "-DCMAKE_PREFIX_PATH=$PM_PREFIX")
fi

: "${OBS_SOURCE_DIR:?set OBS_SOURCE_DIR to an obs-studio checkout matching your OBS version}"
export OBS_SOURCE_DIR

cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 "${MILK_ARGS[@]}"
cmake --build build

rm -rf "$PLUG"
mkdir -p "$PLUG/Contents/MacOS" "$PLUG/Contents/Resources" "$PLUG/Contents/Frameworks"
cp build/obs-groovalizer.plugin/Contents/MacOS/obs-groovalizer "$PLUG/Contents/MacOS/"
cp build/obs-groovalizer.plugin/Contents/Info.plist "$PLUG/Contents/" 2>/dev/null || true
cp -R data/shaders data/locale "$PLUG/Contents/Resources/"
[[ -d data/presets ]] && cp -R data/presets "$PLUG/Contents/Resources/"

if [[ -n "$PM_PREFIX" ]]; then
  # Ship libprojectM inside the bundle. The link-time rpath points at the build
  # prefix, which may be temporary — replace it with one relative to the plugin.
  cp "$PM_PREFIX"/lib/libprojectM-4.4.dylib "$PLUG/Contents/Frameworks/"
  install_name_tool -add_rpath "@loader_path/../Frameworks" "$PLUG/Contents/MacOS/obs-groovalizer" 2>/dev/null || true
  install_name_tool -delete_rpath "$PM_PREFIX/lib" "$PLUG/Contents/MacOS/obs-groovalizer" 2>/dev/null || true
  codesign --force -s - "$PLUG/Contents/Frameworks/libprojectM-4.4.dylib"
fi

codesign --force -s - "$PLUG"
echo "installed -> $PLUG"
echo "restart OBS to load it"
