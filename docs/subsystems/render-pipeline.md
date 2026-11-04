# Render pipeline: cloud shadows, AP volume, sky TAA, resolution scaling, weak-hardware tiers

The background's temporal AA and upscaling, the aerial-perspective volume, the Potato / SKY_LITE tiers and their size budgets.

Part of the subsystem notes (`docs/subsystems/`, map in `AGENTS.md` §4). Moved verbatim from
`CLAUDE.md` on 2026-11-03. Do not read this file whole: `python tools/outline.py docs/subsystems/render-pipeline.md`
lists its sections and topics with line numbers. New notes go next to the section they belong to.

## Subsystem: Cloud Shadows

Marched **per pixel** inside `cloud_march.comp` (`cloudGroundShadow`), sunward from the terrain hit
point `scene_depth.comp` supplies, and delivered in `cloudTargetB.a` — the channel freed by
deleting `tEnterCombined`. `sat_sky.frag` consumes it as a single `directSun *= cloudB.a`.
Gated on `tScene < kNoSurfaceT`, so sky pixels pay nothing. Knockout bit 256.

This replaced `cloud_shadow.comp`, a fixed 128x128 observer-centred tangent-plane grid, which is
worth understanding because the failure modes were structural rather than tuning:

| | 128² grid | per-pixel |
|---|---|---|
| Resolution | 1250 m/texel uniformly at the 80 km default | screen-space; metres near camera, coarsens with distance |
| From altitude | observer-centred at fixed world extent, so it covered less and less of the visible ground | follows the view ray |
| Range | hard cutoff at `cloudShadowRangeM` (which had already caused a real beam bug, worked around via `blockOpacity`) | none |
| Swimming | needed `computeCloudShadowSnap()` + a residual subtracted by every consumer | nothing to snap — the value is a function of the world point being shaded, not the camera |

Deleted with it: the image/view/sampler/descriptor set/pipeline, `CloudShadowPC`, the snapping
block, two `SatDrawPC` fields, the "Cloud shadow range (m)" slider and its settings key, and the
pass's timestamp bucket. Same 12 steps and the same `3e-3` density→optical-depth constant the grid
used, so brightness is comparable. The march phase is jittered off `noiseTex` because adjacent
half-res texels can map to ground points kilometres apart at grazing angles.

---

## Subsystem: Aerial-perspective volume (2026-10-25, branch perf-frames)

`sat_sky.frag`'s single-scattering atmosphere loop (the N_VIEW loop: Rayleigh + Mie with the Sun's path, the Earth's
shadow, god rays, haze near clouds, the orbital terminator gate, an eclipse, the city glow, green / sodium airglow, the
sky-lit shadowed air) ran for EVERY pixel: ~4.4 ms of the sky pass on the ground, more at dusk. `shaders/sky_ap.comp` now
runs it ONCE PER SCREEN TILE and stores its running sums along the tile's ray; the sky pass reads them at the pixel's
distance. Layout and lookup: `include/sky_ap.glsl`; the shared helpers (optDepth, the Earth's shadow, skyZenithSky,
atmHalfAvg3, airglow noise, cityBrightness) moved unchanged into `include/sky_atmos.glsl`, included by both.
- **Grid** = the god-ray froxels' tiles (320 x 180, `cv2FroxJitter`'s per-frame offset; the sky TAA averages the tent),
  48 slices at a NORMALISED depth: u = (t - tStart) / L, tStart the ray's entry into the atmosphere, L its length to the
  atmosphere's exit or the sea-level sphere, slices log-spaced from BOTH ends (d0 = 200 m, `skyApSlice`). A pixel maps its
  distance through its own ray's tStart and L, so a sky pixel reads the last slice exactly. A log from the start alone
  put the last ~17% of a descending ray (from orbit, from an aircraft: where its dense air is) in one slice and brightened
  every raised surface.
