# Build, presets, release packaging, hardware floor

Build commands, runtime-file sync, screenshot / user-data redirects, the CMake presets, macOS universal builds, the Vulkan-limit table.

Part of the subsystem notes (`docs/subsystems/`, map in `AGENTS.md` §4). Moved verbatim from
`CLAUDE.md` on 2026-11-03. Do not read this file whole: `python tools/outline.py docs/subsystems/build.md`
lists its sections and topics with line numbers. New notes go next to the section they belong to.

## Build Commands

```bash
cmake -B build -S .                           # configure (downloads deps via FetchContent)
cmake --build build                           # build + compile shaders + copy SPVs next to exe
cmake --build build --config Release          # release build
```

Run: `build/Debug/SAT_LIGHT_SIM_V_<version>.exe` (e.g. `build/Debug/SAT_LIGHT_SIM_V_1_2_0.exe` — the
exact name tracks `VERSION`; see `CMakeLists.txt`'s `EXE_BASENAME`).

Shaders: auto-detected glob (`shaders/*.vert|.frag|.comp`), compiled by `glslc`, copied as `shaders/*.spv`. New shader files are picked up automatically on next build.

Runtime files (`assets/`, `data/*.json`, `data/satellite_models/`, `THIRD_PARTY_NOTICES.txt`) land next
to the exe via **two** mechanisms, and the second one is the one that matters day to day: the POST_BUILD
copy steps (for a fresh build dir), and `sat_sync_runtime_sources()` — a stamp target that DEPENDS on
those files, so changing one (e.g. regenerating an icon with `tools/make_icons.py`, or editing
`constellations.json`) re-syncs the exe's directory on the next build **even when no C++ changed**.
Without it, nothing relinks, nothing copies, and the exe keeps loading the old file that is already
sitting next to it — the change just silently "does not take".

**Screenshots survive the build tree.** The app writes user screenshots to `<exe dir>/screenshots`,
and that is inside the build tree (`build-win-release/Release/screenshots` for the `windows-release`
preset) — which `release.bat` deletes outright (`rmdir /s /q build-win-release`) before every release
build, taking the images with it. `sat_ensure_screenshot_dir()` (`cmake/EnsureScreenshotDir.cmake`),
an always-run target that the exe depends on, keeps that path a link instead — a directory junction on
Windows, a symlink elsewhere — pointing at the source tree's `screenshots/` (gitignored; the published
`docs/screenshots/` images are separate). Reconfiguring recreates the link, and a pre-existing real
`screenshots/` directory is migrated into it rather than lost.

