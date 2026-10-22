---
pages: [rendering/points-bloom-glare.md, simulation/reflectors.md, development/tools.md, development/harness.md, modding/constellations.md]
code: [shaders/sat_flare.comp, shaders/flare_source.vert, shaders/glare.vert, shaders/include/point_style.glsl, shaders/sat_orbit.comp, src/simulations/SatPhotometry.cpp, src/simulations/SatelliteSim.cpp, src/simulations/SatelliteSimHarness.cpp, tools/sat_model_tool/dump_json.cpp, tools/sat_model_tool/pass_eval.cpp, tools/video/]
---
## Now
**Flare ceiling is bloom-only.** A satellite's visible record carries its uncapped displayed effectFlare, so the point and
the glare (`glare.vert`, b = log2(flare)/2, now clamped at 8 instead of 4) see the full value: a bright flare shows its
diffraction spikes. The S3 soft ceiling R e / (e + R) (R = `sunFlareRefIntensity`, 40) applies only to the bloom seed
(`flare_source.vert`, via `FlareSourcePC::satCeilingBits`, the old `satCount`), the sky-glow bins and the ocean glints.
The point PSF's peak is min(D s0^2 / s^2, 1) (shader and CPU `pointPsf`).

**Reflect Orbital fallback per lock window.** When no target wins a satellite's lock window, the nearest night-valid site
it falls back to is chosen once per window (at the window's start; the previous window's at its start), in
`sat_orbit.comp` and its CPU mirror `satGroundSiteIdeal`. The mirror then eases at the "Mirror max slew rate" cap
like every other transition.

**Tools.** `SatModelTool --dump-json <out with MODEL>` writes a model's triangles, lobes per budget level, validation pairs
and brightness spheres; `SatModelTool --pass <spec.json> --pass-out <out.json>` evaluates a satellite along a given
state track (SGP4 replays) with occlusion and extinction. `SATLIGHTSIM_EXTRA_CONSTELLATIONS=<file>` appends that file's
`satellite_types` / `constellations` to the roster at load. Harness: `beams sats [site=] [n=]` (per beam: satellite,
target, aim error, miss, intensity, spot) and `state` -> `selection.gpu_record` (display flare, sprite px, glare flare,
raw flux). `tools/video/` (vidkit) is a general Manim + harness-clip + ffmpeg pipeline for technical videos, one folder
per project (`projects/sat_lighting` is the lighting-model video); outputs in gitignored `tools/video/out/`.

## History
The ceiling used to cap the record itself, so every flare above ~mag -1 hit one size: a hard-edged white disc with no
spikes (user report, 2026-10-21). Flares between about -0.5 and -3 mag now read up to 0.2 mag brighter; Venus and
Jupiter draw as soft ~6 px glows. The fallback used to be re-chosen every frame from the live position, so when the
nearest site changed mid-window the mirror snapped to it (seen while recording the video's Reflect sweep).
