# Known issues

Open problems, suspected bugs and places where the documentation sources disagree with the code. Each item
carries the date it was recorded and the page it affects. This is a maintained list, not an archive:

- A wiki pass that finds an item resolved **deletes it here** and adds a dated entry to
  [Design notes](design-notes.md) saying how it was resolved.
- An item marked *verified* was checked against the code on the date given; *recorded open* means it was
  written down as open in the working notes (`CLAUDE.md`) or the October 2026 documentation pass and has not
  been re-checked since.
- Accepted photometric error is not listed here; it lives in `data/benchmarks/KNOWN_RESIDUALS.md` and on
  [Results and known residuals](../accuracy/results.md).

## Suspected bugs

Found while reading the code for the October 2026 documentation pass (`docs/rendering/FINDINGS.md`); the ones
fixed since are in [Design notes](design-notes.md).

| Issue | Affects | Status |
|---|---|---|
| The cloud shell top (about 15.2 km, from the type table) includes neither the towers' overshoot (+0.8 km) nor the terrain lift of low tops, so an overshooting dome or lifted top can be clipped by `cv2ShellSegments`. Not observed at runtime. | [The cloud field](../rendering/clouds/field.md) | formula verified 2026-10-03 |
| Beam light on thin (ice) samples applies the delta-scaling factor `1 - 0.45 thin` twice in `beamFinish`; the key light applies it once. | [Weather, rain, lightning, fog](../rendering/clouds/weather.md) | verified 2026-10-03 |
| Mesh bloom and mesh glints (`mesh_bloom.frag`, `glare_mesh.frag`) are not occlusion-tested (deliberately, per the header), while sprite bloom and glare are. A mesh hidden by terrain or opaque cloud still seeds bloom and glints. | [Satellite meshes](../rendering/satellite-meshes.md) | verified 2026-10-03 |
| Stars test cloud with the filtered cloud alpha, not the per-texel `cloudPointVisibilityAt` that satellites, flare sources and glare use; at cloud outlines the filtered alpha mixes a distance with the no-cloud sentinel. | [Points, bloom and glare](../rendering/points-bloom-glare.md) | verified 2026-10-03 |
| Esc while a key binding is listening for input quits the app: `App::cbKey` closes the window on Esc before the sim can cancel the rebind (`capturesKeyboard()` does not cover rebind listening). | [Controls](../using/controls.md) | verified 2026-10-03 |
| A named graphics preset is re-applied at every launch, overwriting a changed render scale and the four terrain-detail strengths (changing render scale does not switch the preset to Custom). | [Graphics settings](../using/graphics-settings.md) | verified 2026-10-03 |
| HQ photos and HQ cinematic exports set `full_rate_above_km` 0 and `sparse_when_still` 0, but with `adaptive_rate` on (the default) `fillCloudsV2Params()` turns full rate into adaptive whenever history is valid and the eye is still. No tile votes, so the photo is the sparse grid converged through history. | [Temporal resolve](../rendering/clouds/temporal.md) | verified 2026-10-03 |
| `cloud_v2_weather.comp` reads `earthSpecTex.r` as a 0/1 ocean fraction, but the binding is the signed-distance water map (0.5 at the shore), so the bake's ocean term (type classification, afternoon convection) is approximate at coasts. | [The cloud field](../rendering/clouds/field.md) | verified 2026-10-03 |
| Moonlight on ground and clouds is not dimmed during a lunar eclipse (`moonDirENU.w` comes from the phase alone). | [Moon and eclipses](../rendering/moon-and-eclipses.md) | verified 2026-10-03 |
| `satEciAt()` at lock-window start instants uses the plain float phase product, not `orbitPhase()`: a few hundred metres of position error in target selection. | [Reflectors and beams](../simulation/reflectors.md) | verified 2026-10-03 |
| `beam_self_march.comp` traces beams to the sea-level sphere while ground spots land on the target's own radius. | [Reflectors and beams](../simulation/reflectors.md) | verified 2026-10-03 |
| Only RandomShell assigns tumble axis, rate and phase: a `tumble`-law type flown in a Walker or Disk shell never spins. | [Attitude](../simulation/attitude.md) | verified 2026-10-03 |
| File-level loader errors (bad or missing `constellations.json` or `reflector_targets.json`, unknown shell type, attitude or target kind) go to stderr only, not `satlight_log.txt`, so Windows modders may never see them. | [Modding](../modding/index.md) | verified 2026-10-03 |