**User data survives the build tree too (2026-10-13).** The app writes settings.json, `reports/`,
`perf_profiles/`, `bookmarks/`, `cinematics/`, traces and exports to `Paths::userDataDir()`, by default
the exe's folder — and `release.bat`'s wipe took a day's settings and perf snapshots with it.
`sat_ensure_user_data_dir()` (`cmake/EnsureUserDataDir.cmake`, an always-run target like the screenshot
one) writes `<exe dir>/user_data_dir.txt` naming `<source>/user_data/` (gitignored; cache variable
`SAT_DEV_USER_DATA_DIR`, empty = off), and `Paths::userDataDir()` uses that folder when the file is
there. So **the user's settings.json is `user_data/settings.json`**, shared by `build/` and
`build-win-release/`; the first time a tree gets its redirect, newer user files next to that exe are
copied over. A harness run's own override still wins, and the file is not in `PackageRelease.cmake`'s
whitelist, so a shipped copy stays portable. `pipeline_cache.bin` and the log's fallback stay per tree.
`python tools/run_latest.py` (VS Code task "Run build/Release (no build)") starts the existing
`build/Release` exe (the harness's tree) through the launch gate — for the user, not for agents; `cmake.buildBeforeRun` is
false, so the CMake Tools launch button no longer builds first (F5 debug still does).

**Launch the app only through the automation harness** (`tools/harness/run.py` / `live.py`,
docs/HARNESS.md) — never interactively (no `run` skill, no bare exe). A harness run is scripted,
exits on its own, runs muted in its own user-data folder (it cannot touch the user's
`settings.json` or trigger crash recovery), and returns captures, state sidecars, layout dumps and
timings you can check. Use it to verify rendering, performance and UI layout. How a change *feels*
(camera motion, input, audio, a menu's ergonomics) is still the user's to judge. (Until 2026-09-25
this rule was "never launch the app"; the harness is the controlled exception it was missing.)

**Requirements**: Vulkan SDK + `VULKAN_SDK` env var, CMake 3.20+, MSVC C++20.

### Presets and release packaging

`CMakePresets.json` holds every per-platform configuration (`windows` / `linux` / `macos`, plus
`*-release` variants and `macos-universal-release`). **It is committed and must stay free of
absolute paths** — machine-specific values go in `CMakeUserPresets.json`, which is gitignored.

**Windows has two release preset pairs.** `windows-release` / `windows-package` use the
`Visual Studio 17 2022` generator — this is the local-dev / `release.bat` path. **CI uses
`windows-ci` / `windows-ci-package` instead: Ninja + whatever MSVC toolset the runner ships**,
set up by `ilammy/msvc-dev-cmd`. The hardcoded VS-version generator name broke the v1.1.1 tag
build outright ("could not find any instance of Visual Studio") the day GitHub started migrating
the `windows-2025` image from VS2022 to VS2026; Ninja + a dev environment is version-agnostic and
matches how the Linux/macOS jobs already build. Keep both pairs — don't delete the VS one, IDE
users depend on it. `build-win-ci/` is the Ninja binary dir (gitignored).

`.github/workflows/release.yml` also runs on pushes to `main` and PRs targeting `main` (not just
`v*.*.*` tags) — build + package all three platforms, so a compiler-portability break (MSVC
accepts narrowing conversions in braced initializers that GCC/Clang reject — this is what the
v1.1.1 Linux/macOS jobs died on) or a CI-environment break surfaces in the PR, not at tag time.
The `publish` job stays tag-gated. `CMakeLists.txt` also promotes MSVC's C4838 (narrowing in an
initializer list) to an error via `sat_strict_warnings()` so that specific class fails a local
Windows build too.
`.vscode/settings.json` and `launch.json` are committed too and are under the same rule; a
hardcoded `cmake.cmakePath` and `VULKAN_SDK` under a developer's home directory shipped there once
(2026-09, macOS-potato branch) and reached the public repo's history.

`cmake --build <dir> --target package-release` stages and archives the distributable into `dist/`.
`cmake/PackageRelease.cmake` holds the copy list — **the only copy of it**. `.github/workflows/
release.yml` and `release.bat` both drive that target rather than repeating the list, which they
previously did five times over (3 CI jobs + 2 batch branches) and had already drifted. On macOS the
same script bundles the Vulkan loader + MoltenVK into `lib/` and writes the launcher `.command`
(there is no system Vulkan on macOS; the exe's baked-in `$VULKAN_SDK` path exists only on the build
machine).

**`CMAKE_POLICY_VERSION_MINIMUM 3.5` at the top of CMakeLists.txt is load-bearing, not cosmetic.**
CMake 4.0 turned `cmake_minimum_required(VERSION <3.5)` from a deprecation warning into a hard
configure error, and two FetchContent deps still declare one (glfw 3.4 → `3.4...3.28`,
nlohmann/json v3.11.3 → `3.1...3.14`). Without it, a stock CMake 4 — Homebrew's default, and
increasingly Windows'/Linux's — cannot configure this project at all. That failure is what the
committed absolute path to a portable CMake 3.31 was working around.

### macOS release architecture

The macOS CI job builds a **universal binary** (`CMAKE_OSX_ARCHITECTURES="arm64;x86_64"`) with
`CMAKE_OSX_DEPLOYMENT_TARGET=11.0`. Both are required and they fix *different* failures:
- **Architecture.** `macos-14` runners are Apple Silicon, so a default build is arm64-only and an
  Intel Mac rejects it with **"bad CPU type in executable"** — the kernel refusing to exec a Mach-O
  with no slice for the host, before any of this app's code runs. Universal is preferred over a
  second `macos-13` Intel runner job because GitHub is retiring those images, and it ships one
  download instead of making players choose.
- **Deployment target.** Without it clang stamps the *build* machine's OS version, and a
  macOS 14-targeted binary is refused on Monterey (12.x) with "not supported on this version of
  macOS" — the error that surfaces *next* once the CPU-type one is fixed.

The workflow's `lipo -archs` step fails the build if either slice is missing from the exe **or from
the bundled dylibs** — LunarG ships universal dylibs, but that is a property of their build, not
something this workflow controls.

### Old / low-end hardware floor

The 128-byte push-constant trim (see "Subsystem: Weak-Hardware Sky Tiers") is one of several places
this project sits above a Vulkan *guaranteed minimum*. `VulkanContext::logDeviceLimits()` logs all
of them every launch and throws a named error when one is short, so a report from a machine nobody
has access to is answerable. Current margins:

| Limit | Guaranteed min | This app needs | Why |
|---|---|---|---|
| `maxPushConstantsSize` | 128 | **128** | every PC struct static_asserts to exactly 128 |
| `maxImageDimension2D` | 4096 | **14999** | `earth_elevation.png` is 14999×7500 (GCN1/Metal cap is 16384 — little headroom) |
| `maxImageDimension3D` | 256 | **1024** | `aurora_noise.comp` bakes a 1024×16×256 volume |
| `maxComputeWorkGroupInvocations` | 128 | **256** | `local_size 16×16` in cloud_march / scene_depth / flare_blur |
| `pointSizeRange[1]` | 64 (with `largePoints`) | up to 1024 | glare sprites (`glare.vert`) clamp to it — smaller on weak parts |
| `maxComputeSharedMemorySize` | 16 KB | ~9 KB | `env_probe_sh.comp`'s SH partial sums (`vec3 part[64][9]`, 6.9 KB packed, 9 KB with vec3 padded); the cloud tile-cull lists ~4.6 KB |
| `maxPerStageDescriptorStorageBuffers` | 4 | **12** | `sat_orbit.comp`'s set (binding 11 = the Kessler buffer since 2026-10-16; the check said 6, for `sat_sky.frag`, long after this set passed it — corrected 2026-09-23 with the occlusion buffers); MoltenVK is the realistic place to hit it, since it maps SSBOs + UBOs + vertex buffers into Metal's 31 per-stage buffer slots |
| `maxPerStageDescriptorSampledImages` / `Samplers` | 16 | **16** | `sat_sky.frag` — AT the floor since terrain v2 P3's material array (binding 27, 2026-09-29): a new sky texture must merge into an existing one (e.g. the two city detail maps into an array). The terrain height clipmap (2026-11-10) adds two in its own set: 18. A device that offers 16 runs without it (`tcSupported`, the procedural terrain shaders), so the floor stays 16 |
| `maxPerStageDescriptorStorageImages` | 4 | **6** | ABOVE the floor (the v1.2 release audit, 2026-10-29: the check said 4): the limit counts the pipeline LAYOUT's bindings, and `cv2MarchDescLayout` has 6 (march colour/depth 10/11, the Sun/sky table's bake target 13, full-rate targets 17/18, the far layer 21) though the march itself reads 4. `sat_sky.frag`'s set has 4 (22/23 mesh targets, 25 the reflection G-buffer, 29 the far layer). MoltenVK reports at least 8 |

