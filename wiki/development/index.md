# Development

This section is for contributors: how the repository is laid out, how to build and release it, how the code
is structured, the conventions that keep the GPU code correct, and the tools for testing a change. It
assumes you can read C++20, GLSL and CMake. Gameplay and modding are covered in [Using](../using/index.md)
and [Modding](../modding/index.md).

The primary application is **SatelliteSim** (`src/simulations/SatelliteSim.*`). The repository also holds
three small legacy demos (Game of Life, particles, a 3D scene) that share the framework and are rarely
touched.

## Repository layout

| Path | What |
|---|---|
| `src/` | the framework: `main.cpp`, `App` (window and frame loop), `VulkanContext` (Vulkan boilerplate and helpers), `Simulation.h` (the interface a simulation implements), `UIRenderer` (Clay UI on Vulkan), `AudioSystem`, `AmbientSynth`, `MusicAnalysis`, `Harness` (the automation harness's command language), `Log`, `Paths` |
| `src/simulations/` | SatelliteSim and its parts: the simulation (`SatelliteSim*.cpp`), satellite models and photometry (`SatModel`, `SatPhotometry`, `SatMesh`, `SatMeshRenderer`, `SatEnvProbes`), benchmarking (`SatBench`, `SatBenchmark`, `SatTrace`), `Ambience`, `Cinematic`, the star catalogue, the UI palette; plus the legacy demos |
| `shaders/` | GLSL sources (`*.vert`, `*.frag`, `*.comp`), compiled by `glslc` at build time |
| `shaders/include/` | shared GLSL headers (`*.glsl`), included with `#include`; never compiled on their own |
| `assets/` | runtime assets copied next to the executable: `textures/`, `icons/ui/` (generated), `sound/` (music, ambience, UI clicks), `noise/` |
| `data/` | moddable runtime data: `constellations.json` and its schema, `reflector_targets.json`, `satellite_models/`; `benchmarks/` (published photometry, read only by the tools); `custom/` (example rosters) |
| `tools/` | Python and C++ tools: the harness drivers, SatModelTool, SoundTool, asset bakers, checkers, analysis scripts ([Tools](tools.md)) |
| `cmake/` | CMake scripts run in script mode: `PackageRelease.cmake`, `AccuracyGate.cmake`, `EnsureScreenshotDir.cmake`, `CopyIfMissing.cmake` |
| `wiki/` | this wiki ([Working on this wiki](wiki.md)) |
| `docs/` | older user-facing documents (modding guide, harness reference, rendering notes) |
| `.github/workflows/` | `release.yml` (build, package, accuracy gate, publish), `wiki.yml` |

Gitignored, never committed: `build*/`, `dist/`, `harness_runs/`, `harness_live/`, `benchmark_runs/`,
`/benchmarks/`, `.plans/` (local design logs), `.claude/` (except its `skills/`), `CMakeUserPresets.json`,
compiled `*.spv`, Python virtual environments.

## Rules that bite

Each of these has broken the build, the image or the machine when ignored. The linked section explains why.

| Rule | Details |
|---|---|
| Launch the app only through the [automation harness](harness.md), never interactively, and never back-to-back: the harness waits 30 s after an app exit before the next launch | [Launch spacing](harness.md#launch-spacing) |
| How a change *feels* (camera motion, input, audio, ergonomics) is the user's to judge; the harness checks numbers and images | [Harness](harness.md) |
| `GpuCloudParams` in C++ is a hand-kept mirror of the GLSL `CloudParams` block; run `python tools/check_cloud_params.py` after touching either, and append new fields at the end of both | [CloudParams](gpu-conventions.md#the-cloudparams-uniform-block) |
| Never store a distance in a half float (RGBA16F tops out at 65 504) | [Precision](gpu-conventions.md#precision-rules) |
| Never form an absolute ECEF position in a float in a shader; work relative to the observer | [Precision](gpu-conventions.md#precision-rules) |
| Every pass that produces or consumes a distance uses the same observer height (`observerEffHeight`) | [Precision](gpu-conventions.md#precision-rules) |
| No push-constant block may exceed 128 bytes; the large ones sit at exactly 128. New per-frame values go in a uniform buffer | [Push constants](gpu-conventions.md#push-constants-128-bytes) |
| `sat_sky.frag` uses all 16 guaranteed sampled-image slots; a new texture there must merge into an existing one | [Descriptor budgets](gpu-conventions.md#descriptor-budgets) |
| `std430` structs shared with C++ need explicit pads: `vec3` aligns to 16 in GLSL and to 4 in C++ | [Layouts](gpu-conventions.md#shared-struct-layouts) |
| Runtime files (assets, JSON, models) reach the executable only through a build | [Runtime files](building.md#runtime-files-next-to-the-executable) |
| `CMakePresets.json` and `.vscode/` are committed and must stay free of absolute paths | [Committed configuration](building.md#committed-configuration) |
| A new settings slider needs a free slot below `kCloudSliderSlots`, which sizes all four per-slider arrays | [Slider slots](controls-and-settings.md#slider-slots) |
| A new key binding goes through the `KB` enum, the `keybindings` initializer and its `static_assert` | [Adding a control](controls-and-settings.md#adding-a-control) |
| Never `return` or `break` inside a `CLAY(...)` block | [UI rules](ui.md#clay-rules) |
| Every Earth-rotation (ECI to ECEF) conversion goes through `earthRotationAngle()`, never `kOmegaEarth * t` | [Time and reference frames](../simulation/time-and-frames.md) |
| Satellite part occlusion is opt-in: no default, preset or first run turns it on (it multiplies the orbit pass's cost several times over) | [Graphics presets](controls-and-settings.md#graphics-presets) |

## In this section

| Page | Covers |
|---|---|
| [Building and releasing](building.md) | prerequisites, configure and build, runtime file sync, presets, packaging, CI, macOS universal builds, raw textures |
| [Code architecture](architecture.md) | the three layers, the `Simulation` interface, the frame loop, the SatelliteSim file split, the loading screen, threads |
| [GPU conventions](gpu-conventions.md) | `VulkanContext` helpers, barriers, push constants, `CloudParams`, layouts, shader includes, precision, descriptor budgets, register pressure, validation |
| [UI (Clay)](ui.md) | the Clay rules, icons, the bitmap font, images and plots, text fields, hit-testing, the palette |
| [Controls and settings plumbing](controls-and-settings.md) | key bindings, the gamepad, `settings.json`, slider slots, tabs, graphics presets, defaults |
| [Automation harness](harness.md) | running the app from scripts: commands, outputs, analysis tools, architecture |
| [Tools](tools.md) | SatModelTool, SoundTool, the asset bakers, checkers and analysis scripts |
| [Profiling](profiling.md) | GPU and CPU timing, knockout bits, the knockout sweep, snapshots, A/B testing shader changes |
| [Working on this wiki](wiki.md), [Style guide](style-guide.md) | how these pages are maintained |