## Rendering: open visual problems

### Clouds

| Issue | Affects | Recorded |
|---|---|---|
| Fine contour ripples ("wood grain") on smooth storm domes and tower surfaces, most visible with low lobe strength. Not the march steps, the anvil, the mid or high layers, or the light march. | [The cloud march](../rendering/clouds/march.md) | 2026-09-28 (pass 14), again 2026-09-29 (pass 18) |
| Dome tips seen from above show pits and rings; a thin anvil (under 1.5 km) streaks horizontally at grazing views. | [The cloud field](../rendering/clouds/field.md) | 2026-09-28 (pass 15) |
| Concentric rings looking down from inside the anvil (not the sun-colour cache). | [The cloud march](../rendering/clouds/march.md) | 2026-09-28 (pass 16) |
| Storm cost: overlooking storms the cloud march was 30-85 ms at full rate before the perf sprint; after it the user's 68 ms view measured 22.5 ms. The worst frames still spike to 18-22 ms of cloud march against a ~12 ms mean. | [The cloud march](../rendering/clouds/march.md) | 2026-09-29, 2026-10-02 (review 22) |
| Floating anvil fragments far from any tower. | [The cloud field](../rendering/clouds/field.md) | 2026-09-29 (pass 17) |
| The mid layer's base, seen from just below it, draws a dark grazing band across storms behind it. | [The cloud field](../rendering/clouds/field.md) | 2026-09-29 (pass 18) |
| The dominant tower's walls are near-vertical up close. | [The cloud field](../rendering/clouds/field.md) | 2026-09-29 (review 4) |
| Cumulonimbus heads shimmer at a grazing Sun: long light steps land in the lit skin under the top or above it (two settles of one view differ by 7.4 levels at 4 light steps). | [The cloud march](../rendering/clouds/march.md) | 2026-09-30 (review 16) |
| Specks along cumulus edges seen from inside cirrus (9 km at 62 N): low cloud glimpsed through gaps at the high layer's edges. Reduced about 40%, not gone. | [Temporal resolve](../rendering/clouds/temporal.md) | 2026-09-30 (reviews 9-10) |
| From 20 000 km the limb shows alternating half-res columns in cloud. Not the far-field reads, the march settings, the TAA or the rate. | [Temporal resolve and the far layer](../rendering/clouds/temporal.md) | 2026-10-01 (review 18) |
| The high layer's regime is still Perlin noise, not the imagery morphology, and the morphology is not wind-aligned (streets follow the cube face). | [The cloud field](../rendering/clouds/field.md) | 2026-10-01 (review 17) |
| Cloud fractal statistics from orbit fall short of MODIS (perimeter dimension ~1.4 vs 1.47-1.73, fewer holes): the missing 1-5 km structure is below the half-res pixel. | [The cloud field](../rendering/clouds/field.md) | 2026-10-01 (review 17) |
| The far cloud layer has no Cb towers, no map-fed anvil shield, no ground shadows and no rain, and does not reproduce the march's scattered small puffs at ~2000 km. | [Temporal resolve and the far layer](../rendering/clouds/temporal.md) | 2026-10-01 (review 18) |
| A pale blocky band where a far cloud's half-res silhouette hides the dust behind it at night, and a thin dark stair-stepped line along distant cloud edges in a still view. | [Weather, rain, lightning, fog](../rendering/clouds/weather.md) | 2026-10-01 (review 20) |
| A dark line where an overcast's far underside meets the sea horizon (not the sea shading). | [The sea](../rendering/sea.md) | 2026-10-01 (review 21) |
| Descending through a deck shows history lag (tstab `fall` 3.8 vs `rise` 1.8) and a band from the sky TAA as the eye's height changes. | [Temporal resolve](../rendering/clouds/temporal.md) | 2026-10-02 (review 22) |
| Fog and dust are not in the light march, the ground shadow or beam occlusion; wind-driven dust storms need a wind field. | [Weather, rain, lightning, fog](../rendering/clouds/weather.md) | 2026-09-29 |
| The cloud volume still marches the rain within the drop particles' reach (optical depth ~0.02-0.05), a slight double count. | [Weather, rain, lightning, fog](../rendering/clouds/weather.md) | 2026-10-04 |
| God rays are experimental and off by default; the effect is subtle and unjudged. Real shafts would need a dedicated jittered sub-march. | [Weather, rain, lightning, fog](../rendering/clouds/weather.md) | 2026-09-29 |
| A harness `knockout +fog_layer` changed nothing visible in one test, which was not understood. | [Automation harness](../development/harness.md) | 2026-10-01 (review 21) |

