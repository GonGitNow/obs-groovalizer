# Groovalizer for OBS Studio

A native OBS **source** that renders a GrooveCore scene driven by any audio source
in your scene collection. Add it like you would a Colour Source; point it at your
Desktop Audio; it reacts.

## Why not just use a browser source?

A browser source running the Groovalizer web app costs an entire CEF process —
around 250 MB of RSS and 15–25 % of a core at 1080p60. This plugin does the same
job through libobs' own graphics abstraction at roughly 2 %, works identically on
the D3D11 and OpenGL backends, and has no window to lose focus or throttle.

## What streamers get

Shaped by what the existing OBS visualizer plugins and their forum threads are
repeatedly asked for:

- **Bind to any audio source** — Desktop Audio, a mic, a specific media source, a
  VST-fed input. Anything with `OBS_SOURCE_AUDIO`.
- **MilkDrop `.milk` presets** via libprojectM — point the source at a folder and
  play a preset library you already own. See the build note below: this needs
  libprojectM built from master, and on Windows it is unverified.
- **22 built-in scenes**, all slow, organic and feedback-driven — full-frame
  ambient looks for backgrounds and BRB screens rather than reactive bar meters.
- **Frequency window** — a From/To pair over the 30 Hz – 16 kHz range, so you can
  drive the visuals from bass only or the full spectrum. It lives in the shared
  `spectrum()` shader helper, so every scene inherits it.
- **Twelve colour themes** plus a Custom mode with three colour pickers, and a
  full hue / saturation / brightness / contrast grade on top.
- **Sensitivity, smoothing and decay** exposed directly: Reactivity, Beat
  Sensitivity, Speed, Trails.
- **Auto Level** — continuously normalises quiet and loud tracks to the same
  visual range so the visualizer does not die during a quiet intro. Switchable off
  if you are riding the fader yourself.
- **Any output size**, up to 8K, independent of canvas size.
- **Detail slider** (noise octaves 3–6) to trade GPU time for richness.
- **Idle animation** when nothing is playing — scenes keep moving on their own
  clock rather than freezing, which reads as a crash on a BRB screen.

## Building

Standalone CMake. Deliberately does *not* require the `obs-plugintemplate`
checkout, so a fresh clone builds with nothing but CMake and an OBS dev package.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --config RelWithDebInfo
cmake --build build --target install-user   # copies into your OBS plugin folder
```

Finding libobs:

| Platform | How |
|---|---|
| Linux | `apt install libobs-dev` (or your distro's equivalent) — found via pkg-config |
| macOS | `brew install obs`, or `-DCMAKE_PREFIX_PATH=<obs-build>` |
| Windows | `-DCMAKE_PREFIX_PATH=<obs-studio-build>/libobs` |

`install-user` drops the module into:

- macOS — `~/Library/Application Support/obs-studio/plugins/obs-groovalizer/`
- Linux — `~/.config/obs-studio/plugins/obs-groovalizer/`
- Windows — `%APPDATA%/obs-studio/plugins/obs-groovalizer/`

### Distributing a build

CI produces the thing you hand to someone. Every push to `main`, and any
`workflow_dispatch` run, uploads two artifacts from `.github/workflows/build.yml`:

| Artifact | Contents |
|---|---|
| `obs-groovalizer-macos-scenes` | `obs-groovalizer.plugin` with the 22 scenes, universal, ad-hoc signed |
| `obs-groovalizer-macos-milkdrop` | the same plus the `.milk` backend, with libprojectM (built from master) bundled inside |

Each zip also carries the preset pack as a loose `presets/` folder and
[`docs/SETUP-macOS.md`](docs/SETUP-macOS.md), written for the person receiving
it. Pushing a `v*` tag attaches both zips to a GitHub release.

The assembly is `scripts/package-macos.sh`, which also runs from any Mac with a
finished build. Packages are ad-hoc signed, not notarised, so the recipient
clears the quarantine flag once; the guide has the command.

### Build status, honestly

| Configuration | State |
|---|---|
| macOS, scenes only | Built, installed, **run in OBS** |
| macOS, `.milk` backend | Built, installed, **run in OBS** |
| Linux, either | Compiles in CI. Never linked or run |
| Windows, scenes only | Compiles in CI. Never linked or run |
| Windows, `.milk` backend | Compiles in CI. **Never executed a single frame** |

The Windows MilkDrop path is the one to be suspicious of. OBS uses Direct3D 11
there while projectM renders with OpenGL, so `src/groove-gl-bridge-win.cpp`
stands up a private WGL context and shares a texture between the two APIs. All
of it was written on a Mac against documentation.

**Before any Windows release**, someone must confirm on real hardware that:

1. a `.milk` preset renders at all;
2. the log line `Windows GL bridge up, transport: …` reports
   `shared texture (WGL_NV_DX_interop2)` rather than `readback` — both should
   work, but silently landing on readback means every user pays a full
   GPU→CPU→GPU round trip per frame;
3. other sources in the scene still render correctly with the source active;
4. quitting OBS with the source live does not crash.

Green CI means it compiles. It does not mean any of the above.

Two effect-parser hazards were found and fixed pre-emptively by reading
`libobs/graphics/effect-parser.c`:

- **No function overloading.** `ep_getfunc` matches by name and returns the first
  hit, so a second `gmod` taking `float2` would never be reached and every vector
  call would silently bind to the scalar body. The vector modulo is `gmod2`.
- **Local array declarations** in `blur9` were unrolled rather than trusted.

## How it works

Per frame, inside `video_render`:

1. **Scene** → `GS_RGBA16F` texrender. Scenes deliberately return values above 1.0
   so the bright pass has something to find; an 8-bit target would clip the
   highlights away before bloom ever saw them.
2. **Bright pass** at quarter resolution, then a separable 9-tap Gaussian.
3. **Composite** into a history ping-pong. Trails are a *max* blend against the
   previous frame, not a lerp — lerp averages toward grey and kills the neon look.
4. **Blit** the result to the source's output.

Audio arrives on the audio thread via `obs_source_add_audio_capture_callback`,
lands in a ring buffer under a mutex, and is analysed once per rendered frame on
the graphics thread.

## The `.milk` backend

`-DENABLE_MILKDROP=ON` builds against libprojectM 4 for full MilkDrop preset
compatibility. Off by default: it adds roughly 12 MB of shader-transpiler binary
and only pays for itself for users who already own a `.milk` library.

**Licensing:** the default build contains no projectM code and carries no LGPL
obligations. A `-DENABLE_MILKDROP=ON` build links libprojectM and its terms apply
to *that build only*. Keep the two clearly separated in any release process.

## Selling it

The plugin is free — that is the distribution channel. The product is **scene
packs**: streamers do not buy software, they buy looks. Adding a pack is dropping
`.gsl` files into `shared/groove-spec/scenes/`, running the generator, and shipping
the `.effect` files; no C++ changes.

A Pro key gating the pack loader is a straightforward addition; the scene catalog
in `groove-source.cpp` already carries a tier concept mirroring the mobile builds.
