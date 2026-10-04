# Findings from the October 2026 documentation pass

These came up while reading the code to write this documentation set. Nothing here was changed.

"Verified" means the code was read and the claim confirmed. "Reported" means it was found in a full read of
the relevant file but not re-checked separately.

## Possible bugs

| # | Where | Finding | Status |
|---|---|---|---|
| 1 | `recordCloudsV2` (`SatelliteSimCloudsV2.cpp`, lightning dispatch → march) | **No barrier between `cloud_v2_lightning.comp` and the v2 march.** The lightning pass writes `cv2SunProf` (and the rain map) into `cv2FlashBuf`; the march reads `cv2SunProf` through binding 12. A barrier happens to be recorded only if the light volume (god rays, default off) or the adaptive tiles pass (moving above 30 km) runs in between. In an ordinary ground-level view there is a write→read hazard on the Sun-path profile, which shadows clouds near the eye's line to the Sun. | **Verified** |
| 2 | `cv2FieldLow` / `cv2ColumnSigma` vs the shell | The shell top `shell.y` is max(base + span·1.35) over the type table, ≈ 15.2 km. It includes neither the towers' overshoot (+0.8 km) nor any terrain lift of low tops. Where the tropopause factor is 1.35, an overshooting dome or a terrain-lifted top can rise above the shell and be clipped by `cv2ShellSegments`. | Shell formula verified; clipping not observed at runtime |
| 3 | `beamFinish` (`cloud_v2_march.comp`) | For thin (ice) samples the beam optical depth uses `f.sigma·(1 − 0.45·thin)`, but `f.sigma` was already delta-scaled by the same factor before shading. That scales ice twice for beam light (the key light scales it once). | Verified (both lines present) |
| 4 | `glare_mesh.frag`, `mesh_bloom.frag` | Mesh bloom and mesh glints are not occlusion-tested. The header says this is deliberate. A mesh hidden behind terrain or opaque cloud in `sat_sky.frag` (the march clamps only to the half-res depth) still seeds bloom and glints, while sprite glare and bloom are tested. | Verified (stated in the header) |
| 5 | `star_point.frag` | Stars test cloud with the **filtered** cloud alpha / transmittance (any alpha ≥ 0 hides), not the per-texel `cloudPointVisibilityAt` that satellites, flare sources and glare use. At cloud outlines the filtered alpha mixes a distance with the no-cloud sentinel. | Verified (filtered `texture()` reads) |
| 6 | `recordCompute`, `activeSatCount == 0` early return | Skips the flare-source pass and blur, but `recordDraw` still composites `flareSourceImg`, which then holds stale contents. | Reported |
| 7 | `cloud_march.comp` above full far blend | The lightning pass is not dispatched at `farBlend ≥ 0.999`, but `lightningCS` still runs on the previous flash list. The cloud glow is gated off; ground strokes from a stale list could still draw. | Reported |
| 8 | `sat_sky.frag` exposure in eclipses | C++ `skyExposure()` scales dayness by the eclipse sky light (`moonEclipseSkyObs`), but `sat_sky.frag`'s own `dayness` has no eclipse term. During totality, the sky pass's exposure and every C++ consumer of `skyExposure()` (probe and post-tonemap exposure) disagree. | Reported |
| 9 | Knockout bit 1024 | The CPU skips the scene-depth dispatches entirely, so the shader's "write `kNoSurfaceT`" branch is dead. Toggled at runtime, `sceneDepthImg` keeps the last real frame's depth, not "no surface". | Reported |

## Dead or vestigial code

- `gCv2NoFar` is set by the ground shadow and read nowhere (the far-field substitution it disabled is gone).
- `prevResDepthTex` (march binding 16) is declared in `cloud_v2_march.comp` and never read there; the tiles
  pass reads it.
- `anchorCol` is uploaded and unused (the tower lattice is a fixed cube map).
- `SatFlarePC::meshSatIdx` / `meshSpriteKeep` are pushed as −1 / 1 and unused (`MeshKeepBuf` replaced
  them); `MAG_REF` in `sat_flare.comp` is unused.
- `kShadowBlurSpread` in `sat_sky.frag` is declared and unused.
- The `envelope` mode of `terrainMarchDetailed` has no caller.
- Timestamp slot 2 ("Beam cloud block (retired)") measures two buffer fills.

## CLAUDE.md out of step with the code