### Terrain, cities, sea

| Issue | Affects | Recorded |
|---|---|---|
| The ground within ~20 m is soft (it would need real texture maps everywhere); terrain shapes are value noise; the DEM is 2.67 km per texel and 8-bit (the Everest region is a smooth plateau). | [Terrain](../rendering/terrain.md) | 2026-09-25 |
| Erosion stripes crowd into fringes where the steering slope flips (crests, valley floors); visible in the erosion debug view, mostly hidden in the render. | [Terrain](../rendering/terrain.md) | 2026-09-25 |
| SKY_LITE and the environment probes draw the plain DEM without procedural detail. | [Weak-hardware tiers](../rendering/hardware-tiers.md) | 2026-09-25 |
| `beam_self_march.comp` uses the CPU's observer height and the mesh shaders the DEM-only `observerEffHeight`; both are off by at most the detail height. | [Terrain](../rendering/terrain.md) | 2026-09-25 |
| The three concentrating-solar targets (Ivanpah, Noor Ouarzazate, Cerro Dominador) are drawn as PV parks; the `clouds.solar_arrays` setting has no UI. | [Cities, farms and solar parks](../rendering/cities.md) | 2026-10-02 |
| Reflect beam sky shafts still end at sea level (`R_EARTH`), while the ground spots end on the target's own ground. | [Reflectors and beams](../simulation/reflectors.md) | 2026-10-02 |

### Sky, Moon, points

| Issue | Affects | Recorded |
|---|---|---|
| The Sun and Moon are of date while stars, the Milky Way and planets are J2000, about 0.5 deg of precession apart in 2037. | [Sun, Moon and planets](../simulation/sun-moon-planets.md) | 2026-10-03 |
| Environment probes use the main observer's Moon position, so the Moon's face in reflections is approximate. | [Moon and eclipses](../rendering/moon-and-eclipses.md) | 2026-10-03 |
| Bloom and corona exist for satellites and the Sun only, not stars and planets. | [Points, bloom and glare](../rendering/points-bloom-glare.md) | 2026-09-23 |
| Aurora: a storm seen from orbit is a filled ring of concentric bands; the curtain noise's azimuth is Earth-fixed while the oval is Sun-fixed; no dawn-side omega bands or pulsating aurora; the dipole axis is fixed (no IGRF). | [Aurora and airglow](../rendering/aurora-airglow.md) | 2026-10-03 |
| Saturn's ring brightness is omitted from its magnitude; the Moon's near-disc sky halo is not modelled; star day suppression uses an older formula than satellites'. | [Sun, Moon and planets](../simulation/sun-moon-planets.md) | 2026-07-30 |
| UI text is one baked bitmap scaled to every size; large captions at high UI scale look blocky. An SDF font would fix it. | [UI (Clay)](../development/ui.md) | 2026-08 |