**The push-constant gate in `pickPhysicalDevice()` read 144 until 2026-09-08** — the pre-trim
`SatDrawPC` size — so it rejected precisely the hardware the trim was performed for. Keep that
constant equal to the largest PC struct; if one grows past 128 the fix is the CloudParams UBO, not
a bigger number there.

Two more non-guaranteed things are now checked rather than assumed: device **features** are
verified present before `vkCreateDevice` (requesting an unsupported one fails with
`VK_ERROR_FEATURE_NOT_PRESENT` and no indication of which), and **linear blit filtering** is a
per-format optional feature, so `VulkanContext::bestBlitFilter()` picks LINEAR/NEAREST per format
for both mipmap generation and the `renderScale < 1.0` upscale — the latter being the path that
exists *for* weak hardware, the least safe place to assume the optional feature.

VRAM is the untested one: the shipped textures are roughly 500 MB of GPU memory before mips
(8K day/night/clouds/specular/Milky Way + the 14999×7500 R8 DEM ≈ 112 MB on its own), against
2 GB on a 2015 MacBook Pro R9 M370X. Nothing streams or downsamples them. **Until 2026-09-22 the
per-satellite buffers reserved another ~2.2 GB on their own** (orbit + the since-deleted
GpuSatInput + visible, all allocated at `MAX_SATELLITES` = 10M); `createSatBuffers()` now sizes
them to the loaded roster (~100 B/satellite, ~131 MB for the default ~1.38M, ~940 MB at 10M).

---