- **Stored per unit of air** (`skyApNorm`: the ray's analytic Rayleigh / Mie column from the eye, `atmosphere.glsl`
  atmColumn): the raw sums grow like the path (1 / cos of the incidence near the limb) and a tile's bilinear blend of that
  convex growth read up to ~9 levels brighter from orbit (0.42 -> 0.08 levels mean once normalised). The airglow columns
  are raw. 12 half floats a cell (accumR, accumM, the shadow light, the city glow, odR, odM, green, sodium), one runtime
  `uvec2` array, x 1e-4: ~63 MB, made the first frame it runs (`ensureSkyAp`; the flash buffer is the placeholder at sky
  binding 33 until then, never read: `cv2.skyAp.x` is 0).
  **Short segments take the column directly** (2026-10-27, issue 20261027_212920 "Water glitter issues when moving",
  0 m over a stormy sea in rain): `atmColumn` is a difference of two Chapman columns to infinity (~35 scale heights each
  along a level ray, r - R good to ~0.5 m in float), so a column of metres was noise of ~1e-3 columns, often 0 (floored
  1e-7); the air in front of rain a few metres off (`tAirFrontM`) read sums / 1e-7 x noise: isolated white pixels over
  the sea, many after every history reset and in motion (blue-noise-patterned: the rain texels' distances). `skyApNorm`
  integrates exp(-h / H) by Simpson along any segment climbing or falling under half a scale height (both sides use it).
  White isolated pixels in the report's path: 4800 / 10704 / 7212 per frame -> 0.
- **The pass** (one thread a tile, after the froxel pass in the cloud bucket, march set + set 1, the march's push
  constants): steps follow the slices, cut into sub-steps no longer than the pixel loop's step (100 km / "View samples
  min"); the god rays read the tile's own froxel column (the same ray and offset); the shadowed air's sky light is taken at
  the shadow's mean point UP TO each slice. **Keep its step in step with the N_VIEW loop** — the band below still runs the
  loop. ~0.3-0.7 ms (RTX 3070 Ti).
- **The band**: within `clouds_v2.sky_ap_band_deg` (2 deg, at most 12 tiles since 2026-10-28, at least 2.5 tiles) of the horizon / the Earth's limb (and the
  atmosphere's top seen from above it), where the integral changes too fast across a tile, the loop runs as before; over
  the band's outer part (2.5 tiles out to its edge) the pixel takes BOTH and cross-fades (`apW`). The volume is the MORE
  accurate (finer slices: against a 64-1024-sample loop 0.01-0.34 levels mean where the default loop is 0.6-0.8, ~5 levels
  at dusk near the horizon), so a hard edge was a step of ~1-4 levels. `sky_ap_band_steps` (no UI, 1) multiplies the loop's
  steps in the band: 4 matches the volume but doubled the sky pass from orbit (the band is the whole limb).
- **Off**: above 1000 km of eye altitude (the loop starts at the atmosphere's top and is cheap there; the pass cost more
  than it saved), SKY_LITE / Potato / SKY_ENV / SKY_REFL (no volume), the HQ photo (`cv2SkyAp` saved / set 0), knockout bit 2.
  "Aerial perspective volume (0/1)" (`clouds_v2.sky_ap`, slot 337; 0 = the loop everywhere, the A/B), "... horizon band
  (deg)" (slot 338).
- **The band is capped at 12 tiles** (2026-10-28, issue 20261028_192855: zoomed to a 0.58-deg field on the refracted Sun
  from 96 km, a tile is 0.003 deg and the fixed 2-deg band covered the whole screen: every pixel ran the loop). Unchanged at
  a 30-deg field and wider. That view (1920x1009, interleaved): sky pass 34.6 -> 9.3 ms (FOV 5: 23.8 -> 10.9, FOV 40 10.4 ->
  10.2); against a 64-4096-sample loop the volume is within ~1 level everywhere, where the default loop read the ground just
  under the limb ~15 levels dark. Its clouds (16-24 ms, run to run) cost about the same at a 40-deg field: content (march
  ~10.5, composite 2.7, resolve 1.2, sky_ap 1.2); an LOD pixel floor at narrow fields saved ~2 ms and was not adopted.
- **Measured** (RTX 3070 Ti, 1920x1009, `harness_runs/ap/`): sky pass summed over 17 scenes 191 -> 146 ms (-24%), the GPU
  frame 425 -> 385 ms; `frame_bench.py` wall clock (uncapped, one run a side) stills 834.5 -> 767.4 ms (-8.0%), motion
  paths 1408 -> 1293 ms (-8.2%; ground and air -7..-14%, the eclipse -20..-25%, orbit -1..-11%). Matrix suite on vs off:
  every motion and noise number within 0.01-0.07 levels. Sky TAA pipeline 168 registers both, binary 811 -> 835 KB.
  Validation clean.
- Open: the pass is one thread a tile (57,600 threads, long loops); the band from orbit is the whole limb ring and keeps
  the loop's (less accurate) result there.

---

## Subsystem: Sky TAA — temporal anti-aliasing of the background (2026-09-30)

The user asked for terrain anti-aliasing; silhouettes, detail normals, far textures and city glitter all
aliased because the sky pass shades one ray per pixel. With the full sky shader at ANY render scale
(`skyTaaWanted`; Potato and SKY_LITE keep their own paths; below 100% it is also the temporal upscaler,
see the last bullets), `recordPrePass` → `recordSkyTaa`:
1. `sat_sky.frag -DSKY_TAA` (`sat_sky_taa.frag.spv`) renders the background offscreen with a Halton (2,3)
   sub-pixel jitter (`cloud.taaJitter.xy`, applied to the ray via the interpolated direction's screen
   derivatives) into RGBA16F + its unified depth as an R32F colour (no depth attachment).
2. `sky_taa.comp` reprojects the previous RESOLVED frame exactly (camera rotation + the eye's motion at each
   pixel's depth, both from the CPU in double, in this frame's ENU; sky by rotation alone), samples it
   Catmull-Rom, rejects disocclusion against the RANGE of the 2x2 history depths (against one texel it
   rejected every silhouette pixel every frame — no anti-aliasing at all), variance-clips it in YCoCg (the
   3x3 of this frame) and blends (`display.sky_taa_weight` 0.1 still, `_moving` 0.35 past ~4 px of motion).
   Ping-pong histories in GENERAL layout.
3. The result is blitted into the swapchain; the main pass (renderPassLoad) opens with `taa_depth_restore.frag`
   writing the depth back (colour writes off), so stars, planets and satellites draw over it unjittered and
   occluded. Display tab "Temporal AA (terrain, sky, sea)", `display.sky_taa`. Cost +1.1 ms still, +0.8 panning
   (1600x900). The half-res cloud composite rides in it, so cloud edges are averaged on screen too.
- **The swapchain now has TRANSFER_DST usage** (`swapTransferDstSupported`): the renderScale < 1 prepass had
  been blitting into images without it. **The main pass's three variants share ONE dependency list**
  (`mainPassDependencies`): compatibility includes dependencies, and every draw of a load or boot frame was
  invalid against the framebuffers and pipelines made with ctx.renderPass (validation layer, 2026-09-30).
- **Temporal UPSCALING below render scale 100% (2026-10-04):** the TAA path now runs at any render scale (with the
  full sky shader). The sky pass renders into the top-left `skyTaaInExtent()` of its full-size input images
  (dynamic viewport/scissor, so a scale change recreates nothing), jittered over 16 Halton phases in INPUT pixels; the
  resolve (`sky_taa.comp`, output size) builds this frame's estimate at each output pixel as a Gaussian (sigma 0.6
  output px) of the 3x3 input samples at their jittered positions, and scales its blend weight by the nearest
  sample's Gaussian weight (`conf`); the 100% path is the old code unchanged. SkyTaaPC's spare w's carry the jitter
  and the input size. The depth restore reads the RESOLVE's full-res depth (`skyTaaRestoreSet[2]`, one per ping-pong
  side). The sky pass's detail LOD uses the OUTPUT pixel (`CloudParams::skyLodScreenH`, v1's unread maxRenderDistM).
  Cost ~1 ms over the plain stretch (the full-res resolve). The plain low-res blit stays for TAA off / SKY_LITE.
  **In motion the nearest sample's weight is taken under a kernel one INPUT pixel wide** (`confWide`, 2026-10-29): under
  the 0.6-output-px kernel, at 50% (Low) a pixel whose nearest sample landed 1-1.4 px away moved by 0.06-0.25 of the
  moving weight, so the image lagged while moving; still views keep the narrow kernel. `display.sky_taa_debug 5` = the
  old weight (A/B). tstab (profile_log_1004j 37 / 38, 1920x1009): Low walk 1.26 -> 1.14, rise 1.39 -> 1.28, the rest and
  Medium within +-0.03 (`harness_runs/tiers/ts2`). Low stays ~20-30% above Medium on tstab: the 50% scale itself
  (full-res clouds, clouds at the window's size or 67% did not change that; 67% costs Medium's GPU time).
