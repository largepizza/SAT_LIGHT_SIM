# Building and releasing

How to configure and build SAT LIGHT SIM, how runtime files get next to the executable, the CMake presets,
and how release archives are made locally and in CI. It does not cover running the app; for that see the
[automation harness](harness.md).

## Prerequisites

| Dependency | Notes |
|---|---|
| [Vulkan SDK](https://vulkan.lunarg.com/) | must set `VULKAN_SDK`; provides the Vulkan loader, headers and `glslc` (the shader compiler). On macOS and Linux run its `setup-env.sh` |
| CMake 3.20 or newer | presets need 3.21. CMake 4 works (see [the policy line](#the-cmake-4-policy-line)) |
| A C++20 compiler | MSVC (Visual Studio 2022) on Windows, Clang (Xcode command line tools) on macOS, GCC or Clang on Linux |
| Ninja | for the Linux, macOS and Windows CI presets |

Everything else is fetched at configure time with CMake's FetchContent: GLFW 3.4, GLM 1.0.1, Clay (UI
layout), stb (font baking and image loading), miniaudio and nlohmann/json 3.11.3. On Windows the MSVC
runtime is linked statically (`/MT`), so the executable runs without the Visual C++ redistributable.

## Configure and build

```bash
cmake -B build -S .            # configure (downloads the dependencies)
cmake --build build            # build the app, compile the shaders, copy runtime files
cmake --build build --config Release
```

The executable is named `SAT_LIGHT_SIM_V_<version>`, where `<version>` is the `VERSION` file with dots
replaced by underscores, for example `build/Release/SAT_LIGHT_SIM_V_1_2_0.exe`. `VERSION` is the single source
of truth for the version; CMake reads it into `APP_VERSION` and `EXE_BASENAME`.

The git commit and build date are stamped into `version.h` **at configure time**, not per build. Reconfigure
before handing a build to someone, or its log, traces and settings carry an older commit.

### Targets

| Target | In `ALL` | What |
|---|---|---|
| `SatLightSim` | yes | the app |
| `CompileShaders` | yes | every shader in `shaders/` to SPIR-V |
| `SatLightSimFresh` | no | the same app compiled with `SAT_FRESH_SETTINGS`: it never reads or writes `settings.json`, so every launch is a genuine first run (named `SatLightSim_FRESH`, never shipped) |
| `SatModelTool` | no | satellite model bake, validation and benchmarks ([Tools](tools.md#satmodeltool)) |
| `SoundTool` | no | soundtrack analysis and offline synth renders ([Tools](tools.md#soundtool)) |
| `UploadStress` | no | a standalone stress test of the texture upload path ([Tools](tools.md#uploadstress)) |
| `accuracy-gate` | no | builds SatModelTool and runs every photometric check ([SatBench and the accuracy gate](../accuracy/satbench.md)) |
| `package-release` | no | stages and archives a release into `dist/` ([Packaging](#release-packaging)) |

`sat_strict_warnings()` makes MSVC warning C4838 (a narrowing conversion in a braced initializer) an error.
GCC and Clang reject that construct outright; without the flag it would pass a Windows build and fail only
in Linux or macOS CI.

### Shaders

CMake globs `shaders/*.vert`, `*.frag` and `*.comp` and compiles each with `glslc -I shaders/include` into
`<build>/shaders/<name>.spv`; a new shader file is picked up on the next build. Some sources are compiled
more than once with different defines:

| Output | Source and define | Used for |
|---|---|---|
| `sat_sky_lite.frag.spv` | `sat_sky.frag -DSKY_LITE` | the weak-hardware sky tier ([Weak-hardware tiers](../rendering/hardware-tiers.md)) |
| `sat_sky_env.frag.spv` | `-DSKY_ENV` | the sky seen from a satellite: mesh environment probes and the model viewer's background |
| `sat_sky_refl.frag.spv` | `-DSKY_ENV -DSKY_REFL` | sharp reflections in mirror-smooth mesh pixels |
| `sat_sky_taa.frag.spv` | `-DSKY_TAA` | the jittered offscreen sky for temporal anti-aliasing |
| `sat_mesh_depth.frag.spv`, `sat_mesh_scene.frag.spv` | `sat_mesh.frag -DMESH_DEPTH_PASS` / `-DMESH_SCENE_PASS` | the mesh depth pre-pass and its equal-depth shading pass |

Every shader depends on **every** header in `shaders/include/`, so touching one header recompiles all
shaders. That is coarse but correct on every generator: the precise alternative (`glslc -MD` with a
`DEPFILE`) needs CMake 3.27 for Visual Studio generators. Headers must live in `shaders/include/` with a
`.glsl` extension, or the glob would try to compile them as shaders.

## Runtime files next to the executable

The app reads its assets with paths relative to the executable's directory (`main.cpp` changes the working
directory there at startup). Two mechanisms put them there:

1. **POST_BUILD copies** (`sat_copy_runtime_files()`): after the app links, the compiled shaders, `assets/`,
   `data/satellite_models/`, `constellations.json`, its schema, `reflector_targets.json` and
   `THIRD_PARTY_NOTICES.txt` are copied next to it. These run only when the target relinks.
2. **A stamp target** (`sat_sync_runtime_sources()`): a custom command that *depends on* all those files and
   re-copies them when any changes. This is the one that matters day to day. Without it, editing
   `constellations.json` or regenerating an icon with `tools/make_icons.py` changes no C++, nothing relinks,
   nothing is copied, and the executable silently keeps loading the old file sitting next to it. Its globs use
   `CONFIGURE_DEPENDS`, so adding or removing a file also re-runs the configure step.

!!! warning "Invariant"
    Edit runtime files in the source tree (`data/`, `assets/`), never in the build directory: the next build
    overwrites the copies next to the executable.

### Screenshots outlive the build tree

The app writes screenshots to `<exe dir>/screenshots`, which is inside the build tree and is deleted whenever
that tree is wiped (`release.bat` deletes `build-win-release/` before every release build).
`sat_ensure_screenshot_dir()` runs `cmake/EnsureScreenshotDir.cmake` on every build. It keeps
`<exe dir>/screenshots` as a link to the source tree's `screenshots/` directory (gitignored): a directory
junction on Windows (which needs neither elevation nor Developer Mode) and a symlink elsewhere. It reads a
marker file through the link to detect a correct link cheaply, recreates a missing one, and migrates a real
`screenshots/` directory into the source tree instead of losing it. If no link can be made it falls back to a
real directory with a warning.

## Presets

`CMakePresets.json` holds every per-platform configuration:

| Configure preset | Generator | Build dir | Use |
|---|---|---|---|
| `windows` | Visual Studio 17 2022 | `build/` | local Windows development, IDE users |
| `windows-release` | Visual Studio 17 2022 | `build-win-release/` | local Windows releases (`release.bat`) |
| `windows-ci` | Ninja + whatever MSVC the environment provides | `build-win-ci/` | Windows CI |
| `linux`, `linux-release` | Ninja | `build/`, `build-linux-release/` | Linux development and releases |
| `macos` | Ninja | `build/` | native-architecture macOS development |
| `macos-universal-release` | Ninja | `build-macos-release/` | universal (arm64 + x86_64) macOS releases |

Build presets add `windows-package`, `windows-ci-package`, `linux-package`, `macos-package` (the
`package-release` target) and `linux-accuracy-gate`.

**There are two Windows release pairs on purpose.** `windows-release` / `windows-package` name the Visual
Studio 2022 generator, which IDE users and `release.bat` rely on. CI uses `windows-ci` /
`windows-ci-package`, Ninja inside an MSVC developer environment, because a generator that names a specific
Visual Studio version fails to configure as soon as a CI runner image ships a different Visual Studio. Ninja
with whatever toolset the environment provides is version-agnostic and matches how the Linux and macOS jobs
build. Keep both pairs.

### Committed configuration

`CMakePresets.json` and `.vscode/` (`settings.json`, `launch.json`, `tasks.json`, `c_cpp_properties.json`) are
committed and **must stay free of absolute paths**. `VULKAN_SDK` and `cmake` are found through the
environment and `PATH`. Machine-specific values belong in `CMakeUserPresets.json`, which is gitignored.

### The CMake 4 policy line

The top of `CMakeLists.txt` sets `CMAKE_POLICY_VERSION_MINIMUM` to 3.5 unless it is already defined. This is
load-bearing: CMake 4 made `cmake_minimum_required(VERSION <3.5)` a hard configure error, and two of the
fetched dependencies (GLFW 3.4 and nlohmann/json 3.11.3) still declare such a range. Without the line a stock
CMake 4 cannot configure the project at all. Older CMake versions ignore the variable.

## Release packaging

```bash
cmake --preset windows-release
cmake --build --preset windows-release --parallel
cmake --build --preset windows-package     # dist/SAT_LIGHT_SIM_v<version>_Windows.zip
```

The `package-release` target runs `cmake/PackageRelease.cmake`, **the only copy of the list of what ships**.
CI and `release.bat` both drive this target rather than repeating the list. It stages
`dist/SAT_LIGHT_SIM_v<version>_<platform>/` and archives it (`.zip` on Windows, `.tar.gz` elsewhere). The
payload is a whitelist:

- the executable;
- `shaders/` (the compiled SPIR-V, including the variants with no source counterpart), `assets/` and
  `satellite_models/` from the build's runtime directory;
- `constellations.json`, `constellations.schema.json`, `reflector_targets.json` and
  `THIRD_PARTY_NOTICES.txt` from the source tree.

`screenshots/` is deliberately not in the list: in a development tree it is a link to the developer's own
screenshots, and copying it would follow the link into the archive. A shipped install creates its own
`screenshots/` on the first capture.

The archive's platform tag is `Windows`, `Linux`, `macOS_universal` for a multi-architecture build, or
`macOS_<arch>` for a single-architecture one.

### `release.bat`

A Windows convenience wrapper over the presets: `release.bat` builds Windows and (through WSL, if installed)
Linux; `release.bat windows` or `release.bat linux` builds one. Each leg deletes its build directory,
configures, builds and runs the package preset. macOS cannot be cross-built from Windows; it comes from CI.

## Continuous integration

`.github/workflows/release.yml` runs on version tags (`v*.*.*`), on pushes to `main`, on pull requests
targeting `main` (documentation-only changes skipped) and on manual dispatch. Three jobs build and package
with the presets above:

| Job | Runner | Steps |
|---|---|---|
| `build-windows` | `windows-2025` | MSVC developer environment, the LunarG Vulkan SDK, `windows-ci` configure, build, package |
| `build-linux` | `ubuntu-latest` | Vulkan SDK, X11/Wayland development packages and Ninja, `linux-release` build, **the accuracy gate**, package |
| `build-macos` | `macos-14` | Vulkan SDK, `macos-universal-release` build, package, then a check that the executable and every bundled library contain both architectures |

The `publish` job, which creates the GitHub release from the three archives, runs only for tags. Running the
build on pull requests means a compiler-portability break (MSVC accepts things GCC and Clang reject) or a
runner-image change surfaces in the pull request, not when a release is tagged. The runner images are pinned
so an image update cannot silently swap the toolchain under a release. The accuracy gate runs on Linux only:
the tool is platform-independent and its sampling is deterministic, and that runner is the fastest.

### macOS: universal binaries and the deployment target

The macOS release is a **universal binary** (`CMAKE_OSX_ARCHITECTURES="arm64;x86_64"`) with
`CMAKE_OSX_DEPLOYMENT_TARGET=11.0`. The two settings fix two independent failures:

- **Architecture.** The CI runners are Apple Silicon, so a default build is arm64 only, and an Intel Mac
  refuses to execute it ("bad CPU type in executable") before any of the app's code runs. A universal binary
  ships one download for both, and keeps working as Intel runner images are retired.
- **Deployment target.** Without it, clang stamps the build machine's OS version into the binary, and an
  older macOS refuses it ("not supported on this version of macOS"). 11.0 is the oldest release with both
  Apple Silicon and MoltenVK support. `CMakeLists.txt` sets it before `project()` (which is what bakes it into
  every object file) and the preset pins it again.

macOS has no system Vulkan: Vulkan exists only through MoltenVK, loaded by the Vulkan loader at runtime. The
executable is built against `$VULKAN_SDK`, a path that exists only on the build machine, so packaging on macOS
also bundles the Vulkan loader, MoltenVK and an ICD manifest into `lib/` and writes a launcher `.command`
that points the process at them. The CI check fails the build if the executable or any bundled library lacks
either architecture.

## Raw textures

The largest single-channel textures are not decoded from PNG at launch. `tools/make_raw_textures.py`
pre-decodes them into a raw format the app reads byte for byte (`readRawR8()` in `SatelliteSim.cpp`): a
16-byte header (`"SLR8"`, width, total height, first row) followed by rows top to bottom, one byte per texel.
A map larger than GitHub's 100 MB file limit is split into consecutive row bands (`earth_elevation_0.r8`,
`earth_elevation_1.r8`). The PNGs stay the source; the app falls back to them (with a log line) when a `.r8`
file is missing. **After editing a source PNG, re-run the script**; the conversion is byte-exact.

Other baked assets (the water map, road network, terrain materials, cloud morphology, ambience samples, UI
icons) each have their own script; see [Tools](tools.md#asset-bakers).

## Where in the code

| File | What |
|---|---|
| `CMakeLists.txt` | targets, FetchContent, shader compilation, `sat_copy_runtime_files()`, `sat_sync_runtime_sources()`, `sat_ensure_screenshot_dir()`, `package-release`, `accuracy-gate` |
| `CMakePresets.json` | configure and build presets |
| `cmake/PackageRelease.cmake` | what ships, and the macOS Vulkan bundling |
| `cmake/EnsureScreenshotDir.cmake` | the screenshots link |
| `cmake/AccuracyGate.cmake` | the list of photometric checks |
| `release.bat` | local Windows and WSL Linux releases |
| `.github/workflows/release.yml` | CI build, package, gate and publish |
| `VERSION` | the version |
