# Subsystem notes

The index of this folder and the CLAUDE.md index as it stood before the split.

Part of the subsystem notes (`docs/subsystems/`, map in `AGENTS.md` §4). Moved verbatim from
`CLAUDE.md` on 2026-11-03. Do not read this file whole: `python tools/outline.py docs/subsystems/README.md`
lists its sections and topics with line numbers. New notes go next to the section they belong to.

## Files

| File | Covers |
|---|---|
| `build.md` | Build commands, runtime-file sync, screenshot / user-data redirects, the CMake presets, macOS universal builds, the Vulkan-limit table. |
| `architecture.md` | The three layers, frame loop order, unified scene depth, the shared depth buffer, shader #include headers, loading screen, startup graphics chooser, VulkanContext helpers, frames in flight, persistent settings, fixed sim state. |
| `clouds.md` | The cloud field, shapes, layers, rain particles, march, resolve, lighting, perf passes, reviews. Storms / weather objects are in weather.md. |
| `weather.md` | Storm objects (towers, shields, squall lines, MCSs, cyclones, tornadoes, surge), biome map + wildfires, haboobs, the Weather window, lightning + thunder, weather-cube evolution. |
| `sky-events.md` | The Sky events window and its event kinds, the lunar bases' glints, meteor showers, comets and their tails, the inertial camera lock. |
| `ui.md` | HUD popups, UIRenderer / Clay rules and kit, icon + font atlases, controls and keybindings, the settings window's layout and search, the intro cinematic, the first-run tutorial. |
| `harness-internals.md` | How the harness is wired into the app (the user manual is docs/HARNESS.md). |
| `capture.md` | Everything that records, exports or saves a view. |
| `sound.md` | The ambience layer table, drivers, synth voices, tonality, music player, offline audio. |
| `satellites.md` | Attitude groups, SatModel geometry / materials / lobes, earthshine, occlusion, the mesh renderer + env probes, benchmarks, SatModelTool, station models. |
| `orbits.md` | Constellation distributions, the sat_orbit / sat_flare dispatches and buffer layouts, push constants, TargetedReflector targeting, Reflect-Orbital beams and their cloud occlusion. |
| `kessler.md` | The scripted storm, dead satellites, debris pool, engine, dust, re-entries, rollback, prebake, memory. |
| `launches.md` | Trains, retirements, rockets, plumes, flames, pads, droneships, crew and cargo, berthing, the deployment era, mission milestones, mission-ops suite. |
| `photometry.md` | Flare/magnitude conventions, the point-source model, bloom and glare, the sky glow SSBO. |
| `sky-bodies.md` | The natural sky's objects and the light pollution / extinction models they share. |
| `perf.md` | Timestamps, CPU buckets, knockouts, the sweep, frame profile / hitches, and every perf pass with its measurements. |
| `render-pipeline.md` | The background's temporal AA and upscaling, the aerial-perspective volume, the Potato / SKY_LITE tiers and their size budgets. |
| `terrain.md` | Terrain history, procedural terrain detail, ELEVATION ENCODING (read before touching terrain), water map, city lights, farms, the sea. |

## The CLAUDE.md index as it stood on 2026-11-03 (before the split)

# CLAUDE.md

This file provides guidance to Claude Code when working with this repository.
The primary simulation is **SatelliteSim** (`src/simulations/SatelliteSim.h/.cpp`).
All other simulations (GameOfLife, Particles, Scene3DDemo) are legacy and rarely touched.

---

## Index — start here

This is the internal architecture reference. It is written to be read **before** editing code: the
load-bearing invariants are stated where they apply, and many sections carry a "the first cut did X
and that was wrong" note — usually the fastest way to understand why the code is shaped as it is.
Design decisions and their dates live next to the code they constrain, not in a separate chronology.