- **Point-like lights under the sky TAA (2026-10-04, user snapshots: far city lights blinking "like z-fighting").**
  Two benchmarks, run on BOTH sides of any change here (docs/HARNESS.md): the still-view flicker set
  (`harness_runs/rscale/bench4.satcmd` + `bench4.py`: Irvine horizon from 330 m, SF Bay from 1.2 km, SoCal from 1805
  km, Dallas from 3.5 km; % of pixels varying > 8 levels; TAA off reads 0%) and the climb set over clouds
  (`harness_runs/trails/r37|r38.satcmd`, tstab `rise` + `boostrise`; profile_log_1004j 37 Pacific cumulus from 2 km, 38
  Edmonton from 4.3 km). Committed state before the day: flicker 2.31 / 0.58 / 1.07 / 0.94, climb 1.60 / 0.85. Now:
  flicker 0.57 / 0.04 / 0.13 / 0.04, climb 1.65 / 0.91 (the moving frames differ from the committed ones only by
  cloud-march sampling grain, ~0.4/255 mean; checked by eye).
  - **The resolve** (`sky_taa.comp`, otherwise as committed): when NOTHING moved this frame — the CPU's exact test:
    the eye moved < 1 cm, the view did not turn, sim time <= ~1.5x; `eyeDelta.w` = 2 — the history is not clipped
    (a light smaller than a pixel is caught by only some jitter phases, and in the others the all-dark 3x3 box clipped
    its accumulated light away) and the flash rule uses the 3x3 mean without its brightest sample (a lone glint on a
    wave facet took its frame raw: white specks on the sea). Non-finite inputs are dropped (`fin`).
  - **The city lights** (`sat_sky.frag`): read at the UNJITTERED pixel only where the view is steeper than ~12
    degrees (`uvTaa0` / `taaDn`, gated on |cos| > 0.2 of the unjittered ray): the night map, the terrain contour, the
    land test and the pattern's point. Glitter points are at least an input pixel wide under the upscale, the
    glitter's presence ramps over a band of the hash, the steep-ground cut fades past a 400-1200 m footprint, `cFoot`
    takes the sphere's slope past ~100-400 m; city sprites are tested at 0.95 of their range (since 2026-10-06 against the unjittered half-res scene depth, below).
  - **Tried and REVERTED, each made rendering less stable** (the user: "over-optimizing the bugfix ... made rendering
    a lot more unstable"; trails climbing over clouds): (1) the unjitter at every view angle — the tangent-plane point
    slides kilometres toward a grazing horizon (Irvine 2.3% -> 4.6%); (2) a PER-PIXEL "still" (reprojection < 0.1 px)
    widening or dropping the clip and raising the flash threshold — climbing, the far clouds move less than that while
    the layer being crossed changes: trails and stair bands (climb 1.60 -> 3.87, and two settles of one view no
    longer agreed); (3) the flash rule against the history's neighbourhood; (4) restoring the 3x3 farthest depth with
    a wider disocclusion tolerance; (5) the disocclusion tolerance widened by the 3x3 depth spread; (6) the
    footprint's |cos| floor 0.2 -> 0.03. Most of these measured well on the still views alone.
  - **2026-10-06 (user: "z-fighting-like flicker at low angles, far away"):** the r35 view's worst pixels alternated
    ~240 / ~37 levels frame to frame on the 16-phase jitter cycle. Two causes, both at crests seen at a low angle (a
    ridge in front of the far city): (a) **city sprites against the restored depth** — it is this frame's JITTERED
    sample, so a light just behind a crest was hidden or drawn whole by turns. City sprites now take depth 0 (points
    write none) and are tested in `sat_point.frag` against the UNJITTERED half-res `sceneDepthImg` at 0.95 of their
    range (`fragManualDepth`), the renderScale < 1 path's test. Dilating the restored depth (3x3 nearest) only moved
    the flicker one pixel over; a grazing-dependent depth margin (up to half the range) changed nothing on r35 and
    worsened the Edmonton boost-climb 0.73 -> 1.11 — both dropped. (b) **the resolve's disocclusion test when STILL**:
    last frame's 2x2 jittered depths need not hold this phase's depth at an edge, so the history was rejected on
    every switch. When nothing moved (`eyeDelta.w` >= 2) the accepted range also takes this frame's 3x3 depths —
    item (5) above, but ONLY when still, so climbing is untouched (p37 1.65 / e38 rise 1.08, boost 0.68). Still
    flicker r35 0.57 -> 0.31% (worst pixel std 103 -> 34), others unchanged; time-averaged image |diff| 0.13/255.
    What is left on r35 is the EMA sawtooth of glitter points caught by one jitter phase in 16.
  - **Storm sea trails need RAIN and a slow eye (2026-10-13, issue 20261012_235712 shot 7):** the trail share
    (sat_sky.frag `gTaaSeaTrail`) is x smoothstep(0.1, 0.4, `gSeaRain`) — the map's rain over that point of the sea,
    the march's own rule, set by `oceanSeaState` — and the resolve fades it over 1-4 px of parallax. On any stormy
    sea, unclipped and held at the still weight "moving or not", the history was whatever stood over the water a
    frame ago: boosting at 22 km/s the clouds were dragged across the sea in stepped copies.
  - **"Still" / "frozen" come from the sim time itself (2026-10-06, `skyTaaPrevSimT`):** frozen (`eyeDelta.w` 3) =
    sim time did not change since the last resolve, still (2) = it moved at most 1.5x the frame step. They read
    `timePaused` and the time scale, but a cinematic holds `timePaused` while it sets sim time every frame (so does
    the harness's `path play`): the sea dropped its clip while the waves moved — grey crest trails (still-view
    pixels past 8 levels 15.6% -> 4.5% at Big Sur in that mode). Tried the same day and dropped: the glitter's
    along-view point size from the true footprint (cos floor 0.03, gCityFootA) — r35 0.31 -> 0.51%, city rise
    2.93 -> 3.10; a half-input-pixel upscale kernel — softer at 50% (detail -32%) for little.
  - **Vertical motion (2026-10-06, user snapshot: Glasgow at night, "smears moving up and down over city
    terrain"):** the resolve's moving weight also ramps with the PARALLAX — the reprojection's offset from where a
    pure rotation would put the pixel — over 0.1 -> 1 px (the total motion keeps its 0.5 -> 4 px ramp, so turns, pans
    and the far ground in a slow drift are unchanged). Climbing, the ground's reprojection is 0.1-0.5 px
    a frame, so the history sat at the still weight and was resampled at a fractional offset every frame — about ten
    frames of resampling blur stacked on the 1-px lights. tstab (Glasgow, 60 m/s, 20 deg down, motion vs settled):
    render scale 75% rise/fall 2.08/2.05 -> 1.67/1.66, 50% 2.74/2.73 -> 2.46/2.43; climb set p37 1.63/1.64 -> 1.55/1.62,
    e38 1.20/0.72 -> 1.04/0.66, Irvine rise 2.88 -> 2.80; still set unchanged. **Measure motion with
    `display.dynamic_resolution false`:** the user's settings have it on, and in a harness run (a GPU shared with other
    apps) it dropped the scale mid-path — the same up path scored 2.3 and 5.4 in two runs. In the app it also drops
    while moving (the moving cloud march costs more), and a lower scale is softer in motion: temporal upscaling only
    recovers its detail when still (sharpness 4.7 settled vs 3.1 moving at 50%).
  Debugging notes: print the worst pixels' value series (lit in 1 of 8 or 16 frames = the jitter; any other period is
  something else) and A/B with the history off (`set display.sky_taa_weight 1`); `Copy-Item` keeps a file's old
  timestamp, so restoring a shader from a backup does NOT rebuild it (touch it) — identical-to-the-digit results
  across a change mean it did not build. The harness is bit-deterministic run to run. `cloud_v2_resolve.comp` writes
  "no cloud" for a non-finite result (user snapshot: black squares at the limb after a fast climb; not reproduced).