## Simulation and accuracy

| Issue | Affects | Recorded |
|---|---|---|
| No inertial-pointing attitude law; Hubble's attitude is an anti-sun stand-in. | [Attitude](../simulation/attitude.md) | 2026-09-23 |
| The GPU cost of part occlusion on stations (thousands of samples on one thread) has not been measured. | [Satellite photometry](../simulation/photometry.md) | 2026-09-23 |
| VRAM on 2 GB parts is untested: shipped textures take about 500 MB before mips and nothing streams or downsamples them. | [Weak-hardware tiers](../rendering/hardware-tiers.md) | 2026-09-22 |
| Rain ambience is not loudness-calibrated against the -24 LUFS convention. | [Ambience](../sound/ambience.md) | 2026-09-28 |

## Dead and vestigial code

Recorded in the October 2026 pass; the first three re-checked 2026-10-03 and still present.

- `gCv2NoFar` is set by the ground shadow in `cloud_march.comp` and read nowhere.
- `kShadowBlurSpread` in `sat_sky.frag` is declared and unused.
- `anchorCol` is uploaded and unused (the tower lattice is a fixed cube map).
- `prevResDepthTex` (march binding 16) is declared in `cloud_v2_march.comp` and read only by the tiles pass.
- `SatFlarePC::meshSatIdx` / `meshSpriteKeep` are pushed as -1 / 1 and unused; `MAG_REF` in `sat_flare.comp` is
  unused.
- The `envelope` mode of `terrainMarchDetailed` has no caller.
- Timestamp slot 2 ("Beam cloud block (retired)") measures two buffer fills.
- Several `GpuCloudParams` fields are unread since the v1 clouds were deleted (marchSteps, lightSteps, hgG,
  shadowMaxDistM, maxRenderDistM and others); some have been reused under new names.

## Documentation drift

Places where a documentation source disagrees with the code. Wiki pages follow the code; these list what the
sources still get wrong, so they can be corrected. Later passes append here.

### `CLAUDE.md` (recorded 2026-10-03)

| Section | Says | Code |
|---|---|---|
| GPU Performance Profiling | SatelliteSim writes slots 1-3 in `recordCompute`, 4 in `recordDraw` | slots 1-5 in `recordCompute`, 6 in the prepass or `recordDraw` |
| Unified scene depth | satellite points write their own range | points test at their range; no point pipeline writes depth |
| GpuSatVisible layout | fourth field is a pad | it is `meshPx`, which becomes `glareFlare` after `sat_flare.comp` |
| Occlusion: one shared depth buffer | terrain and ocean only | the half-res pass also takes the mesh distance; the quarter pass does not |
| Cloud Shadows | `cloudGroundShadow`, 12 steps | `cloudGroundShadowV2`, 20-60 + 12 steps on the v2 field |
| Cloud-light tile culling | in `cloud_march.comp`, feeding `beamCloudLighting()` | in `include/beam_cloud_lights.glsl`, called from `cloud_v2_march.comp` |
| The sea is not terrain | gate is `terrainH0 <= 0`; land cannot read 0 | gate is the water mark; land at 0 m stays terrain |
| Terrain current state | Blinn-Phong ocean glint | Beckmann glint; Phong survives only on wet paddies |
| Weak-hardware: shadow blur | 3x3 at spread 1.7 | [1 2 1] tent at one texel |
| Clouds v2 "Passes" | resolve weight 0.1 rising to 0.35 | 0.05 still (capped 0.15, scaled by altitude), 0.4 moving |
| Clouds v2 pass 16 | cirrus gives way over storms | only with the map-fed anvil shield on (default off) |
| Clouds v2 lightning | eye Sun transmittance by a 24-step march | 32 quadratic steps to 400 km |
| Clouds v2, weather cube | mip 0 has 5 km texels | about 12 km at a face centre, 5-6 km at corners |
| Clouds v2 sub-texel jitter | off in fast flight | off whenever the eye moves more than 1 m a frame |
| Clouds v2 beam shafts | integrated inside the march | a closed-form per-ray pass after the march loop |
| Terrain v2 P1 | night sky light 0.25 of the full Moon | default 0.2 |