| Section | Says | Code |
|---|---|---|
| Frame Loop Order | `recordPrePass` is "renderScale < 1.0 only"; `recordScreenshotCopy` is a no-op unless a shot is pending; lists cloud_march as one dispatch | `recordPrePass` is also the sky TAA path; the exposure meter blits every frame; the cloud bucket is ~9 passes; the quarter-res depth pass, env probes, sharp reflections and city sprites are missing from the list |
| GPU Performance Profiling | SatelliteSim writes slots 1-3 in `recordCompute` and 4 in `recordDraw` | slots 1-5 in `recordCompute`, 6 in the prepass / `recordDraw`. With TAA, the resolve + blit fall in "Satellite + star draw"; mesh scene and probes fall in "Scene depth" |
| Unified scene depth | "satellite points write their own range" | points **test** at their range; no point pipeline writes depth |
| GpuSatVisible layout | `[16] color, angularSize, rangeM, pad` | the fourth field is `meshPx`, which becomes `glareFlare` after `sat_flare.comp` |
| Occlusion: one shared depth buffer | terrain/ocean only | the half pass also mins the full-res mesh distance; the quarter pass does not |
| Cloud Shadows | `cloudGroundShadow`, 12 steps, 3e-3 | `cloudGroundShadowV2`: 20-60 + 12 steps on the v2 field (the review entries are right) |
| Cloud-light tile culling | in `cloud_march.comp`, feeding `beamCloudLighting()` | in `include/beam_cloud_lights.glsl`, called from `cloud_v2_march.comp`; `beamCloudLighting` and `cloudMarchCS` are gone |
| The sea is not terrain | the gate is `terrainH0 <= 0`; "land cannot read 0" | the gate is the water mark (`hMip3 ≤ kTdWaterMark`); land at 0 m stays terrain |
| Terrain current state | ocean has Blinn-Phong glint (exp 300) + Schlick | Beckmann glint; Fresnel is `min((1 − n·v)³, 0.5)`; exp-300 Phong survives only on wet paddies |
| Weak-hardware: shadow blur | 3×3 at spread 1.7 | [1 2 1] tent at one texel |
| Clouds v2 "Passes" | resolve weight 0.1 rising to 0.35 | still 0.05 (≤ 0.15, × 0.4-1 by altitude), moving 0.4 (≤ 0.5), ramp 0.05-1 px |
| Clouds v2 pass 16 | cirrus gives way over storms | only with the map-fed anvil shield on ("Anvils" > 0; default 0) |
| Clouds v2 lightning | eye Sun transmittance by a 24-step march | 32 quadratic steps to 400 km |
| Clouds v2 review 8b | ground shadow uses `gCv2NoFar` | that flag no longer does anything |
| Clouds v2, weather cube | mip 0 "5 km texels" (and derived mip sizes) | ~12 km at a face centre, ~5-6 km at corners; derived labels ~2.5× small at face centres |
| Clouds v2 sub-texel jitter | off "in fast flight" | off whenever the eye moves > 1 m a frame |
| Clouds v2 beam shafts | integrated "inside the march" | a closed-form per-ray pass after the march loop, using its transmittance checkpoints |
| Terrain v2 P1 | night sky light 0.25 of the full Moon | default 0.2 |
| Sky descriptor set | 29 bindings (0-28) | 30: binding 29 is the far cloud layer |
| Review 12 | point sources test cloud per texel | true for satellites, flare sources and glare; not stars (finding 5) |

## Stale comments in shaders

These are minor, but they mislead a reader:
- `cloud_march.comp` header: it describes the v1 march.
- The `occKm` comment says it uses the later of half-opacity and mean; the code uses the mean.
- `cloud_v2_march.comp` header: says IGN start jitter (it is blue noise); `TerrainFrame.w` is described as
  the rain rate, which is no longer written.
- `cloud_v2_far.comp`: "only the low layer" (it adds the mid and high layers).
- `terrain_detail.glsl` header: three erosion octaves (there are two), the depth pass using
  `envelope = true` (it does not), refinement by bisection (it is regula falsi).
- `scene_depth.comp`: `terrainFrame.w` = rain rate, DEM 21600×10800.
- `terrain.glsl`: water height "forced to 0" (it is the body's level).
- `sat_sky.frag`:
  - target A/B channel descriptions near the bindings;
  - a "3×3 box blur" on the cloud read;
  - "B.a currently 1.0";
  - "12-step" shadow;
  - the loop "from the observer";
  - earthSpecTex as an "ocean mask";
  - `moonDirENU.w` as "illuminated fraction";
  - the seed "3×3 texels".
- `star_point.vert`: `moonDirENU.w` described as the illuminated fraction; it is the Moon's angular radius
  there.
- `clouds_v2.glsl`:
  - the lobe sizes quoted for a 7 km shape period (now 1900 m);
  - "half the flow" for the weather lookup (it takes all of it);
  - stormProx "0 for the eye's rain";
  - `CV2Field.rain` described as a share (it acts as a flag).
