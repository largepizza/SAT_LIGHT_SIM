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

1. **Graphics mode.** Before anything heavy loads, the loading screen asks for a graphics mode (see
   [Choosing a graphics mode at startup](graphics-settings.md#choosing-a-graphics-mode-at-startup)): **Full
   graphics**, **Planetarium** (no clouds) or **Potato (very old hardware)**. On a first run the choice the GPU
   suits is pre-selected and marked *Recommended for this computer*: Full graphics for a discrete GPU with
   more than about 2 GB of memory, Planetarium for a smaller discrete GPU, integrated graphics or a software or
   virtual device, and Potato for a non-Apple GPU on macOS. Later launches pre-select the mode you ran last. Pick one with the mouse, the arrow keys
   and Enter, or the d-pad and **A**; the screen waits until you do.
2. **Loading screen.** A list of start-up steps appears while textures, noise volumes, satellite models and the
   roster are prepared. A cold start compiles every shader; later launches reuse a pipeline cache and start in a
   few seconds. If the first frames after loading are very slow (over 250 ms each), the program drops one tier
   (Full to Planetarium, Planetarium to Potato) and says so.
3. **The intro.** A short cinematic plays, set to the first music track, from the California coast at dusk
   looking at the orbital data-centre satellites and the mirrors aimed at a nearby solar farm. Press **Space** (or
   **Start** on a gamepad) to skip it. When the graphics mode was not picked on the startup screen (the question
   is switched off), the intro also measures the GPU frame time, and if it plays to the end the preset moves one
   step when the measurement calls for it: down when frames are slow (more than about 18 ms: Medium to Low, Low
   to Planetarium, Planetarium to Potato), up when they are fast (under about 5 ms: Potato to Planetarium, Low to
   Medium, Medium to High). A notice then says which preset is active. A skipped intro changes nothing.
4. **The tutorial.** After the intro, a card at the bottom of the screen walks through looking around, moving,
   climbing, boosting and selecting a satellite. Each of these steps finishes when you do it, and a picture of the
   keyboard, mouse or gamepad lights the input to use. Later cards outline the selection buttons, the time
   controls, the picture buttons and the menu buttons (Bookmarks, Cinematics, Settings) on the screen. When the
   program is on Planetarium or Potato, a last **Graphics** card says that there are no clouds and that **Low**
   (Settings → Display → Preset) brings them and every other effect back at half resolution. **Skip** ends the
   tutorial; with a gamepad, **Start** goes to the next card and **View** skips. It runs once; Settings →
   Display → **Replay tutorial** runs it again.

Whether the intro plays at every launch is the **Play intro on startup** setting (Display tab; on for a new
install). **Replay intro** plays it once on demand.

!!! note "Crash recovery"
    The program keeps a `session.lock` file while it runs. If it is still there at the next launch, the previous
    session did not exit cleanly: the graphics-mode screen then appears even when the question is switched off,
    with the mode one tier lighter than last time pre-selected and a note saying why.

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
| `perf_profiles/profile_log.jsonl` | Performance snapshots (F9, or Settings → Performance → Save snapshot) |
| `bookmarks/` | Saved bookmarks (`bookmarks.json`) and their thumbnails. A first run fills it with the [bookmarks that ship with the sim](features.md#the-bookmarks-that-ship-with-the-sim) |
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

**Resetting.** Settings → Display → **Reset to defaults** deletes the saved settings; the next launch starts
from the defaults as a first run. Deleting `settings.json` by hand does the same.

!!! warning "Settings from an older version"
    `settings.json` carries a schema version. When it does not match the running program, the photometry,
    cloud and render-scale values are reset to their defaults; key bindings, camera, observer, audio and
    constellation toggles are kept.

## If something goes wrong

- **The window stays white or the program exits at start.** Read `satlight_log.txt`: every start-up step is
  logged before it runs, so the last line names the step that failed. The log also names the GPU and any Vulkan
  limit the GPU is short of.
- **It runs, but slowly.** Pick a lower preset in Settings → Display, or turn on **Automatic render scale**
  there; see [Graphics settings](graphics-settings.md).
- **A modded roster does not show.** See the troubleshooting table in
  [Constellations](../modding/constellations.md#troubleshooting).
