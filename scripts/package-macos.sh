#!/usr/bin/env bash
# Turn a finished CMake build into a distributable macOS package.
#
#   scripts/package-macos.sh <build-dir> <scenes|milkdrop> <out-dir> [libprojectM-prefix]
#
# What the zip contains, and why:
#
#   Groovalizer-macOS-<variant>/
#     obs-groovalizer.plugin/   binary + Info.plist from the build, plus data/
#                               copied into Contents/Resources — that is where
#                               obs_module_file() looks for a .plugin bundle,
#                               and a bare CMake build does not put it there.
#                               milkdrop: libprojectM in Contents/Frameworks,
#                               with the binary's reference rewritten to find
#                               it there instead of on the build machine.
#     presets/                  the bundled .milk pack, loose, for people who
#                               want to point "Preset folder" at it or use it
#                               in another MilkDrop player.
#     SETUP-macOS.md, LICENSE
#
# Signing is ad-hoc: enough for a local install once the recipient clears the
# quarantine flag (the guide covers it), not notarised. Real signing needs a
# Developer ID and belongs in CI secrets, not here.
#
# Run by .github/workflows/build.yml on a macOS runner. Works from any Mac with
# a completed build too.
set -euo pipefail

BUILD_DIR="${1:?usage: package-macos.sh <build-dir> <scenes|milkdrop> <out-dir> [libprojectM-prefix]}"
VARIANT="${2:?variant: scenes or milkdrop}"
OUT_DIR="${3:?output directory}"
PM_PREFIX="${4:-}"

case "$VARIANT" in scenes|milkdrop) ;; *) echo "unknown variant: $VARIANT" >&2; exit 2;; esac

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# buildspec.json has three "version" keys; only the top-level one is ours.
VERSION="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["version"])' "$ROOT/buildspec.json")"

STAGE_NAME="Groovalizer-macOS-$VARIANT"
STAGE="$OUT_DIR/$STAGE_NAME"
BUNDLE="$STAGE/obs-groovalizer.plugin"
BIN="$BUNDLE/Contents/MacOS/obs-groovalizer"

rm -rf "$STAGE"
mkdir -p "$STAGE"
cp -R "$BUILD_DIR/obs-groovalizer.plugin" "$BUNDLE"
[[ -f "$BIN" ]] || { echo "no plugin binary at $BIN — did the build finish?" >&2; exit 1; }

# ── Data ────────────────────────────────────────────────────────────────────
mkdir -p "$BUNDLE/Contents/Resources"
cp -R "$ROOT/data/." "$BUNDLE/Contents/Resources/"

# ── libprojectM (milkdrop only) ─────────────────────────────────────────────
if [[ "$VARIANT" == "milkdrop" ]]; then
  [[ -n "$PM_PREFIX" && -d "$PM_PREFIX/lib" ]] \
    || { echo "milkdrop needs the libprojectM install prefix as the 4th argument" >&2; exit 1; }

  # Whatever the binary actually links against. The versioned filename is
  # libprojectM's decision, not ours, so read it rather than hardcode it.
  ref="$(otool -L "$BIN" | awk '/libprojectM/ { print $1; exit }')"
  [[ -n "$ref" ]] || { echo "binary does not link libprojectM; was ENABLE_MILKDROP on?" >&2; exit 1; }
  lib="$(basename "$ref")"

  mkdir -p "$BUNDLE/Contents/Frameworks"
  cp -L "$PM_PREFIX/lib/$lib" "$BUNDLE/Contents/Frameworks/$lib"

  case "$ref" in
    @rpath/*)
      # Resolved through the rpath list: add ours, drop the build machine's.
      install_name_tool -add_rpath "@loader_path/../Frameworks" "$BIN"
      install_name_tool -delete_rpath "$PM_PREFIX/lib" "$BIN" 2>/dev/null || true
      ;;
    *)
      # Absolute path baked in: point it at the bundled copy directly.
      install_name_tool -change "$ref" "@loader_path/../Frameworks/$lib" "$BIN"
      ;;
  esac
  install_name_tool -id "@rpath/$lib" "$BUNDLE/Contents/Frameworks/$lib"
  codesign --force --sign - "$BUNDLE/Contents/Frameworks/$lib"
fi

# ── Sign ────────────────────────────────────────────────────────────────────
codesign --force --sign - "$BUNDLE"
codesign --verify --verbose=2 "$BUNDLE"

# ── Companions ──────────────────────────────────────────────────────────────
cp "$ROOT/docs/SETUP-macOS.md" "$STAGE/SETUP-macOS.md"
cp "$ROOT/LICENSE" "$STAGE/LICENSE"
mkdir -p "$STAGE/presets"
cp "$ROOT"/data/presets/*.milk "$STAGE/presets/"

# ── Verify before zipping ───────────────────────────────────────────────────
# Each of these has been the thing that was wrong at least once in some plugin.
echo "── architectures";  lipo -info "$BIN"
echo "── linked libraries"; otool -L "$BIN"
echo "── rpaths";         otool -l "$BIN" | awk '/LC_RPATH/ { getline; getline; print $2 }'
echo "── Info.plist";     plutil -lint "$BUNDLE/Contents/Info.plist"
for f in shaders/post.effect locale/en-US.ini presets; do
  [[ -e "$BUNDLE/Contents/Resources/$f" ]] || { echo "missing Resources/$f" >&2; exit 1; }
done
if [[ "$VARIANT" == "milkdrop" ]]; then
  otool -L "$BIN" | grep -q '@\(loader_path\|rpath\)/.*libprojectM' \
    || { echo "libprojectM reference still points outside the bundle" >&2; exit 1; }
  if otool -l "$BIN" | grep -A2 LC_RPATH | grep -q "$PM_PREFIX"; then
    echo "build-machine rpath survived: $PM_PREFIX" >&2; exit 1
  fi
fi

# ── Zip ─────────────────────────────────────────────────────────────────────
# ditto rather than zip: it keeps the bundle's symlinks and extended attributes
# intact, which is what Finder expects to unpack.
ZIP_NAME="obs-groovalizer-$VERSION-macos-$VARIANT.zip"
rm -f "$OUT_DIR/$ZIP_NAME"
(cd "$OUT_DIR" && ditto -c -k --sequesterRsrc --keepParent "$STAGE_NAME" "$ZIP_NAME")

echo "── package"
ls -la "$OUT_DIR/$ZIP_NAME"
(cd "$STAGE" && find . -type f | sort)