### Shader comments (recorded 2026-10-03)

Stale header and inline comments listed in `docs/rendering/FINDINGS.md`: `cloud_march.comp`'s header describes
the v1 march; `cloud_v2_march.comp` says IGN jitter (it is blue noise) and describes `TerrainFrame.w` as the rain
rate; `cloud_v2_far.comp` says "only the low layer"; `terrain_detail.glsl` gives three erosion octaves (two),
an envelope depth pass and bisection (regula falsi); `terrain.glsl` says lake heights are "forced to 0";
`star_point.vert` and parts of `sat_sky.frag` call `moonDirENU.w` the illuminated fraction; `clouds_v2.glsl`
quotes lobe sizes for a 7 km shape period (now 1900 m).

### Found while writing the wiki (recorded 2026-10-03)

Every page follows the code. These are the sources it disagreed with.

| Source | Says | Code |
|---|---|---|
| `CLAUDE.md` photometry table | brightness 1.0, day suppression 500, mirror boost 300, moon suppression 4, pollution gain 1 | 0.93, 570, 1000, 6.6, 25 (vis threshold 0.0001, highlight 0.014) |
| `CLAUDE.md` glare | near gain 3, near range 400 km | 8.0, 21.86 km |
| `CLAUDE.md` clouds v2 | `lightning_rate` 3; fog amount/depth/density 0.6 / 250 / 0.012; `kBeamDiskCut` 1.1 | 0.53; 0.26 / 200 / 0.0013; 1.15. "Base roughness" defaults to 0 |
| `CLAUDE.md` city lights | early LOD and sprite defaults | sprite gain 4, ground 1, start 18 m/px, twinkle 4; LOD 27 / 380 / 1300 / 2000 m/px; street share 0 / 0.13; roads 2 |
| `CLAUDE.md` orbits | Walker: random phase per plane | random `u0` per satellite, no Walker-delta phasing |
| `CLAUDE.md`, schema, modding doc | KnifeEdge roll clamped to ±80°; the table omits `SunTrackingTilted` | ±10° (`legacyAttitudeGroup()`) |
| `CLAUDE.md` controls | KB enum has 8 entries; Move Fine is held; WASD boost ×10 / fine ×0.1 | 20 entries; Move Fine toggles; WASD ×6.25 (cap 0.5 rad/s) / ×1/16 (×10 / ×0.1 is Q/E); WASD is not rebindable |
| `CLAUDE.md` push constants | every struct is exactly 128 B | six are 128; `GlarePC`, `FlareSourcePC` and others are smaller (the rule is at most 128) |
| `CLAUDE.md` hardware floor | sky uses 2 storage images; ~5.2 KB shared memory | 4 (bindings 22, 23, 25, 29: the floor, unchecked by `logDeviceLimits()`); the check requires 6 KB |
| `CLAUDE.md` profiling | App writes timestamp slots 5 and 6 | 7 and 8 |
| `CLAUDE.md` slider arrays | resize four arrays per new slot | all four are sized by `kCloudSliderSlots` |
| `CLAUDE.md` sound | rain driver from `terrainFrame.w` | the rain map's centre cells in the flash buffer |
| `CLAUDE.md` Moon | the phase law feeds satellite dimming | `sat_flare.comp` uses its own linear lit fraction |
| `CLAUDE.md` "one eye" | CPU and shaders share one eye | the CPU ground is a ~18.5 km/px DEM copy; the GPU uses the full DEM plus detail (hundreds of metres apart in mountains) |
| `CLAUDE.md` SKY_LITE list | includes the aurora surface glow | that term no longer exists; zodiacal light, terrain sun shadows and the Milky Way's sea reflection are also cut |
| `CLAUDE.md` sky | ozone in the sky march | no ozone in the sky march (only cloud sun paths and the eclipse sky light) |
| `CLAUDE.md` / presets | sky TAA at render scale 1 (reads as the default) | Low and Medium use 0.70 / 0.85, so they never get TAA |
| `CLAUDE.md` terrain | "Terrain fade start (m)" | persisted and uploaded, read by no shader; "fade end" skips the march outright |
| `KNOWN_RESIDUALS.md` | Sun hour angle ~5.25 h off (no GMST at J2000); Reflect mirror 2376 m²; V2 Mini export "unmitigated" (5.81) | GMST included; 55 × 55 m = 3025 m²; the model is the mitigated one |
| `KNOWN_RESIDUALS.md`, model files | Guowang 5.82, Amazon Leo sd 0.94; V1.5 backsheet "Held out: nothing" | 5.80, 0.918; `mallama2025_starlink_v15` exists |
| `CLAUDE.md` provenance | reference models need zero unexplained parts | not enforced by code (SatModelTool prints and passes); `hubble.json` has no sources block |
| `CLAUDE.md` VisorSat | phase-matched mean 7.220 | 7.260 |
| `docs/CONSTELLATION_MODDING.md` | user data in `%APPDATA%\SatLightSim` first; mesh cap 4096; resolved types written field by field | exe folder when writable, OS folder as fallback; 256; model types written as name/model/base_color |
| schema | 1-2 attitude groups per type | the loader accepts up to 4 |
| `README.md` | Visual Studio 2022 required | CI builds with Ninja + any MSVC |
| `docs/HARNESS.md` | dated host-freeze paragraph, review numbers | history; the wiki's harness page leaves them out |