| Document | What it is |
|---|---|
| `README.md` | User-facing: what this is, prerequisites, build, packaging |
| `wiki/` + `mkdocs.yml` | **The project wiki** (MkDocs Material): present-tense reference for using, modding, simulation, accuracy, rendering, sound, development; dated material only under `wiki/history/`. Process in `wiki/development/wiki.md` |
| `docs/CONSTELLATION_MODDING.md` | User-facing modding guide (`constellations.json`, `satellite_models/*.json`) |
| `docs/rendering/` | **As-is rendering architecture**: frame graph, cloud field / march / lighting / temporal, terrain + depth, sky composite, points + meshes; `FINDINGS.md` lists suspected bugs and doc drift |
| `docs/HARNESS.md` | **Automation harness**: scripted runs, captures, perf, UI dumps — how an agent runs and sees the app |
| `tools/video/README.md` | **Technical videos (vidkit)**: Manim scenes driven by the tools' exports (`SatModelTool --dump-json`, benchmark reports) + engine clips + an ffmpeg edit; one folder per video under `tools/video/projects/`, output in the gitignored `tools/video/out/` |
| `docs/FREEZES.md` | The 2026-09 machine freezes (unresolved; mitigated by launch spacing) and the forensics tools built for them |
| `CHANGELOG.md` | What changed per release |
| `data/benchmarks/KNOWN_RESIDUALS.md` | Accepted photometric error, with the measurement behind it |
| `.plans/*.md` | **Untracked local design logs** (phase plans, terrain, cloud perf, boot loader). Sections below link to them; a fresh clone has none |

**Read in this order (~10 minutes) if you are new:** Build Commands → Architecture → Frame Loop Order
→ Unified scene depth → Satellite Types (Rigid attitude groups, Geometry models) → Photometry /
Shader Constants → VulkanContext Helpers. Then jump to the section you actually need.

**Build & release** — *Build Commands* (configure/build/run, exe name, and the rule about
launching the app: only through the harness) · *Presets and release packaging* (the two Windows release paths,
`package-release`) · *macOS release architecture* · *Old / low-end hardware floor* (the
guaranteed-minimum table and the push-constant gate).

**Architecture** — *Frame Loop Order* (the canonical pass order) · *Loading screen* (frames presented while init runs) · *Startup graphics chooser* (the mode picked before init; light tiers defer the full sky pipelines) · *Unified scene depth* (one
log-distance encoding, written by every surface) · *Occlusion: one shared depth buffer* (why
`sceneDepthImg` is half-res R32F, and what it replaced) · *Shader `#include`* (the header table, the
`CloudParams` mirror and `check_cloud_params.py`).