- **Presets (2026-10-04, the user):** Medium renders at 67%, Low is Medium's settings at 50% with every effect on
  (aurora, beams, fog, terrain detail); Planetarium is the "everything off" tier. Measured (RTX 3070 Ti, 1600x900, the
  user's snapshots): Low 12.3 / 13.8 / 9.9 / 7.9 ms, Medium 17.8 / 18.2 / 13.8 / 11.2 (orbit storm, anvils, Dushanbe,
  Dallas night). First runs seed `SatelliteSim::recommendedPresetForDevice` (2026-10-06; static, so the
  startup chooser can call it before init): **Potato** for MoltenVK on a non-Apple GPU (the 2015 MacBook Pro's
  R9 M370X is DISCRETE, 2 GB), **Planetarium** for integrated / CPU / virtual GPUs and discrete ones with
  <= ~2 GB of VRAM, **High** for discrete GPUs with >= 5.5 GiB of device-local memory (2026-10-20: a 6-8 GB card;
  High is the compiled defaults, the author's tuned look), else Medium — logged as `graphics: recommended preset ...`. Crash recovery steps the preset
  down one tier (`crashRecoveryPreset`: full -> Planetarium -> Potato; it always forced Planetarium). The tutorial then ends with a "Graphics" card
  (`TUT_GRAPHICS`, shown only when it starts on Planetarium or Potato) saying there are no clouds and pointing at
  Settings > Display > Preset.
- **Automatic render scale (2026-10-04, `updateDynamicResolution`, Display "Automatic render scale", keys
  `display.dynamic_resolution` (off) / `dynamic_target_fps` (60) / `dynamic_min_scale` (0.5)):** keeps the GPU frame
  under 0.9 x 1000 / fps by 5% steps (down as far as a 30% fixed + 70% x scale^2 model says, after 0.75 s; up one step
  when predicted under 85% of the budget, after 1.5 s). The clouds' history is BLITTED across a resize
  (`recreateComputeScaledTargets`), so a step does not restart them. The orbit storm snapshot settled at 55% (13.8 ms).
- **Things that move on their own (2026-10-21, issue 20261021_174219: "crazy trails on all models" with TAA on).** The
  resolve reprojects a static world; meshes (satellites, rockets, pads) and the plume / flame light have no motion
  vectors. `sat_sky.frag -DSKY_TAA` marks them in the colour's alpha (0.875: a mesh pixel, or a composite texel with
  the emissive flag, gathered), the resolve stores their depth NEGATED (`taa_depth_restore.frag` takes abs) so the next
  frame knows where they were, and wherever the 3x3 or the history's 2x2 holds one (`dynNb`) the still view's
  exemptions are off: no unclipped history, no widened depth test. A descending ship from below was a translucent,
  dotted ghost: its history (the sky) passed the widened test and was kept unclipped at the still weight.
