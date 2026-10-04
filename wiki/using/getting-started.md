# Getting started

This page covers installing a release, building from source in brief, what happens on the first launch, and
where the program keeps its settings, logs and pictures. Moving around is on [Controls](controls.md).

## What you need

- A GPU and driver with **Vulkan 1.2**. On macOS, Vulkan runs through MoltenVK, which the release bundles.
- 64-bit Windows, 64-bit Linux, or macOS 11 (Big Sur) or later on Intel or Apple Silicon.
- Roughly 1 to 2 GB of free video memory. The Earth textures alone are several hundred MB of GPU memory, and the
  default roster of about 1.38 million satellites adds about 130 MB. Weak and integrated GPUs work through the
  lower [graphics presets](graphics-settings.md).

## Installing a release

Releases are published on the project's GitHub page as one archive per platform:

| Platform | Archive | How to run |
|---|---|---|
| Windows | `SAT_LIGHT_SIM_v<version>_Windows.zip` | Unzip anywhere and run `SAT_LIGHT_SIM_V_<version>.exe` |
| Linux | `SAT_LIGHT_SIM_v<version>_Linux.tar.gz` | Extract and run `SAT_LIGHT_SIM_V_<version>`; the Vulkan loader comes from your GPU driver package |
| macOS | `SAT_LIGHT_SIM_v<version>_macOS_universal.tar.gz` | Extract and double-click `SAT_LIGHT_SIM_V_<version>.command` |

The executable's name carries the version with underscores, for example `SAT_LIGHT_SIM_V_1_2_0`.

Every archive has the same layout:

```
SAT_LIGHT_SIM_V_<version>[.exe]
constellations.json            the satellite roster (moddable)
constellations.schema.json     field reference for the roster
reflector_targets.json         ground sites for orbital mirrors (moddable)
satellite_models/              one geometry file per satellite type (moddable)
shaders/                       compiled shaders
assets/                        textures, sounds, music, icons
THIRD_PARTY_NOTICES.txt
lib/  + the .command launcher  macOS only: the Vulkan loader and MoltenVK
```

**macOS.** The archive is a universal binary (arm64 and x86_64), so the same download runs on Apple Silicon and
Intel Macs. macOS has no system Vulkan, so the archive carries the Vulkan loader and MoltenVK in `lib/`. Always
start the program through the `.command` launcher: it writes the MoltenVK driver manifest with the folder's
real path and points the dynamic loader at `lib/` before starting the executable. Launching the bare
executable fails to find Vulkan.

The program needs no installation and writes nothing outside its own folder when that folder is writable (see
[Where files live](#where-files-live)).

## Building from source

```bash
cmake -B build -S .
cmake --build build
```

This needs the Vulkan SDK (with `VULKAN_SDK` set), CMake 3.20 or later and a C++20 compiler; all other
libraries are downloaded at configure time. The executable lands in `build/Debug/` (or `build/Release/` with
`--config Release`), with the shaders and data files copied next to it. Presets, packaging and the platform
details are on [Building and releasing](../development/building.md).

## The first launch

1. **Loading screen.** A list of start-up steps appears while textures, noise volumes, satellite models and the
   roster are prepared. A cold start compiles every shader; later launches reuse a pipeline cache and start in a
   few seconds.
2. **Graphics preset.** With no saved settings, the program picks a starting preset from the GPU type: a
   discrete GPU starts on **Medium**, anything else on **Low**.
3. **The intro.** A short cinematic plays, set to the first music track, from the California coast at dusk
   looking at the orbital data-centre disk and the mirrors aimed at a nearby solar farm. Press **Space** (or
   **Start** on a gamepad) to skip it. The intro also measures the GPU frame time. If it plays to the end on
   the Potato, Planetarium, Low or Medium preset, the preset moves one step down when frames are slow (more
   than about 18 ms) or one step up when they are fast (under about 5 ms), never above High, and a notice says
   which preset is now active. A skipped intro changes nothing.
4. **The tutorial.** After the intro, a card at the bottom of the screen walks through looking around, moving,
   climbing, boosting and selecting a satellite. Each step finishes when you do it. Later cards point at the
   selection buttons, the time controls, the picture buttons and the settings gear. **Skip** ends it; with a
   gamepad, **Start** goes to the next card and **View** skips. It runs once; Settings → Display → **Replay
   Tutorial** runs it again.

Whether the intro plays at every launch is the **Play intro on startup** setting (Display tab; on for a new
install). **Replay Intro** plays it once on demand.

!!! note "Crash recovery"
    If the previous session did not exit cleanly (the program keeps a `session.lock` file while it runs), the
    next launch starts on the **Planetarium** preset and shows a notice. Pick your preset again in Settings →
    Display once you know the cause.

## Where files live

The program keeps its own files in a **user data folder**. That is the folder the executable is in, as long as
it is writable. Only when it is not (for example an install under `Program Files` without administrator
rights) does it fall back to the per-user folder of the platform:

| Platform | Fallback user data folder |
|---|---|
| Windows | `%APPDATA%\SatLightSim` |
| Linux | `$XDG_DATA_HOME/SatLightSim`, else `~/.local/share/SatLightSim` |
| macOS | `~/Library/Application Support/SatLightSim` |

What is in it:

| File or folder | Contents |
|---|---|
| `settings.json` | Every setting, the key bindings, the observer's position and the camera. Written when the settings window closes and at exit |
| `satlight_log.txt` | This session's log: start-up steps, model loading, warnings. The previous session's log is kept as `satlight_log.prev.txt` |
| `pipeline_cache.bin` | Compiled shader pipelines, so later launches start fast (next to the exe when writable). Safe to delete |
| `session.lock` | Present while the program runs; see crash recovery above |
| `cinematics/` | Saved cinematics, one JSON file each |
| `traces/` | Magnitude traces exported as CSV |
| `exports/` | Bulk brightness exports (CSV) |
| `perf_profiles/profile_log.jsonl` | Performance snapshots (F9, or Display → Save Snapshot) |
| `satellite_types_resolved.json` | The loaded satellite types in explicit form; see [Modding](../modding/index.md) |
| `satellite_models_debug/` | A 3D shape (OBJ) of every satellite model, for checking a mod |
| `music_analysis/` | A cache of the soundtrack's key analysis |

**Pictures** always go to a `screenshots/` folder next to the executable (or the user data folder if that one
cannot be created):

| What | File |
|---|---|
| Screenshot (F12) | `screenshots/satlight_<date>_<time>.png` |
| HQ photo (F8) | `screenshots/satlight_hq_<date>_<time>.png` |
| Cinematic export | `screenshots/cinematics/<name>_<stamp>/` (one PNG per frame; `_hq` suffix for an HQ export) |

**Resetting.** Settings → Display → **Reset to Defaults** deletes the saved settings; the next launch starts
from the defaults as a first run. Deleting `settings.json` by hand does the same.

!!! warning "Settings from an older version"
    `settings.json` carries a schema version. When it does not match the running program, the photometry,
    cloud and render-scale values are reset to their defaults; key bindings, camera, observer, audio and
    constellation toggles are kept.

## If something goes wrong

- **The window stays white or the program exits at start.** Read `satlight_log.txt`: every start-up step is
  logged before it runs, so the last line names the step that failed. The log also names the GPU and any Vulkan
  limit the GPU is short of.
- **It runs, but slowly.** Pick a lower preset in Settings → Display; see
  [Graphics settings](graphics-settings.md).
- **A modded roster does not show.** See the troubleshooting table in
  [Constellations](../modding/constellations.md#troubleshooting).