**Satellites: data, geometry, orbits** — *Satellite Types* (catalogue, **Rigid attitude groups**,
**Geometry models** — SatModel, materials, lobes, earthshine, transmission, render-only parts,
lattices, patterns — legacy attitude modes, `typeIdx`, adding a type) · *Orbital Mechanics /
Constellations* · *GPU Orbital Pipeline* (two-dispatch pattern, buffers, `Gpu*` layouts, push
constants) · *TargetedReflector / Mirror Ground Targets* · *Kessler syndrome — "The Sun's Fury"* (the
scripted storm, dead satellites, debris pool, cascade engine, event log) · *Launches* (trains, retirements,
rockets, plumes, crew rotations, the Missions window: the roster's slots refreshed on a schedule) · *Reflect-Orbital Beam Cloud Occlusion* ·
*GpuSatInput* (a tombstone — the buffer was deleted 2026-09-22). · *Sky events window* (eclipses, conjunctions, occultations, Moon-base glints, meteor showers, comets: a table of event kinds, each a search + a Watch) · *Meteors* (the annual showers + sporadics as a pure function of sim time; heads, streaks, fireballs, trains) · *Moon-base glints* (the lunar bases' solar arrays mirroring the Sun to Earth near full Moon, LunarBases.h) · *Comets* (the Great Comet of 2037 and hashed minor comets: conic orbits, the magnitude law, tails as 3D particles — dust under mu (1 - beta), ions in the solar wind — splatted in their own pass) · *Inertial camera lock* (the view fixed on the stars, the Earth turning beneath; the Sun occulter)

**Stars** — *Stars* (the HYG catalogue to V 8.5, faint stars only under dark skies, selectable stars, the scripted
2037 Betelgeuse supernova).

**Rendering** — *UIRenderer / Clay* (icon atlas, the SDF font atlas, manual hit-testing, the **UI kit** every window is built from, HUD text entry, **the UI text table**) · *Sky TAA* (the background's temporal AA, and the main pass's shared dependencies) · *The Sun as an object* (refraction, mirages, turbulence: one ray tracer, Refraction.h) · *Seeing* (the turbulence warp after the main pass, the real-time twinkle clock) · *The Moon as a body* (true position and size, eclipses) · *The sea* (periodic waves, sea state, shore) ·
*Photometry / Shader Constants* (lobe model, bloom and glare) · *The Milky Way* (the panorama, its zoomed star clouds, Andromeda) · *Light Pollution Dome* · *Atmospheric
Extinction* · *Sky Glow SSBO* · *Planets* · *Cloud Shadows* · *Resolution Scaling* · *Weak-Hardware
Sky Tiers (Potato / SKY_LITE)*. The mesh renderer, model viewer and environment probes are
subsections of *Satellite Types* (Phases 4b–4f).

**Sound** — *Ambient sound* (the layer table, the context drivers, procedural voices + CC0
samples, the tonality that follows the soundtrack (`MusicAnalysis`), the harness's offline audio
mode and the mix rule).

**State, profiling, cinematics, terrain** — *Persistent Settings* · *Fixed Simulation State* · *Video export* · *GPU
Performance Profiling* · *Intro Cinematic (UC3)* · *First-run tutorial* · *Bookmarks* · *Controls / Keybinding Pipeline* · *Active
Development: Earth / Terrain Rendering* (read **Elevation texture encoding** before touching terrain
code).

**Tools and gates** — `tools/harness/` (the automation harness: `run.py`, `live.py`,
`imgtools.py`, `selftest.py`, `gc.py`, `tstab.py` (temporal stability), `ui_lowres.py` (every window at low resolutions), `reports.py` (issue reports: triage, agent briefs, resolutions), `mission_ops.py` (every launch vehicle traced phase by phase, PASS / FAIL), `launch_bench.py` (the launch system's CPU / GPU cost), `hitch_check.py` (frame-hitch regression check over the time-scale ladder) — docs/HARNESS.md) · `tools/sat_model_tool/` (SatModelTool: bake,
validate, benchmark, trace replay, `--dump-json` for visualisation) · `tools/video/` (vidkit: technical videos) · `tools/check_cloud_params.py` · `tools/parse_bsc.py` · `tools/make_station_models.py` (the stations' models + ISS/Tiangong detail) · `tools/make_geo_belt.py` + `tools/make_geo_models.py` (the GEO belt's members and models) · `tools/make_pad_coasts.py` (the coasts around launch sites, `data/launch_coasts.json`) · `tools/make_kessler_prebake.py` (the shipped Sun's Fury history, `data/kessler/*.pak`) · `tools/make_icons.py` (regenerates the
UI icon PNGs from geometry declared in that file) · `tools/make_ambience.py` (the ambience samples,
from CC0 sources declared in that file) · `tools/sound_tool/` (SoundTool: soundtrack analysis +
offline synth renders) · `tools/benchmarks/` ·
`cmake/AccuracyGate.cmake` (`cmake --build build --target accuracy-gate`) ·
`cmake/PackageRelease.cmake` (the single "what ships" list).

**Wiki routine** — never edit `wiki/` pages during feature work. When a change alters something the wiki
describes (behaviour, technique, file format, setting/default, accuracy or perf number), finish by leaving a
note in `wiki/_inbox/` with the `wiki-note` skill. Pages are rewritten only in a dedicated pass (`wiki-pass`
skill), which folds the notes in as present-tense prose, files their history under `wiki/history/`, and must
pass `python tools/wiki/check.py`.

**Rules that bite (each one has a section behind it)** — launch the app only through the harness
(`tools/harness/run.py`, docs/HARNESS.md), never interactively, and never back-to-back (the harness
spaces launches 30 s apart); how a change *feels* is the user's to
judge; `GpuCloudParams` is a hand-maintained mirror of
`cloud_params.glsl` (run the checker after touching either); never store a distance in a half-float;
every push-constant struct `static_assert`s to exactly 128 bytes; `CMakePresets.json` and `.vscode/`
are committed and must stay free of absolute paths; `.plans/`, `.claude/`, `build*/`, `dist/` and
`benchmark_runs/`, `harness_runs/`, `harness_live/` are gitignored.

---