- Run with `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` after touching any of this; the harness run's
  `app_stdout.txt` holds the messages. `scripts/sky_taa.satcmd` (set `display.sky_taa` with `true`/`false`).

## Subsystem: Resolution Scaling

Settings → Display → "Render scale" (50%-100%, default 100%, top of the tab — a user-facing perf
option, not a debug tool). Only the sky/terrain/ocean/cloud-composite background scales;
satellites, stars, and UI always render at native resolution, no exceptions — the explicit design
goal, given the earlier-session concern about losing tiny satellite point fidelity to a whole-
frame downscale.

**Two paths below 100%** (both run from `SatelliteSim::recordPrePass`, a `Simulation` hook, before the main
render pass opens; the main pass then uses `ctx.renderPassLoad`, which LOADs colour so the prepass's result
survives — same attachment formats, so the same `ctx.framebuffers` stay compatible):
- **Temporal upscaling (the default, sky TAA on with the full sky shader):** the sky pass renders jittered into the
  top-left `skyTaaInExtent()` of the TAA's input images and `sky_taa.comp` resolves to the full output size — see
  "Sky TAA" above. The depth is restored at full resolution from the resolve (`taa_depth_restore.frag`), so
  satellites, stars and planets are depth-tested by hardware exactly as at 100%.
- **Plain upscale (TAA off, or SKY_LITE):** the background renders into a low-res offscreen target and is blitted
  (`vkCmdBlitImage`, linear) into the swapchain. **Depth is not blitted** (depth-format blits are not
  spec-guaranteed, and this path exists for weak hardware), so the point draws test terrain manually against the
  half-res `sceneDepthTex` (`PointDrawPC::manualTerrainTest` = 1).
At exactly 100% with TAA off the prepass does nothing and the code path is the original one.