### Code comments and unused fields (recorded 2026-10-03)

- Ocean comments say: a Blinn-Phong exp-300 glint, foam = sun + 0.9 × sky reflection, waves read `pad1`/`pad2`, `kSeaChoppy` "4→2", an 8-step secant, shoaling ~200 m, 0.76 m per float step. The code has Beckmann + Smith, a diffuser, `oceanState.xy`, 3, 5 coarse + 6 secant steps, 150 m, 0.5 m. "Ocean MW refl" is on the Atmosphere tab.
- `cloud_params.glsl` calls `cityLod2.zw` unused (`.z` is the sea wave range fade). The `city_sprites.comp` push-constant comment says 48 B (it is 64), and its coloured-sign branch is unreachable. `GpuSolarSite` `params.x/.z` are written and never read. `SatelliteSim.h` names the key `terrain.solar_arrays` (it is `clouds.solar_arrays`). Three twinkle-rate comments disagree.
- `auroraGlowAt()` is never called; `aurora_ground_gain` and `auroraCloudGain` are uploaded and never read; `cloud_march.comp` mentions `auroraOvalMaskLocal` / `auroraSheetsAdd`, which don't exist.
- Knockout bit 64 (`cloud_self_shadow_cone`) is read by no shader.
- The `GpuSatLobe` comment says 48 B (it is 64). The RandomShell tumble comment says 0-1 Hz (the code scales by 0.001: up to ~0.0063 rad/s). The Potato enum comment says render scale 0.5 (it is 1.0). `MusicAnalysis.h` says the cache key is size + write time (it is size + a content hash).
- `star_catalog.h` comment names are wrong for several stars ("Bellatrix" on Canopus, "Capella" on Betelgeuse), from `tools/parse_bsc.py`'s `STAR_NAMES` table. Only the comments are affected.
- Unread JSON fields: `constellations.json` `"version"` and `reflector_targets.json` `"tech"`; `observer_spawn` is only logged.
