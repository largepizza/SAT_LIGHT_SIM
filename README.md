# SAT LIGHT SIM

A real-time GPU visualization of Earth's satellite megaconstellations, seen from any point on the
surface — physically-based photometry, atmospheric scattering, volumetric clouds, terrain, ocean,
aurora, and a real star catalog, all rendered through a Vulkan compute + graphics pipeline.

v1.0 was a satellite simulator. v1.1 is a full atmospheric/terrain renderer that happens to
contain a satellite simulator.

---

## Screenshots

<!--
  Drop images here, e.g.:
  ![Twilight over the terminator](docs/screenshots/twilight.png)
  ![Aurora oval from orbit](docs/screenshots/aurora.png)
  ![Reflect Orbital mirrors over a solar farm](docs/screenshots/mirrors.png)
-->



---

## Prerequisites

| Dependency | Notes |
|------------|-------|
| [Vulkan SDK](https://vulkan.lunarg.com/) | Sets `VULKAN_SDK` env var |
| CMake 3.20+ | `winget install Kitware.CMake` |
| Visual Studio 2022 | C++20 + MSBuild |

GLFW, GLM, Clay (UI), stb (fonts/images), miniaudio, and nlohmann/json are fetched automatically at
configure time via CMake FetchContent.

---

## Build

```bash
cmake -B build -S .
cmake --build build
```

Or open the folder in **VS Code** with the CMake Tools extension — **F5** to build + debug, **F7**
to build only.

Shaders are auto-detected by CMake, compiled by `glslc`, and copied next to the executable. No
manual shader step needed. The built executable is named `SAT_LIGHT_SIM_V_<version>` (tracks the
`VERSION` file), e.g. `build/Debug/SAT_LIGHT_SIM_V_1_1_0.exe`.

---


## Constellations

Fully moddable via `constellations.json` next to the executable — see `CONSTELLATION_MODDING.md`.

---

## Project structure

```
src/
├── main.cpp                    ← pick simulation here (one line)
├── App.h / App.cpp             ← window + frame loop
├── VulkanContext.h / .cpp      ← Vulkan boilerplate + helpers
├── Simulation.h                ← abstract base class
├── UIRenderer.h / .cpp         ← Clay UI → Vulkan pipeline
├── AudioSystem.h / .cpp        ← miniaudio wrapper
└── simulations/
    ├── SatelliteSim.h/.cpp/SatelliteSimUI.cpp  ← primary simulation (this project)
    ├── StarCatalog.h / .cpp    ← star catalog renderer (precursor, legacy)
    ├── GameOfLife.h / .cpp     ← Conway's Game of Life (legacy)
    ├── Particles.h / .cpp      ← GPU particle system (legacy)
    └── Scene3DDemo.h / .cpp    ← 3D mesh + SDF rendering (legacy)
shaders/
    sat_orbit.comp               ← GPU orbital mechanics + attitude
    sat_flare.comp               ← photometry compute: visibility + glow histogram
    scene_depth.comp             ← shared terrain/ocean depth buffer
    cloud_march.comp             ← half-res volumetric clouds/cirrus/aurora/airglow
    beam_cloud_block.comp        ← Reflect Orbital target illumination
    sat_point.vert/frag          ← satellite point sprites (additive blend)
    sat_sky.vert/frag            ← sky background: atmosphere + terrain + ocean + sun + moon
    star_point.vert/frag         ← star catalog + planet points
    flare_source/blur/composite  ← render-to-texture lens flare pipeline
    ui.vert/frag                 ← Clay UI quads + text + icons
    include/                     ← shared GLSL headers (common/terrain/cloud_params/reflect_beam)
data/
    constellations.json           ← satellite types + constellation definitions (moddable)
    reflector_targets.json        ← real solar-farm sites for Reflect Orbital mirrors
assets/
    textures/                    ← Earth day/night/elevation/clouds, moon, Milky Way
    sound/                       ← music tracks, flare SFX, UI sounds
    icons/ui/                    ← PNG icon sprites packed into GPU atlas
```

AI Code was used in this project.
See `CLAUDE.md` for the full architecture writeup (frame loop order, GPU buffer layouts, subsystem
design notes) and `THIRD_PARTY_NOTICES.txt` for third-party licenses.

---