**`gl_FragCoord` gotcha — read this before adding any new `gl_FragCoord`-based lookup to
`sat_sky.frag`.** `gl_FragCoord.xy` is relative to whatever framebuffer the CURRENT draw call
targets, not always the full swapchain — a real bug shipped and was fixed same-session: the cloud
composite sample divided `gl_FragCoord.xy` by `textureSize(cloudTargetA,0)*2.0` (an assumed full-
res constant, since `cloud_march.comp`'s own dispatch is unaffected by `renderScale`), which
silently broke the moment the background could render into a smaller offscreen target — clouds
drifted off-center, fully distorted at 50%. The sky render-target size is now `cloud.skyScreenW`/
`skyScreenH` in the CloudParams UBO (`skyLowResExtent` when the scaled prepass runs — recordPrePass
and recordDraw's Pass 1 are mutually exclusive, so one per-frame value suffices — else
`ctx.swapExtent`); any `[0,1]`-normalized UV in `sat_sky.frag` derived from `gl_FragCoord` divides
by `vec2(cloud.skyScreenW, cloud.skyScreenH)`, never an assumed constant. (A fixed-frequency noise
seed like the terrain-march jitter lookup, `gl_FragCoord.xy * (1.0/128.0)`, is fine as-is — no
total-resolution assumption baked in, not a normalized UV.) The point shaders (`sat_point.frag`/
`star_point.frag`) never render scaled, so they keep a plain `screenSizePx` (= `ctx.swapExtent`) in
`PointDrawPC`.

`screenSizePx` originally lived in `SatDrawPC` (which grew 132→144 for it); it moved to the
CloudParams UBO / `PointDrawPC` in the 128-byte push-constant split. `buildSkyDrawPC(ctx)` /
`buildPointDrawPC(ctx)` fill the two draw push constants; the CloudParams UBO fill in
`recordCompute()` sets `skyScreenW`/`H` from `renderScale`.

See `TERRAIN_PLAN.md` session 29 log for the full design writeup and the bug's root-cause
narrative.

**The half-res compute passes follow it (2026-10-04, "Clouds follow render scale", `display.clouds_follow_render_scale`,
default OFF since 2026-10-20 — on for Low / Medium / Planetarium / Potato, off for High / Ultra, set by
`applyGraphicsPreset`; toggling it, or the render scale, makes the preset Custom):** scene depth (+ its quarter seed), the cloud march targets and clouds v2's screen images are sized by
`computeHalfExtent()` = half of the SCALED extent, recreated by `recreateComputeScaledTargets()` (onResize, and
recordCompute's check before anything is recorded when the extent changes). Every consumer already sized itself from
`imageSize`/`textureSize` or sampled by UV. The march's noise LOD keeps the 100% pixel (`cv2.motion.w` = march pixel /
100% pixel x `fp`): with the coarser footprint the storm cumulus texture filtered away. Measured (RTX 3070 Ti,
1600x900, user snapshots, full-rate march): orbit storm total 32.4 / 20.6 / 11.9 ms at 100 / 75 / 50% (fixed-size
clouds at 50%: 28.0), Dushanbe snow 28.3 / 17.4 / 10.9 (22.6), Arizona anvils 34.0 / 21.8 / 14.0 (28.1). Rain/snow
drops (drawn in the half-res composite) get softer and larger below 100%.

---

## Subsystem: Weak-Hardware Sky Tiers (Potato / SKY_LITE)

`sat_sky.frag` is ~2900 lines. On a **2015 MacBook Pro (AMD Radeon R9 M370X / GCN 1.0, macOS 12,
MoltenVK)** it compiles to one Metal fragment function whose register pressure collapses wavefront
occupancy — measured **~490 ms/frame, the entire frame**. This is not tunable by any quality slider
or `debugDisableMask` bit: those skip *execution*, not compiled *size*, and the constraint is peak
VGPR count + total texture-fetch latency that must be hidden, not instruction count. See
`SKY_OPTIMIZATION_PLAN.md` for the full investigation and `.gputrace` capture workflow
(`tools/make_capture_bundle.sh` — needs full Xcode to read).

Two stand-in fragment shaders, both bound through **`skyBgPipeLayout` / `skyDescSet` unchanged**
(each declares only the bindings it reads) and selected in `recordDraw()` Pass 1 by
`debugDisableMask` bit:

| Tier | Bit | Pipeline | Shader | Notes |
|---|---|---|---|---|
| **Potato** | `262144` | `skyBgMinimalPipeline` | `shaders/sat_sky_minimal.frag` (own file, ~370 lines; the Sept-2026 shader + only what today's bindings and depth require: the weather cube at binding 7, the unified depth, and its own DEM-only eye height, `minimalEyeHeight`) | closed-form analytic atmosphere (Kasten-Young airmass, one 32-tap arithmetic loop — no raymarch), day/night + city-detail textures, one flat drifting cloud shell w/ terminator lighting, cheap ocean (1 noise-tap slope + Fresnel + Blinn glint), textured moon, verbatim `lensFlare()`. **~60 FPS.** No Milky Way / volumetric clouds / aurora / airglow / real ocean waves — those don't fit the GCN1 occupancy ceiling (~31–32 KB SPV; the Milky Way's `atan2`/`asin` + panorama fetch is the specific thing that broke it). |
| **SKY_LITE** | `524288` | `skyBgLitePipeline` | `sat_sky.frag` **recompiled with `-DSKY_LITE`** → `sat_sky_lite.frag.spv` (2nd `add_custom_command` in CMakeLists) | `#ifdef SKY_LITE` cuts inside the real shader: (2026-10-06) every v1.2 ground feature — the terrain march (`dbgSkipTerrain()` is constant true), close-up materials and terrain debug views, the sea's sky-reflection march (`dbgSkipOceanRefl()` constant true) and shore, the beam ground spots, the whole procedural city (layout, glitter, roads, farms, beaches, roof PV, solar parks: the night map + the tiled city detail textures carry the cities, as in v1.1) and the mesh composite (the CPU draws no meshes or env probes there, `kNoMeshBits`); and since 2026-09: Milky Way block, 64-bin satellite sky-glow loop, cloud layer loop `3→0` becomes `1→0`, the 3×3 `cloudTargetA/B` rgb blur → single tap, aurora surface glow (`auroraGlowAt` on terrain+ocean), zenith-ambient `N_ZT` 4→2, and the per-atmosphere-step **green/sodium airglow** (two `warpPerlin3` masks/step — the dominant cost) — city-glow upwelling KEPT but only on the first 3 march steps (since 2026-10-21 the green band comes back in CLOSED FORM, after the loop: the column along the ray from the tangent height / zenith angle at the shell, no noise — the night limb's horizon line from orbit, +3 KB of SPIR-V; a ray down onto the Earth keeps its crossing only near the limb, cos Z at the shell 0.18-0.32: the whole night disc read faintly green from orbit). **2 FPS → ~55 FPS** on the target. |

**Keep these tiers protected (2026-10-06).** `sat_sky_lite.frag.spv` had grown 142 KB (v1.1.0) → 567 KB and
the minimal shader had taken on terrain v2's water code, unnoticed, because each new feature was gated by a
RUNTIME knockout or `if (false)` — glslc (no `-O`) still compiles every call, and the driver must allocate
registers for the largest path. The rule: a new feature in `sat_sky.frag` is `#if`'d out of SKY_LITE (not just
gated on a uniform), nothing new goes into `sat_sky_minimal.frag`, and its CPU work is skipped for those presets.
The build enforces it: `cmake/CheckSpvSize.cmake` runs after both compiles and fails past `SKY_LITE_MAX_BYTES`
(225 KB) / `SKY_MINIMAL_MAX_BYTES` (46 KB) in CMakeLists.txt — ~10% over the trimmed 199 / 41 KB (glslc versions
differ by a few percent). As of 2026-10-21 they are ~212 / ~41 KB (SKY_LITE grew with the UBO's pad/coast blocks and
the closed-form airglow limb): ~13 KB of SKY_LITE headroom left. Raise a budget only with a measurement on the target hardware.
`tools/harness/scripts/low_tiers.satcmd` is the check: Potato / Planetarium / Low at a day, night-city, orbit and
storm view, perf + captures, and run once with `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`. First run
(RTX 3070 Ti, 1600x900, GPU frame): Potato 0.8-1.0 ms, Planetarium 1.4-3.0 ms, Low 4.8-6.7 ms (coverage forced
1.6 in its views); no validation errors (3 pre-existing `ShaderOutputNotConsumed` warnings at pipeline creation).

**The CPU side of the tiers (2026-10-06).** Knockout 32768 (both tiers set it) switches off ALL of clouds v2
host-side: `recordCloudsV2` records only the weather evolution (the flat decks read the cube) — no march,
resolve, far layer (`cloudFarBlend()` is 0), lightning pass or tiles, and none of their barriers (each restarts
the encoder on MoltenVK); bolts, rain particles, thunder, the rain ambience and the Sun's cloud transmittance key on
`cloudsV2LightningActive()` / `cv2LightningRanThisFrame`, and `cloud_march.comp` ignores the stale resolved
images. Potato also skips `cloud_march.comp` itself (the minimal sky never reads its targets) after clearing them
once to "no cloud" for the point passes (`cloudMarchTargetsClear`). Knockout 1024 (Potato) clears the depth images
to `kNoSurfaceT` once when it is switched on (they kept the last frame) and, outside Potato, fills
`terrainFrameBuf` from the CPU ground for its GPU readers (city sprites, far layer, lightning).

262144 wins if both bits are set. The always-on `Log::line` breadcrumb in `recordDraw` reports
`MINIMAL` / `LITE (SKY_LITE)` / `FULL sat_sky.frag` on any change (into `satlight_log.txt`) — this
is also the instrument for `potato-mode-intermittent-slow-start` (see memory).

**Preset wiring** (`applyGraphicsPreset`, `SatelliteSimUI.cpp`): **Potato** sets `kBitMinimalSky`
plus every compute-side knockout; **Planetarium** sets `kBitLiteSky` + `kBitCloudMarch |
kBitCirrusMarch` (coverage 0 at that tier) + `viewSamplesMin/Max 4/10`. Medium and up keep the full
shader unchanged. `GraphicsPreset::Potato` is enum value 6 (appended after `Custom` to preserve
persisted int indices); `kGraphicsPresetNames` and the preset-row UI list it first.

`skyLowResPipeline` (renderScale < 1.0 prepass) is **not** given a lite/minimal variant — both
those presets force `renderScale = 1.0`, so the prepass never runs for them.

**What the light tiers offer (2026-10-29, workstream C).** Every Watch / Go on Planetarium and Potato was walked
(`harness_runs/tiers/vis1`); what lands on nothing is disabled with its reason as the tooltip, through
`SatelliteSim::tierNeedReason(needs)` (`kNeedClouds` = the volumetric clouds, `kNeedAurora` = knockout 32,
`kNeedEclipse` = not the Potato sky):
- **Weather** (`weatherUnavailable()` = `cloudsV2Off()`): the HUD button is dimmed, the window shows only a notice and
  runs nothing (no radar, no storm search), the Settings Weather tab's label is dimmed with a notice on top.
- **Bookmarks** (`defaultBookmarkNeeds(id)`, by the shipped ids so seeded copies are gated too): Beam-lit Snow,
  Rainbow, Typhoon need clouds; Solar Storm and The Sun's Fury need the aurora; on Potato the Great Australian
  Eclipse and the Blood Moon (sat_sky_minimal.frag draws neither the Moon over the Sun nor the Earth's shadow on it).
  Harness `bookmark go` fails with the same reason.
- **Sky events**: solar and lunar eclipse Watches on Potato (`skyKindNeeds`; `skyEventWatch` refuses too).
- **Missions**: the Sun's Fury card's Watch (the storm night is the aurora).
- **Potato draws the point sources again**: its recordCompute branch now runs `kesslerFillDust` (the CPU fill of
  kFireBuf: rockets, re-entry fireballs, meteor heads, comet heads, Moon-base glints, the supernova). It skipped it
  with cloud_march.comp, so those Watches showed an empty sky and Betelgeuse went dark at its supernova. The Gaussians
  it also lists (plumes, streaks, dust) are not drawn on Potato; launch plume parcels are not built there.
- Everything else draws: comets (tails included), conjunctions, occultations, Moon glints, meteors (heads),
  launches (points, + plumes and flames on Planetarium), re-entries, the twilight launch, Starmind; Planetarium draws
  both eclipses. The validation layer is clean on both tiers through all of them.

**Preset benchmark** (2026-10-29, RTX 3070 Ti, uncapped, one app alone on the GPU; `harness_runs/tiers/bench2`, script
generator in the workstream-C report). GPU ms at 1600x900 / 1920x1009:

| view | Potato | Planetarium | Low | Medium | High | Ultra |
|---|---|---|---|---|---|---|
| day, Alps 50 m | 1.1 / 1.1 | 1.9 / 2.1 | 8.5 / 10.0 | 11.8 / 15.0 | 19.6 / 24.2 | 21.4 / 26.4 |
| LA night 2 km | 0.9 / 1.3 | 2.1 / 2.8 | 6.4 / 7.9 | 8.7 / 11.2 | 14.6 / 19.3 | 15.9 / 21.5 |
| orbit 420 km | 1.3 / 1.6 | 2.5 / 2.9 | 6.2 / 7.4 | 11.1 / 14.8 | 20.2 / 26.7 | 24.0 / 30.6 |
| storm (issue 20261023_230322) | 1.6 / 1.7 | 2.5 / 2.9 | 11.3 / 12.4 | 15.5 / 18.5 | 22.1 / 26.7 | 30.7 / 32.9 |
| launch Watch | 1.6 / 1.8 | 4.1 / 5.3 | 8.6 / 9.6 | 10.4 / 12.2 | 15.8 / 19.5 | 18.3 / 23.3 |
| 1 h/s, Shanghai 1 km | 2.4 / 2.6 | 3.6 / 4.2 | 7.3 / 8.4 | 11.5 / 13.9 | 23.7 / 28.8 | 26.4 / 32.8 |
| Great Comet Watch | 2.0 / 2.4 | 2.9 / 3.6 | 8.9 / 10.1 | 11.6 / 13.7 | 16.6 / 20.0 | 18.3 / 21.8 |
| 2037 eclipse Watch | 1.0 / 1.1 | 2.1 / 2.6 | 3.6 / 4.1 | 4.4 / 5.5 | 6.7 / 8.4 | 6.8 / 8.6 |
| supernova Watch | 1.0 / 1.2 | 2.2 / 2.6 | 3.6 / 4.1 | 4.7 / 6.0 | 7.9 / 10.1 | 8.1 / 10.4 |

The main-thread CPU (summed buckets) is 0.5-2 ms everywhere but 1 h/s (~4 ms Potato, ~6-7 ms the rest: Kessler,
launches, storms). Potato's frame is mostly fixed cost; Planetarium is 2-5 ms; Low is ~0.6-0.75x Medium.

### All-platform changes that came out of this pass (land on `main`, not tier-gated)

- **Sun corona bridge** (`sat_sky.frag` "Sun disc + atmospheric corona"): three stacked
  `pow(cosA, 1800/320/55)` lobes peaking at ~disc brightness, added between the hard disc and the
  `×0.12` wide `corona` Gaussian. Without it the disc dropped straight to the dim corona at its
  edge — a hard brightness step that read post-tonemap as a dark ring / "cutout." Same shape as
  `sat_sky_minimal.frag`'s Potato corona so the tiers match.
- **Moon `squish` dead code removed**: the disabled (`squish = 0`) atmospheric-refraction block
  still ran an `asin` + (below 15° elevation) two `tan` + `radians` to feed the identity
  `dir.z * (1.0 + 0.0)`. Gone; ray/disc intersection uses `dir` directly.
- **Cloud-shadow blur 5×5 → 3×3** (`kShadowBlurSpread = 1.7` keeps the ~radius-2 footprint;
  centre tap reuses the already-sampled `cloudBCenter.a`): −17 texture samples on every ground-hit
  pixel, the largest sample-count cut in the file. If ocean graininess returns, strengthen
  `cloudGroundShadow`'s dither in `cloud_march.comp` rather than widening this back out.
- **Push constants trimmed to the 128-byte `maxPushConstantsSize` floor** (2026-09-07). The same
  old AMD integrated parts that need Potato/Planetarium also report exactly the Vulkan-guaranteed
  minimum `maxPushConstantsSize` of 128, and `SatDrawPC` (176) / `CloudMarchPC` (148) both blew
  past it — the app could not create those pipeline layouts at all. `SatDrawPC` split into a
  128-byte sky core + a new 128-byte `PointDrawPC` for the point pipelines; `CloudMarchPC` trimmed
  to 128. Every per-frame-uniform tail field (`debugDisableMask`, `screenSizePx`,
  `skyGlareVisibility`, the `beam*` scalars, `mwSuppressEased`, `showBeamDebugRays`,
  `cloudShadowRangeM`) moved into the CloudParams frame UBO — see the **SatDrawPC** / **PointDrawPC**
  entries under "Subsystem: GPU Orbital Pipeline → Push constants" and the "Push-constant relief"
  block in `GpuCloudParams`. **Consequence for future work:** `SatOrbitPC` and `SatFlarePC` are
  already at exactly 128 — any new push-constant field on any of these structs must go in a UBO,
  not the push constant. Prefer the CloudParams UBO where the consumer already binds it.

---
