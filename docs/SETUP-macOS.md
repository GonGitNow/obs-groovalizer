# Groovalizer for OBS — macOS setup guide

Groovalizer is an OBS Studio **source** that draws a music visualizer driven by
whatever audio OBS can hear. This guide takes you from the zip you were sent to
visuals reacting to your music. Allow about five minutes.

## What you received

```
Groovalizer-macOS-milkdrop/
├── obs-groovalizer.plugin/   the plugin — this is what you install
├── presets/                  the Groovalizer .milk preset pack (45 presets)
├── SETUP-macOS.md            this guide
└── LICENSE
```

There are two builds of the plugin:

| Build | What it plays |
|---|---|
| **milkdrop** | the 22 built-in scenes **and** MilkDrop `.milk` presets |
| **scenes** | the 22 built-in scenes only |

If you were sent presets, you have the **milkdrop** build. Everything below
applies to both unless it says otherwise.

## Requirements

- macOS 12 Monterey or newer. One download works on Apple Silicon and Intel Macs.
- **OBS Studio 32.x.** This build was compiled against OBS 32.1.2. OBS plugins
  are tied to the OBS major version, so an older or newer major can crash OBS at
  start-up. Check yours under OBS ▸ About OBS.
- Nothing else. The milkdrop build carries its own copy of libprojectM inside
  the plugin.

## 1. Install the plugin

1. Quit OBS if it is running.

2. Double-click the zip to unpack it.

3. In Finder press **⇧⌘G** (Go ▸ Go to Folder…), paste this path and press Return:

        ~/Library/Application Support/obs-studio/plugins

    If Finder says the `plugins` folder does not exist, go one level up to
    `obs-studio` and create a folder named `plugins`.

4. Drag **`obs-groovalizer.plugin`** into that `plugins` folder.

5. macOS flags anything that arrived by download or email as *quarantined* and
    will silently refuse to load it inside OBS. Clear the flag once. Open
    **Terminal** (Applications ▸ Utilities ▸ Terminal), paste the line below and
    press Return:

        xattr -dr com.apple.quarantine ~/Library/Application\ Support/obs-studio/plugins/obs-groovalizer.plugin

    It prints nothing when it works.

6. Start OBS.

To confirm the plugin loaded, open **Help ▸ Log Files ▸ View Current Log** and
search for `groovalizer`. You want a line like:

```
[groovalizer] loaded version 1.0.0
```

## 2. Give OBS something to listen to

A Mac does not let apps hear each other's audio on its own, so OBS needs a
capture source before Groovalizer has anything to react to. Add one of these to
your scene with the **+** button under *Sources*:

- **macOS Audio Capture** — captures a chosen app or the whole desktop. Needs
  OBS 30 or newer on macOS 13 or newer. This is the one to use for Spotify,
  Apple Music, a DAW or a browser.
- **Audio Input Capture** — a microphone or an audio interface, for live
  instruments.
- **Media Source** — if the music is a file that OBS itself is playing.
- A loopback driver such as BlackHole, if you already route audio that way.

Play something and check that the meter for that source moves in the
**Audio Mixer**. If the meter is still, Groovalizer will be still too.

## 3. Add the visualizer

1. Under *Sources* click **+** ▸ **Groovalizer (Music Visualizer)** ▸ OK.
    The Properties window opens.

2. **Audio Source**: pick the capture source from step 2. Leave it on
    *(None — idle animation)* only if you want the visuals to drift on their own
    clock without reacting.

3. Choose what to draw.

    - **Visuals ▸ Built-in scenes** (milkdrop build) or simply the **Scene** list
      (scenes build): pick one of the 22 scenes, then a **Colour Theme**. Choose
      *Custom* to set your own three colours.
    - **Visuals ▸ MilkDrop presets (.milk)** (milkdrop build only): pick a
      **Preset**. The bundled pack is listed as `Groovalizer · <name>`. To add your
      own collection, set **Preset folder** to any folder of `.milk` files (it is
      scanned recursively), close the window and reopen it; your files appear
      under `My presets · <name>`.

4. Click OK. Move and resize the source like any other; it renders at the
    Width and Height set in its properties regardless of your canvas size.

### Settings worth knowing

| Setting | What it does |
|---|---|
| Reactivity, Beat Sensitivity | How strongly the visuals follow level and beats. 1.0 is neutral. |
| Speed | Animation speed. |
| Auto Level | Normalises quiet and loud tracks to the same visual range. On by default; turn it off if you ride the fader yourself. |
| Frequency From / To | Which slice of the spectrum drives the visuals. Slide *To* left for a bass-only look. |
| Trails, Bloom, Vignette | Post-processing on top of the scene. |
| Hue Shift, Saturation, Brightness, Contrast, Exposure | Colour grade. Exposure is the control that makes an overlay bright. |
| Width, Height | Render size, up to 8K. |
| Detail | Noise octaves, 3 to 6. Lower it first if OBS starts dropping frames. |

### Using it as an overlay

To float the visuals over a camera or a game instead of covering the frame,
click **Optimise for Overlay** in the properties. It switches on
*Transparent Background*, removes the vignette and raises exposure in one go.
Then right-click the source ▸ **Blending Mode** ▸ *Additive* or *Screen* for the
brightest result. Raise **Black Point** until any faint haze over your
background disappears.

## 4. The preset pack

The `presets/` folder next to the plugin holds the same 45 `.milk` files that
are built into the milkdrop plugin, so you do not need to do anything with it
for OBS. It is there so you can

- keep a copy somewhere convenient and point **Preset folder** at it, or
- drop the files into any other MilkDrop-compatible player (projectM, Winamp,
  Kodi's MilkDrop visualisation).

The built-in scenes need nothing from this folder.

## Troubleshooting

**Groovalizer is not in the Sources list.** The plugin did not load. Open
Help ▸ Log Files ▸ View Current Log and search for `obs-groovalizer`.

- A message mentioning *code signature*, *System Policy* or an *unidentified
  developer* means the quarantine flag is still set. Repeat step 1.5 exactly,
  including the backslash before `Support`.
- *built for macOS 12.0* or similar with *newer than running OS* means your
  macOS is older than 12. There is no build for it.
- Nothing about the plugin at all: check it landed in the `plugins` folder
  under `obs-studio`, not in `obs-studio` itself, and that it is still named
  `obs-groovalizer.plugin`.

**OBS crashes at start-up since installing.** Almost always an OBS version
mismatch. Remove `obs-groovalizer.plugin` from the plugins folder, start OBS,
update it to 32.x, and put the plugin back.

**The visuals move but do not react to the music.** Audio Source is set to
*None*, or the source you chose is silent. Confirm its Audio Mixer meter moves.

**The Preset list says "(enable MilkDrop and reopen this window)".** Set
*Visuals* to *MilkDrop presets (.milk)*, click OK, then open Properties again.
The list fills on the second visit.

**The log says "libprojectM could not start".** OBS is not using its OpenGL
renderer. On macOS it is the default, so this usually means a settings change;
check Settings ▸ Advanced.

**OBS drops frames with the visualizer on.** Lower *Detail* first, then the
render *Width* and *Height*. Trails and Bloom cost the least.

## Uninstall

Quit OBS and delete `obs-groovalizer.plugin` from
`~/Library/Application Support/obs-studio/plugins`. Nothing else is written
anywhere.
