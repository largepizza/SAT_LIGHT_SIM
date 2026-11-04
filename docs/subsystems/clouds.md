# Clouds v2 (the volumetric clouds)

The cloud field, shapes, layers, rain particles, march, resolve, lighting, perf passes, reviews. Storms / weather objects are in weather.md.

Part of the subsystem notes (`docs/subsystems/`, map in `AGENTS.md` §4). Moved verbatim from
`CLAUDE.md` on 2026-11-03. Do not read this file whole: `python tools/outline.py docs/subsystems/clouds.md`
lists its sections and topics with line numbers. New notes go next to the section they belong to.

## Subsystem: Clouds v2 — the volumetric clouds (2026-09-27; v1 deleted the same day)

Design log and status: `.plans/CLOUDS_V2_PLAN.md`. The only volumetric cloud renderer: v1's
`cloudMarchCS`, its ground shadow, its copy in `beam_self_march.comp` and its sliders are gone (there
is no toggle). Every tunable is a Clouds-tab slider AND a settings.json key under `clouds_v2`, so the
harness can sweep it (`set clouds_v2.<key>`). Cirrus (`cirrusMarchCS`) and the flat 2D layers are
still the older model (v1's ground fog was retired 2026-09-29 for the v2 fog + dust, below) and keep their own sliders ("Cirrus", "Flat layers" sections) — the old cirrus
only as the stand-in when the volumetric march is knocked out (see HIGH LAYER below).
- **One field** (`shaders/include/clouds_v2.glsl`, `cv2Field`) serves the view march, its light march,
  the ground shadow (`cloudGroundShadowV2`, cloud_march.comp) and beam occlusion (beam_self_march.comp).
  Weather cube (baked from the 8K map: coverage, classified type, precipitation, tropopause) x
  mesoscale fields on the sea-level sphere x 3D shape noise x detail erosion; 5 cloud types
  (`cv2Types`, SatelliteSim.h). Metric coordinates anchored at the observer's sea-level point (CPU
  double), like terrain detail — never an absolute ECEF position in float.
- **The map drifts** (~16 deg of longitude per 12 h), and every consumer reads ONE phase:
  `SatelliteSim::cloudDriftPhase()` = rate x (t - 2036-06-21) + `cloudDriftPhaseOffset` (session state;
  the default keeps the default rate's map exactly where rate x (t - J2000) put it). It used to be rate x
  the time since J2000 (~7500 rad), so any change of the rate threw the map to an unrelated longitude —
  and the intro FORCED its own rate (6.55e-6), which settings.json then saved: every intro silently moved
  the player's clouds. The intro now sets only the session offset. **The whole field is read in the
  drifted frame** (`cv2Drift`: the weather cube AND the noise volumes, anchors rotated on the CPU), so the
  clouds move with the map as one body, and the resolve reprojects that motion exactly (`extra.zw` = the
  drift since last frame, `obsDelta` = Rz(d) eye - prevEye + the shape wind's shift); `motion.x` is the
  volumes' relative slide it cannot follow, which raises the new-sample weight. A `time sun` jump moves
  the clouds — pick benchmark spots at a fixed time: `python tools/harness/find_cloud_spots.py <ISO time>`
  (assumes the default rate; a harness run from a user settings file should `set clouds.drift_rate
  0.0000065`).
- **Coverage** is the map's brightness remapped (`cover.xy`: clear below 0.12, overcast above 0.6);
  the raw value used as a fraction never let anywhere close over.
- **The shape is a 3D margin, not a heightfield** (2026-09-27): cloud exists where
  `m = e(x,y) + lobes(x,y,z) - g(z) - gBase(h) > 0`. `e` = how far inside the thresholded 2D field;
  `lobes` = inverted-Worley 1.75 km / 875 m / 440 m octaves in the SAME units as `e`, so they move the
  side surface sideways (cauliflower lobes, overhangs), faded with the footprint and damped near the
  base; `g(z)` = the strength needed to reach height z (convective z^1.2: small cells low mounds, cores
  towers; decks flat with a lumpy top, `alt.z` = flatness); `gBase` curls the base up toward the edge.
  **Why:** v1 and the first v2 cuts extruded a 2D presence under a height, and a heightfield cannot
  bulge: every cloud, down to the smallest puff, was a flat-bottomed can with vertical sides (the
  "cola can"). Noise on the TOP height moves a steep wall almost not at all — it has to be in `e`'s
  units. Deep types also read the cells two mips coarser (wider towers). Erosion is reduced deep inside
  (`form.z`) — full-strength erosion holed the interior and the sky showed from inside a cloud.
- **Decks** (2026-09-28): a deck's thickness follows the closed cells and a km-scale Perlin field
  (`deckVar`, `zTop`), a thicker part hangs lower, and every base is bumped in 3D by the lobes (`hbE`,
  `baseAmp` = 40 m cumulus .. 220 m stratus x `storm.w` "Base roughness"): from below an overcast is seen
  by the light it transmits, so its thickness is its texture. `CV2Field.deck/topH` let the march shadow
  a deck lit at a grazing angle by the path to its top (tens of km, far past the light march) — without
  it the terminator band glowed red from orbit.
- **Deep convection**: lobes and erosion storm.x ("Storm feature size", 2x) larger via second anchors
  (`anchorStorm`, `anchorStormDetail` — never scale an anchored coordinate: frac(a)/k pops when the
  observer crosses a period), erosion kept storm.y; **anvils** (`cv2AnvilSigma`, storm.z "Anvils"): an
  ice shield under the tropopause where the weather cube read ~30 km coarse and 25 km upwind says deep
  convection, 3 km thick at the core, flat domed lid, frayed outline; the Cb towers overshoot it by
  250 m. Straighter-walled deep towers (0.9 z^1.8) were tried and extruded vertical flutes: reverted.
- **Cells** (`cloud_v2_noise.comp` mode 2): three sizes of blobs (8/16/32 per period) in warped space,
  merged by a SOFT UNION and clumped by Perlin — max() of one or two sizes thresholded into Voronoi
  polygons from altitude.
- **Genera so far:** the low system's 5-type table (stratus .. cumulonimbus); nimbostratus = a
  stratiform deck thickened by up to 3.5 km where the map rains; a MID LAYER (`cv2MidSigma`, 2.8-7 km,
  `misc.w`, "Mid layer (Ac/As)"): altocumulus cloudlets in a thin lens where the low cloud is broken,
  translucent altostratus where it is stratiform, only where the map has cloud and a mid-level regime
  field (cluster Perlin) allows. **HIGH LAYER** (`cv2HighSigma`, 2026-09-28, `high`/`high2`, sliders
  "High layer (Ci/Cs/Cc)", density, "Cirrus stretch", "Cirrus wind"): at ~0.74 of the tropopause,
  cirrus streaks (the shape volume read with the along-wind coordinate = drifted longitude x R at a
  period that divides the equator an integer number of times — no antimeridian seam — stretched,
  meandered, sheared with height for fall streaks, combed by a 9x read), cirrostratus veils where the
  map is stratiform, cirrocumulus patches. Its samples are lit cheaply (`CV2Field.thin`: no light
  march, no ambient probe — it spans the whole sky). It replaces `cirrusMarchCS` and flat layer 1:
  `layers[1].enabled` is 0 while it is on and the march isn't knocked out. **The baked Perlin fBm is
  0.50 +- 0.057** (measured by replicating the bake in numpy): thresholds on it must be set against
  that spread — the first cut of the high layer used 0.5-0.7 and drew nothing.
  `cv2Field` = low + mid + anvil + high, so every consumer (march, light march, ground shadow, beams)
  sees them all.
- **Terrain-relative bases** (2026-09-28): the weather cube's ALPHA is the DEM height / 8 km, smoothed per mip
  (`cloud_v2_weather.comp` binding 3), read at the EARTH-FIXED direction (`cv2Ground`, the map drifts, the
  terrain does not); the tropopause it used to hold is a function of latitude (`cv2Tropo`). Low bases rise
  by 0.6 (decks) .. 0.9 (convective) x the ground, tops with them but capped at the Cb top; the mid layer by
  0.8x. Until then every base was above SEA level and the Andes/Tibet stood inside the cloud.
- **Erosion detail fades with distance toward its MEAN, at full strength** (`df = mix(0.45, fetched,
  detailAmt)`). It used to fade the erosion toward NONE and then switch to the mean where detailAmt hit 0: a
  band of uneroded cloud just inside 4 x `detail_lod_start_m`, seen from orbit as a ring (~220 km out).
- **The source map is a JPEG**: its 8x8 blocks (~40 km) sit just above/below the clear threshold. Anything
  switched on by `cov > 0` alone draws them as straight-edged rectangles (the mid layer did; it now fades in
  with `smoothstep(0, 0.35, cov)`). Scale new layers by coverage, never gate them on it.
- **High layer from orbit**: its regime follows the map's coverage over ~150 km (weather mip 4) — cirrus
  lives with the weather systems — and its streaks come in bundles (the volume read 4x coarser; the CPU keeps
  the along-wind period count a multiple of 4). On noise alone it covered the globe in even splotches.
- **Debug view 7**: per-layer optical depth (red low incl. rain, green mid, blue anvil, white high) — the
  first thing to look at when a cloud artifact has an unknown owner.
- **Motion** (`cloud_v2_resolve.comp`): the new-sample weight and the clamp box follow PARALLAX (the
  reprojection at the cloud's depth vs at infinity) plus the clouds' own evolution — not total pixel motion.
  A rotation reprojects exactly; raising the weight during a pan swapped the accumulated image for the
  quarter grid's noisy samples (a knitted cross-hatch). In parallax all four pixels of a 2x2 block take
  nearly the same update. Check motion with `path play record=` (a settled `capture` hides it).
- **Far field (orbit)**: a fractal presence from the cluster Perlin at 1x and 4x (3-90 km, `fz`), thresholded
  to the map's coverage (area ~ cov), replaces the sub-pixel cells' uniform haze as the cells stop resolving
  (`farK`), and clusters the near field a little (0.08) so orbit and ground agree on where the holes are.
  Before it, clouds from space were the map's warped 5-10 km blobs.
- **Beams as LIGHT** (2026-09-28, `.plans/BEAMS_V2_PLAN.md`, `cv2.beam`: "Beam shafts" x, 0 = the old drawn
  line; "Beam haze / dust" y): `beamLightCloud` lights cloud samples like the Moon does (3-step march toward
  the satellite, the cloud phase lobes, ice/rain optics); `beamShafts` integrates each culled beam's Gaussian
  column across the view ray in closed form (air + aerosol scattering at the closest point, phase at the
  angle to the beam, occluded by the ray's clouds through transmittance checkpoints `tq`) inside the march,
  so the resolve anti-aliases it. Verified in pass 10 (harness_runs/cloud_v2_p10b, _p10c: Anchorage, 133-214 beams, clear and cloudy). **Pass 10 (2026-09-28):** a beam is a DISK (`beamDisk`: the Sun's
  limb-darkened disk seen in the mirror, radius = `footprintRadM`, edge blurred over ~the mirror's width),
  not a Gaussian of that sigma cut at 4 sigma — that lit ~16x the area, drew a glow ~4x the spot ending in
  a hard circle ("blobby, sharp-edged beams"), and crowded ~4x the beams into each tile (`kBeamDiskCut`
  1.1 in the cull). The ground spot (sat_sky.frag, same total energy) follows. Light is PHYSICAL: mean
  irradiance = reflecting area / disk area in Suns (`beam.w` = 1/(1361 beamGain) undoes sat_orbit.comp's
  intensity), x `beam.z` "Beam light on cloud" (30: the night view's moon is ~2400x physical, so 1 reads
  dim beside it); v1's `beamSkyGlowGain` no longer applies. `beamLightCloud` sums the lights first and
  marches occlusion ONCE along their mean direction: three field samples per beam per cloud sample cost
  350 ms frames over Anchorage (the user's profile; 3-4 ms after). Harness framing: `beams` / `beams go`
  (the site where beams converge NOW — a guessed time and site framed none three times), coverage forced to
  0 / up for clear / cloudy nights (harness_runs/cloud_v2_p10b.satcmd).
- **Rain / sleet / snow / diamond dust at the eye are PARTICLES** (2026-10-04, `rain_particles.vert/.frag`,
  `SatelliteSimRain.cpp`): one instanced draw in the main pass after everything else, i.e. AFTER the sky TAA,
  at full resolution, premultiplied over the tonemapped frame (ONE / ONE_MINUS_SRC_ALPHA: a drop dimmer than the
  sky behind it darkens it). Everything is procedural from `gl_InstanceIndex`: nested world-fixed boxes in the
  rain frame (`cv2.rainE/N/U`, the nearest 0.25-degree point, eye position from double wrapped to 1024 m), box k
  = 4 m x 2^k holding N0 x 2^k drops ("Drop particles (x1000, nearest box)" `rain_particles_k` 16, slot 261),
  drawn over [L/4, L/2] and cross-faded with the next over [0.4, 0.5] L, out to "Drop reach (m)" (`drop_reach_m`
  32, slot 208, at most 64 = 6 levels; the old `drop_distance_m` was the lattice's and is not read). **Rain drops
  (2026-10-23 rework, the user: "extremely floaty and blowing straight sideways", even at fall x10)**: a particle
  carries a fixed share of the rain's cross-section, so its size is drawn from the AREA-weighted Marshall-Palmer
  distribution Gamma(3, Lambda) with Lambda from the rate round the eye (`RainDrawPC::dropLambda`, the old unused
  `aspect`: 2.5 / mm at 10 mm/h, 1.7 at 60) and falls at its terminal velocity (Atlas, ~4-7 m/s). It was the NUMBER
  distribution above 0.5 mm at a fixed 5 mm/h: median 0.74 mm, 2.9 m/s — the drawn rain fell at drizzle speed and
  any wind of a few m/s blew it sideways. And each particle is REDRAWN elsewhere every "Drop persistence (ms)"
  (`drop_persistence_ms` 60, slot 334, 0 = never; `cv2.snowMotion.zw` = sim time / it and on, each particle at its
  own phase, faded over the epoch's ends): it stands for ~12 (level 0) to ~3000 (level 4) real drops, and kept as one
  body the far levels (which hold most of the particles) crept across the screen as lumps at 2-3 px a frame — the
  "floaty" look; raising the fall speed only turned them into long faint lines. Now every frame holds unrelated
  streaks, as a camera sees rain. Flakes (2-8 mm at ~1 m/s, fluttering) and diamond dust are kept for good and take
  0.35 of the shutter (a flake is seen, not a dash). **Motion is integrated over sim time on the CPU**
  (`updateRainMotion`; `cv2.rainMotion` / `rainWind`, wrapped to 1024 m): the fall phase (a drop's offset = its
  terminal speed, quantised to 1/64 m/s, x the phase) and the wind drift of rain and of snow, so speed and wind change
  smoothly. Fall = "Drop fall speed (x physical)" (`drop_fall_speed` 1, slot 330; heavier rain falls faster through
  its sizes); ICE has its own phase (`cv2.snowMotion.xy`) at that factor capped at 1.5. **Wind = the surface wind at
  the eye** (`surfaceWindAt`, a pure function of place and sim time; harness `state` -> `clouds_v2.rain.surface_wind`):
  a mean wind from a slow noise over place and hours at eye height (median ~1.4 m/s, up to ~6, any direction; "Surface
  wind (x)" `surface_wind_gain` 1, slot 331); the storm towers' OUTFLOW (wx::Catalog's towers, read only, within 60 km,
  away from each, strongest a few km out; "Storm outflow (m/s)" `storm_outflow_mps` 8, slot 332); the shower's outflow
  from the rain map's rain-weighted centre (0.25 x that x the rate, 0.3 of it for snow); **GUST FRONTS** ("Gust
  fronts" `gust_fronts` 1, slot 333): events hashed per 15-s bin of sim time and 2-degree cell, a 1-2 s rise to +50-100%
  of the wind + up to ~5 m/s more by storms, at most +10 m/s, turned 15-40 deg, decaying over 5-20 s; a bin's chance
  ~0.1 in calm air to ~0.45 in a downpour by a storm, faded in with it (no pop when conditions change); small
  turbulence ("Wind turbulence" `wind_turbulence` 1, slot 266); eased over 0.25 s. Snow drifts at "Snow drift (x surface
  wind)" (`snow_drift` 0.7, slot 209). It was 0.4 x the cloud wind aloft toward the east + a 9.5-14 m/s outflow from
  ANY shower (snow too, x 1.2 for flakes): ~5 m/s on flakes falling ~1.5 m/s. The old keys (`rain_fall_speed`,
  `rain_wind_gain`, `rain_storm_wind_mps`, `rain_gusts`, `snow_wind`) are not read: they were tuned against the lumped
  drift. Measured (Venezuela bookmark, shower 0.3): wind 2.5-4.5 m/s, the visible drops slant ~25-35 deg; 8 km from a
  Cb tower in its core (rate 1): 7-12 m/s in gusts, ~50-60 deg. The `gust` envelope drives the ambience (below). Which drops exist follows the rain map at each drop (`cv2RainMapAt` x
  `cv2RainBurst`, now in cloud_lightning.glsl): a shaft's drops appear in the far levels first. **Coverage is
  conserved:** a drop is the streak it sweeps over "Drop shutter (ms)" (`rain_shutter_ms` 33, slot 262; + the
  eye's velocity, capped at 30 m/s), each pixel's alpha the share of the exposure it spent there, and each
  particle carries (the real rain's geometric cross-section per m^3, Marshall-Palmer at R = 60 rate^1.5 mm/h;
  snow x4) / (particles per m^3): summed along a ray the particles' coverage is the rain's optical depth. Where
  one particle would carry so much that it draws as a bright dot (far, heavy rain: alpha 0.12-0.35) it fades and
  the volume carries that distance. "Drops at the eye (visibility)" (`rain_streaks`, slot 157) multiplies the
  alpha: 1 = physical. **Light = the march's rain sample at the eye**: cloud_v2_lightning.comp thread 1 writes
  `cv2RainKey` (Sun x its cloud transmittance x the Earth's shadow, or the Moon), `cv2RainKeyDir` (w the liquid
  share), `cv2RainAmb` (zenith sky through the column above as the march's ambVis, + the ground bounce + city
  light at night; its HUE kept only by the column's diffuse transmission and at most 60% of the zenith's, as the
  march's `ambHue`, 2026-10-23: under a Cb the drops were sky-blue, ambient B/R 1.58 at tau 275 -> 0.97, same
  luminance), already / the white balance (`kCv2RainLightOffset`); copies of the march's `sunColorAt` /
  `skyZenithAt` there (keep in step). The phase is the volume's own (`include/cv2_optics.glsl`, moved out of
  clouds_v2.glsl unchanged — the march's SPIR-V is identical): drops 42 degrees from the antisolar point flash
  the bow's colours, drops toward the Sun glow in its diffraction lobe; flakes add a fluttering plate glint;
  diamond dust (ice fog at the eye in sunshine, no rain) draws only tumbling plates' glints. The host draws
  only when the lightning pass ran this frame and the previous frame's map held rain (or diamond dust).
  Harness `state` -> `clouds_v2.rain` (mode, instances, rate at the eye / max, `max_at_en_m`, key, ambient);
  benchmark: the user's Venezuela snapshots (`profile_log` 40-41, harness_runs/rain_rebuild/base.satcmd; walk
  into a shaft with time PAUSED: the clouds drift ~40 m/s and outran a timed walk). Cost ~0.35 ms at the
  defaults (496k instances, mostly culled in the vertex shader), +0.45 at 64 m; the cloud march lost ~1 ms
  without the old drops. Not modelled: the volume still marches the rain within the particles' reach (tau
  ~0.02-0.05, a slight double count). Lightning lights the drops (thread 1 adds each flash to `cv2RainAmb`), and so
  do the **Reflect beams the eye stands in** (2026-10-06): the CPU evaluates beamGather at the eye from the
  cloud-light list (`cv2BeamEyeIrr` / `cv2BeamEyeDir`, SatelliteSim.cpp after the emit; disk profile, Suns, the
  beam's block altitude/opacity) into `cv2.beamEye` (x irradiance x "Beam light on cloud", y 1 - dayness) /
  `beamEyeDir`, and rain_particles.vert lights every particle by it through the same phase (`dropPhase`: drops show
  the bow opposite the satellites, flakes and ice crystals glint and halo around them). Harness `state` ->
  `clouds_v2.rain.eye_temp_c / shaft_temp_c / beam_eye_irr_suns / beam_eye_dir_enu`. **Precipitation type**:
  `cv2.precip.x` = the air temperature at the eye (`SatelliteSimCloudsV2.cpp`: -18 + 45 cos(lat)^1.5 at sea
  level, + the season swing, - 6.5 C/km); snow below ~-1 C, rain above ~2.5 C, sleet between. Only the eye's
  precipitation changes type. The `rain` ambience driver takes the liquid share (`cv2EyeTempC`) and the rain
  map's centre. **Replaced** (review 3-22): `cloud_march.comp`'s `rainDrops`, a per-pixel search of a 3D
  lattice's nearest cells in up to 9 depth layers inside the HALF-RES composite: a few hundred fat streaks, smeared
  by the sky TAA, lit by the cloud colour along the ray, a far drop "a clump" ((rad/w)^0.25) — and before it
  streaks in direction space that travelled with the eye. Debug view 6 now shows the rain at the eye (r) and in
  the map (g). **Snow shafts
  show no rainbow** (review 6: the shafts' air is 500 m above the GROUND under the eye, and the season
  swing is 0.25 x |lat|, at most 15 C — 0.35 x |lat| made the Antarctic plateau rain in November) (cloud_v2_march.comp, per ray): below freezing ~500 m up (the eye's temperature,
  6.5 C/km), the rain phase's bows give way to faint ice optics (sundogs, a weak 22 degree halo, a pillar
  under a low Sun) — a snow shower's big aggregates scatter broadly; only its few plates make optics.
  **The beams' light follows the same rule** (2026-10-06, `beamFinish`; `cv2.precip.w` = the shafts' liquid share,
  CPU, from the same temperature): until then every beam-lit shaft took the rain phase, and the user's McMurdo
  view (mid-April twilight, -23 C at 384 m and in the shafts' air) showed a bright "satbow" opposite the Reflect
  satellites in a snow shower. Snow gives the snow phase (no bow; a faint halo and sundogs around the satellites).
  `cv2PillarOptics` gives nothing for a source above ~25 deg, so beams make no pillar; a real light PILLAR from a
  beam's ground spot (plates reflecting the lit snow below) would need the spots as light sources in the fog/snow
  march — not built.
- **Per-layer scales (pass 10):** the shape period ("Shape period (m) - low cloud") no longer sets the
  cirrus fibres (`cv2CirrusPeriodM`, "Cirrus period") or the altocumulus (`anchorMid`, "Mid layer
  period"); the mid layer has its own density (`atmo.z`) and the anvils only "Anvils" (neither reads
  "Density" any more). "Storm feature size" is still a multiple of the low shape period; the rain
  curtains still read it.
- **Anvils are fed by their towers (pass 10):** the weather at ~10 km (mip 1) at the point and 12/25/40 km
  upwind, Cb-typed and covered; it read one 30 km-blurred texel 25 km upwind, so only storm regions hundreds
  of km across had one and it did not sit on its towers.
- **Cloud sunlight vs the sky (pass 10):** the key light's Rayleigh is its own (`atmo.x` "Cloud sunlight
  Rayleigh", x physical), not the sky's `atmosRayleighGain`; "Twilight sky light" (`atmo.y`) boosts the sky
  ambient from ~7 deg below a sample's horizon to ~14 deg above it (the zenith alone under-counts the dome;
  past the terminator its ozone blue turns the last red light purple). Ice is DELTA-SCALED: the forward
  peak (0.45) is not extinction (`sigma x (1 - 0.45 thin)`, phase = the broad lobe, optics / 0.55) —
  full extinction dimmed the clouds under cirrus as if the peak were lost (dark bands from orbit) — and
  the high layer is lit from below by the cloud under it (albedo from the map's coverage).
- **Sky light through cloud (pass 10b):** the ambient at a sample is the DIFFUSE transmission of the cloud
  above it, 1 / (1 + 0.11 tau) (two-stream, g ~0.85; tau from the probe 250 m up or the column to the top),
  plus light from below (the ground and horizon sky, 0.35 x (1 - hf)). It was exp(-sigma x 250 m): ~0 inside
  cumulus, so every large cloud's lower part was one flat dark grey under a hard line (user report, near sunset).
- **Beam shafts are a ray-cylinder intersection (pass 10b):** the stretch of the view ray inside each beam's
  column (clipped to the eye, the scene depth, the ground point and 60 km), with the air integrated along it;
  the column's edge widens to 1.5 pixel footprints (energy kept). The closest-point chord it replaced turned
  with the camera when standing inside a beam (billboarding). The light cull reaches 60 km, not the cloud top.
  Debug view 8: r the tile's lights, g the beams a ray's interval found, b the shaft radiance (log).
- **Pass 11 (2026-09-28):** the user's cloud settings are the compiled defaults (clouds_v2 only; the presets' 
  max_iters/light_steps/step_growth and the perf-heavy full_rate_above_km 0 were left out). **Flow** (anchorFlow, 
  flow.x, sliders "Flow warp" / "Flow scale"): the mesoscale reads, and half as much the weather lookup, are 
  displaced by a smooth 2-component field (the Perlin channel at 6000 km, coarse mip) so systems bend into curves; 
  the shear must stay < 1 (0.07 of a 2400 km period folded the low field into combed brush strokes). **Layer 
  spread** (flow.y): the mid layer keys on the weather over ~80 km (mip 4), not only the low cloud under it, and 
  cirrus less on the low systems. **Storm veins**: the erosion detail volume uses a smooth-min F1 (no creases) and 
  deep convection bites 70% fewer cracks. **Beam lines** (atmo.w, 0.25) draw each beam's own streak beside the 
  shafts, Rayleigh-blue and 1-20 km; shafts write their weighted distance as depth so the resolve reprojects them 
  at the right parallax while walking.
- **Pass 12 (2026-09-28):** **Convective profile** (`cv2FieldLow`, "Top-heavy" `flow.z`, "Tower top" `flow.w`,
  0 = the pass-11 cones for A/B): each CELL is one cloud — its top T from the cell-scale strength (the cells
  2 mips coarser, 4 for deep types) at most "Tower top" of the span, and a required strength parabolic about
  its widest point high up (a narrower stem, a round dome; no flat stretch, which extruded the 2D field into
  vertical walls), capped by the cells' own dome (smooth min) so dense fields keep turrets instead of mesas;
  convective lobes 2.5x, faded in from the cell edge. It was `e - z^1.2` with each column's own top: a cone
  per cell, and strong cores cut flat at the type's top. Tried and dropped: normalising e by the cell's peak
  (dense fields became slabs). Weak cells now have a minimum height (0.25 of the span): fair-weather fields
  draw as fields of puffs where the old shape drew a few dots. **Flow**: the early out read the map at the
  UNflowed point at mip 4 while the lookup moved up to ~200 km, cutting clouds along 80 km texel edges; it now
  reads the flowed point (`cv2FlowDisp`, fixed mip), and the mid/high/anvil layers read the same flowed map
  (`cv2FlowWeatherDirAt`). **Beam cloud light**: only beams landing within ~1 footprint of their site (the
  line's perpendicular miss) feed its light, fading out as they leave; slewing beams get none (a departing
  mirror dragged the site's light out, then it snapped back).
- **Pass 13 (2026-09-28):** **The flow is ANALYTIC** (`cv2FlowDisp`: the curl of eight smooth waves on the
  drifted sphere, float, divergence-free; `anchorFlow` = x its slow evolution phase, w 1/period). It was the
  RGBA8 mesoscale volume's Perlin channel at a coarse mip, and hardware filtering has 8-bit sub-texel weights:
  at ~375 km texels the displacement stepped every ~1.5 km, shifting the clouds by hundreds of metres per step
  — blocks with straight seams on the texture grid, anchored at the observer, worse with the warp (and the
  v1 distortion had the same). Never read a displacement (or anything multiplied up by a large factor) from a
  hardware-filtered 8-bit texture; the DEM hit the same limit. **Cirrus** (section "Cirrus (high layer)"; the
  old "Cirrus" section is v1, drawn only with the march knocked out, now labelled legacy): its regime noise is
  read at its own field size (`high2.y`, "Cirrus field size", 1200 km, two octaves) on a frame displaced by the
  low flow x `high2.z` ("Cirrus flow", 1.5); it was the cluster-scale noise (splotches) and ignored the flow.
  **Cb columns**: strong deep cores (`deepCol`) run the full span up to the anvil (capped at "Tower top" the
  anvil floated free) with a concave profile — wide base, a waist (0.55 + 0.2 x Top-heavy), a flare under the
  lid, flat top. Every deep cell at full height cost 56 ms inside a storm; cores only, ~the old cost.
  **Snapshots carry the view**: `view` (obs_dir, height, camera, cloud drift phase/rate) and the full
  `settings`; harness `snapshot <profile_log.jsonl> [index=]` restores one exactly — the user's way to hand
  over a location (lat/lon + time alone framed other clouds: the drift phase is session state).
- **Pass 14 (2026-09-28, the user's snapshots):** **The weather-lookup warp folded the map**: its third
  component was the closed-cell RIM field (`ce.b`, a near step) x 7 km, so the lookup jumped ~4 km across
  every Voronoi rim and storms extruded the folds into planar walls and fins; now smooth Perlin only. The
  weather cube (`cv2WeatherSmooth`) and the coarse cell reads (`cv2MesoSmoothG`) are read C1-smooth (the
  fractional texel coordinate smoothstep-remapped before one fetch): linear filtering of 5 km texels / a
  4-16 texel mip is flat facets with straight creases. Deep cells use the coarse cells only (the fine
  cells' edges were grooves up a 10 km column); a Cb's head reads a wider, downwind-displaced cell field
  above its waist (`headW`, the mushroom overhang); shallow cells get no stem narrowing (`tallC`) and
  "Base flatness" (`high2.w`, 0.8) damps the lobes and the erosion at convective bases (mammatus look).
  **Flow**: bends only the weather map (full flow) and the cluster field — never the cells (any warp
  stretches every field read through it by its shear); "Flow warp" capped at 0.1. **Ground shadow**
  (`cloudGroundShadowV2`): starts 10 m up (on the sea the start point straddled the 0 m shell floor in
  float rounding: rings and lines of shadow around the nadir), reads the field at the step's footprint,
  jittered; 20 steps through the lowest 3.5 km above the ground, then 12 through the rest (review 3: 24
  even steps over the whole shell were ~600 m apart under a high Sun, so each pixel's jitter hit or
  missed a 300-m deck — the shadow of a thin overcast was a grain of full shadow and none; +~0.8 ms). **Sun-colour cache** in the march refreshes on 800 m of altitude too (from inside the anvil
  a ray lit the cumulus 10 km below with the 12 km sunlight: clouds flipped gold to white with 1 km of
  eye altitude). **Night side**: the cloud's sky ambient fades over the Sun's first ~5 deg below the
  sample's horizon (`skyDusk`; skyZenithAt's six midpoints over-counted twilight). **Beam spots**: a
  soft dome (1 - smoothstep(0.25R, 1.15R), x 1.885 = same energy) on the ground and on cloud.
  White balance backs off below ~11 deg of Sun. Open: fine contour ripples on smooth storm domes
  (not the march steps, anvil, mid/high layers or the light march; present before this pass).
- **Pass 15 (2026-09-28, the user's third batch):** **Cumulonimbus is three layers**: the low layer's
  storm regions stop at "Storm cumulus top" (`column.w`, no full-span deep cores while "Cb columns" > 0);
  `cv2ColumnSigma` draws discrete towers on a jittered 3D lattice (`anchorCol`, 64 cells per period so the
  cell hash is stable; a centre is kept within 0.45 cell of the sea-level sphere, and the 2x2x2
  neighbourhood is exact to 0.75 cell), each with its own axis and a concave radius profile: wide base,
  waist, head flare under the lid, blown downwind, with storm-scale lobes. `cv2AnvilSigma` hangs lower
  around a tower's head (`near`). A margin field thresholded per height cannot narrow and flare again, so
  the old Cb was a mountain sloping up into a floating anvil. Every shape constant is a setting (sliders
  164-175: `cb_columns`, `cb_spacing_km`, `cb_radius_km`, `cb_cumulus_top_km`, `cb_waist`, `cb_flare`,
  `cb_head_drift_km`, `cb_lobes`, `cb_sparsity`, `cb_overshoot_km`, `anvil_thick_km`, `anvil_hang_km`),
  so one harness launch can sweep configurations with `set`. Column strength is read once per sample
  from weather mip 3; a read at each candidate centre (up to 8 scattered cube fetches) took storm views
  from 10 to 29 ms. **The flow is evaluated once per field sample** (`gCv2FlowSet`/`gCv2FlowD`, set by
  `cv2Field`): low, mid, anvil, high and columns each paid the eight waves. Columns cost ~1.5 ms in a
  storm view. **Cloud airlight** (`cloud_v2_march.comp`): the path's in-scatter was weighted by
  (1 - attn) as well as (1 - T), counting extinction twice (inherited from v1's camPathInscatter). A
  cloud got a fraction of the haze beside it, and past the terminator from 15-60 km it read as dark
  blue patches in golden haze. It now also takes the sky pass's orbital terminator gate, so the two
  agree above 40 km. "Storm feature size" goes to 12. Open: the dome tips seen from above have
  pits/rings; a thin anvil (< 1.5 km) streaks horizontally at grazing views.
- **Pass 16 (2026-09-29, the user's fourth batch, 12 km towers):** the tower lattice is **2D on an
  equal-angle cube map** of the drifted sphere (2x2 search, 3x3 once a tower's reach passes 0.72 cell;
  only the upper half of a tower can need it). The 3D lattice (pass 15) cut big towers along cell faces
  (grid seams) and rejected most candidates as off the sphere; the 2D one cannot straddle a cube-face
  edge, so towers whose reach crosses one fade out (a tower-free band, never a cut); `anchorCol` is
  unused. **Every tower meets the anvil**; its head rounds off over 1.5 km and stays 300 m INSIDE the
  anvil, and only about a third of towers push an overshooting dome through (`cb_overshoot_km`, 0.8,
  replaced `cb_full_frac`). Heads closing at the lid stood above it as flat plates tens of km wide:
  march-step contour rings, a moat, and from orbit dark "tumours" (a grazing Sun's light-march ray skims
  a plate; decks avoid that through the path-to-top term, towers do not). Tower edges ramp over a fixed
  ~300 m (0.2 x the radius was 2.4 km of fog around a 12 km tower). The cirrus layer (0.69-0.93 of the
  tropopause) gives way over storms (one weather read), since it cut through anvil sides. The march's
  sun-colour cache is kept at fixed 800 m levels and interpolated. The ground height is read once per
  field sample (`gCv2GroundSet`). **Perf is open:** with the user's 12 km towers, 3.6 km anvil hang and
  3.3 km anvil, storm views are ~34-41 ms of cloud march vs ~16-29 ms with towers off. Towers at 0.3
  density cost the same, so it is not the volume, but no per-sample cut tried moved it (3D -> 2D lattice,
  fewer fetches). Also open: the concentric rings looking down from inside the anvil (user snap 1), not
  the sun-colour cache.
- **Pass 17 (2026-09-29, the user's fifth batch, 1920x1009):** the tower lattice's cell is at least
  (full reach)/1.2 (`reachFull`, uniforms only), so the 3x3 search covers whole towers: 12 km towers
  flared 2.6x reach ~55 km, and on 20 km cells everything past 25 km was dropped (heads and hang cut
  on arcs). "Cb spacing" is now a minimum, and the change MOVES every tower (a snapshot framing a
  storm shows a different one). The hang footprint fits the search. A non-overshooting tower's dome
  apex sits 300 m inside the anvil (it was AT the lid for every tower); the column reports its local
  top (the dome's surface over the point — the apex made dark craters via the deck/ambient terms); a
  head's lobes and erosion fade over its last ~1.2 km (their lumps showed through the thin lid as a
  cell pattern). The mid layer's altitude reads the Perlin stretched to +-2 sigma (raw it was one flat
  sheet at ~4.5 km, a dead-straight line across storms seen from near its height) and gives way over
  storms (`CV2Col.storm`). Tried and REVERTED: sinking heads 900 m (+15 ms from above), joining
  towers and anvil by max (thinner storm, +30-50%), a light-march od cutoff + probe skip + opaque-ray
  stride (all three together SLOWER, ~13 ms). Measured (harness = in-app within ~10%): the user's
  pass-16 exe 33 / 64 / 42 ms cloud march at the three snaps; pass 17 30 / 85 / 55. Open: a
  waffle-grid texture on lit anvil walls (not the shape texels: a C1-smooth fetch did not change it),
  floating anvil fragments far from towers, and the storm cost.
- **Pass 18 (2026-09-29):** the corrugated tower walls ("waffle") were the tower strength read from
  weather mip 3 (~40 km texels) by hardware filtering, whose 8-bit sub-texel weights step every ~150 m;
  it scales an 11 km radius, so the wall moved in steps: vertical flutes up the whole tower. Now
  `cv2WeatherBilinear` (four texel-centre fetches blended in float, C1). A tower's radius goes to ZERO as the storm
  strength fades (sqrt(smoothstep(0.02, 0.5, str))): floored at 0.7 of its radius it vanished at full
  density where the strength crossed the 0.02 cutoff — a flat vertical crescent wall ~10 km wide, which
  the march drew with flutes and rings (the rest of the "waffle"). `cv2Add` weights the deck flag
  by density share (max() gave a tower crossed by a thin anvil the deck's path-to-top shadow at the
  tower's density: a black band). Open: wood-grain contour rings on smooth tower surfaces (the pass-14
  "contour ripples", obvious with low lobes); the mid layer's base seen from just below it draws a dark
  grazing band across storms behind it.
- **Pass 19 (2026-09-29):** a weaker tower is SHORTER, not thinner: its top (`topC`) sinks from under
  the anvil at full strength to its base at the 0.02 cutoff, its radius stays >= 0.8, and only towers
  reaching the anvil (`full`) get the waist, flared head and overshoot dome (shrunk in radius, storm-edge
  towers stood as thin "straws"). "Storm cumulus top" is a TARGET for the storm regions' low tops
  (`mix`, not `min`: the congestus type already topped out below it, so raising it did nothing).
  **Full rate only while moving** (`cv2SparseWhenStill`, slot 176, key `sparse_when_still`; default OFF since 2026-10-20 — High / Ultra march at full rate always, the lighter presets
  turn it on): after 8 frames with no camera turn (> 0.3 half-res px), eye motion, zoom, time warp or history
  loss, the march goes back to 1 pixel in 4 even above `full_rate_above_km`; the history converges on
  the same image. Measured at 1920x1009 overlooking anvils: 16 ms still vs 44 ms full rate; a settings
  change takes ~4x longer to reconverge (grain for a few seconds at history weight 0.05).
- **Storm cells (review 4, 2026-09-29, `cv2CbRole`):** towers come in CLUSTERS: the lattice is grouped
  in 3x3 blocks, a block holds a storm cell with probability "Cb fill" (`column3.w`, now per block): one
  DOMINANT tower (the only one reaching the anvil: flared head, overshoot, the anvil's hang, lightning)
  and a flanking line of 0-2 towers stepping down beside it (height x0.7 / x0.5, radius x0.8-0.9, never
  anvil-reaching). cloud_v2_lightning.comp flashes dominant towers only. A tower's outline BULGES
  (turrets: 3rd and 5th angular orders, turning with height, within ~23% so the lattice search still
  covers it), and a non-anvil tower's top rounds off over up to 3 km (flat boxes at 1.5 km). Defaults:
  waist 1.0 (none), flare 1.3, anvil hang 0.6 km — the waist, flare and hang around EVERY tower made an
  even forest of mushroom-capped smokestacks (user snapshot 7, review 3). The lattice moved again (the
  flare sets the cell). Open: the dominant tower's walls up close are still near-vertical.
- **Storms as clusters, not map regions (review 5, 2026-09-29):** the map types regions hundreds to
  thousands of km across as Cb. (1) The low layer's storm cumulus ("Storm cumulus top") now rises only
  within "Storm cumulus reach" (km, `column3.x` in METRES, 0-60) of a tower's edge — `CV2Col.prox`, from
  the column search, which cv2Field now runs BEFORE cv2FieldLow (`stormProx` argument; 0 for the eye's
  rain) — and away from towers a Cb-typed region is capped at the cumulus type's top. Taken over the whole
  region it was a continent-wide 5-7 km floor with the towers standing in it (user snapshots 1-2). The
  proximity is computed at every height (below the Cb base too: `colH`), or the low tops would step there.
  (2) The towers' flared heads ARE the anvils: the user tuned a "perfect anvil" with the map-fed shield
  OFF, and those settings are the defaults (radius 15 km, flare 1.6, waist 0.7, lobes 0.18, columns 1.5,
  storm feature size 12, fill 0.57, cumulus variation 0.8, anvil thickness 0.95 / hang 2.4 km, `anvil` 0):
  a 15 km tower sets ~42 km lattice cells, so storms stand ~125 km apart. (3) "Cb head lobes"
  (`cb_head_lobes`, slot 207, `motion.z`): the head (zeta 0.5-0.75 up, anvil-reaching towers) has its
  own lobe strength. (4) The map-fed shield (`cv2AnvilSigmaP`, still available) is smooth: fray 0.15
  (was 0.45), a soft edge (presence 0-0.6, squared), a gentler underside — it was lumpy and grey
  (snapshot 5). A separate storm layer not rooted in the map (hurricanes) is a possible later step.
- **Storm cumulus is wide cells (2026-10-26, issue 20261026_192322, "large narrow spikes"):** cv2FieldLow's coarse
  "deep" cells were chosen by the map's TYPE only, so the storm cumulus round a weak tower, raised to "Storm cumulus
  top" (12 km) on a cumulus-typed map, was 1-2 km cells extruded 10 km: rows of spires. `deep` also follows the cloud's
  own span (smoothstep 3.5-8 km of topMax - base). Ordinary cumulus A/B (cloud suite towers / trade / cu_air / sea):
  visually identical, cost equal; 128 registers.

- **Pass 20 (2026-09-29):** "Storm cumulus top" reaches the cloud AROUND a storm: the weather map
  types only a storm's core as Cb (which the towers cover) and the cloud around it as stratocumulus,
  so keyed on the Cb class the slider changed 0.2% of a storm view. Low cloud now takes it by storm
  proximity (weather type at mip 4, float-bilinear, 0.3-0.6), fully for cells, half for
  stratocumulus: 26% of the view moves between 2 and 7 km. The mid layer gives way wherever towers can
  stand (`col.storm` 0-0.15): a mid-layer sheet cut a weak storm-edge tower as a horizontal belt.
  "Storm cumulus reach (km)" (`cb_cumulus_reach_km`, 10, slot 177, `column3.x` (since review 5: metres from a tower's edge) = the weather mip whose texels
  are that wide) sets how far; the storm threshold falls with the mip (0.62 - 0.06 x mip), since a fine mip sees
  the core undiluted and at 0.3 it caught plain cumulus everywhere. Head drift applies to anvil-reaching towers only. The lumpy "fields" beside towers seen from altitude
  were the mid layer (debug view 7 green), not cumulus.
- **Perf sprint (2026-09-29, overnight):** the storm views' cloud march is ~62-67% cheaper at full rate
  (the user's 68 ms overlook: 22.5 ms), images within noise of before (`harness_runs/perf_sprint/`:
  `baseline.json` vs `cp1.json`, captures `cap_ref` / `cap_e14`). **What governs this shader's speed:**
  (1) every call site inlines the whole field (~60 KB of GPU code each): the light march, far sample,
  sky probe and beam occlusion now share ONE call (`lightAmbOD`), and the coarse and fine view samples
  another — 459 KB -> 220 KB, -45%; (2) NVIDIA gives it either 128 registers (small spill) or ~227 (no
  spill), and 227 is 20-60% SLOWER. Trivial edits flip it — a per-sample loop bound, three debug
  counters, values cached in globals — so check `shaders reload`'s Register Count (harness,
  docs/HARNESS.md) before timing any variant, and A/B variants interleaved in ONE live app (run-to-run
  noise is 5-10%). A smaller workgroup forcing fewer registers spills and loses (32x32: 2.4x slower).
  Kept: the one call site; **light LOD** (`cv2LightLodFootprintM`, "Light LOD footprint (m)", slot 178,
  key `light_lod_footprint_m`, 40, 0 = off: behind T < 0.5 or past that pixel footprint, 2 light steps
  over the same length with the first kept — dropping the first step flattened the towers' self-shadow);
  cirrus shaded at COARSE steps (a thin-ice sample stands for two steps and never drops the march into
  fine steps: they were a large share of the steps from altitude); the low field's altitude bound (no
  low cloud tops past the congestus top / "Storm cumulus top" + the ground's lift); `[[dont_unroll]]` on
  the field's small loops. **"Half rate while moving"** (`cv2HalfRateMoving`, slot 186, key
  `half_rate_moving`, default OFF): where the march would run at full rate it takes a checkerboard of the
  half-res pixels, alternating each frame (`cv2.misc.z` = 2; the resolve estimates a stale pixel from its
  four fresh neighbours and pulls a fresh one toward its diagonals in motion): -25% of the cloud bucket
  in storm views, but visibly grainier in motion than full rate (harness_runs/perf_sprint/motionb_c.png) —
  the user's to judge. Tried and dropped: a per-tile tower list (lists of dozens of towers from
  altitude: slower), a shared-memory tile cache of the lattice (slower), a lighting context caching the
  view sample's coarse reads in globals (the register flip), reusing the far sample / probe every other
  lit sample (0%), one call site for view + light (234 registers: +60%). Remaining cost: the towers'
  candidate search (~35% of the rest), mostly coarse steps through broken cumulus out to max distance.
  **From orbit (420 km, ~13-14 ms full rate) the cost is spread over the layers** — each off: Cb
  columns -3.5 ms, mid -2.6, anvil -2.0, high -2.0, light steps 1 -1.8, max_iters 80 -2.3
  (`harness_runs/orbit_lod/attrI.json`). **From inside the high layer (11 km, 2026-10-04)** the cirrus is ~11 of 32 ms: horizon
  rays stay in its band for ~200 km and every sample is evaluated and shaded (density does not change the cost); the
  anvil heads ~3, columns ~2.4. Its bounce term now reuses the field's flow (`gCv2FlowD`) instead of re-running the
  eight waves. Tried: 4x steps for far thin-only samples (-1.2 ms) and a 100-km sun-colour cache (-1.3, sunset
  colours change): not adopted. Tried and dropped: skipping the empty shell above a
  conservative per-column ceiling (tropopause top + overshoot, ground-lifted low tops) — 0% (the
  steps are inside the cloud, not above it); a vertical step cap growing with the footprint (200 -> up
  to 500 m) — -0.4%. An orbit LOD needs a cheaper field (an impostor), not fewer steps.
- **Perf pass (2026-10-13, `harness_runs/perf13/`: `mkvar.py` builds text-substituted march variants, `ab.py`
  times them interleaved in one live app, `tab2.py` tabulates).** Cloud bucket at 1920x1009, the user's settings
  (430 iterations, 7 light steps, full rate above 30 km, sparse below), before -> after: orbit storm 388 km
  78 -> 38 ms (limb 41 -> 21), 130-145 km 22-28 -> 14-17, 58 km 38-48 -> 22, anvils at sunset 17 -> 9.2, cumulus
  from 6 km 16.3 -> 9.2, under storms 10-13 -> 6-10.5; a climb over the anvils 14.1 -> 9.2 ms mean. All at 128
  registers, images within run-to-run noise except where noted.
  - **THE COST MODEL: a group of rays (32) runs in lockstep, so every iteration costs what its most expensive
    ray costs.** A lit sample is ten field evaluations (view + 7 light steps + far + sky probe); an empty step is
    one. In one loop, rays in empty air waited out their neighbours' light marches: time = (the longest ray's
    steps) x (a lit sample's cost). Everything below follows from it, and so do the failures: early termination
    of opaque rays (T < 0.02-0.05, tail filled) gained 1-4% (the group runs as long as its longest ray), and
    skipping work on alternating samples gains nothing unless every ray skips together.
  - **Density first, then light, in batches** (`kBatch` 32, `bF/bT/bTr/bCov/bW`): the march loop walks the ray on
    the field alone (coarse steps, fine lattice, refinement, transmittance and every depth statistic) and keeps
    the samples with cloud; a second loop lights the batch (`L += bW[bj] * S`). Same samples, order, weights. In
    the lighting loop `t`, `T`, `f`, `covB`, `rel`, `p`, `fp`, `detailAmt` are locals SHADOWING the march's —
    anything the lighting needs from the density loop must be buffered. Batches of 4 gained nothing from orbit,
    16 most, 24-64 the same. -14% .. -40% everywhere. Tried on the way: one sample per batch ("search, then
    light": +13-35% on overcast, every lit ray waited on the slowest search), a bounded search (233 registers),
    thin-ice samples at the batch's far end (noise).
  - **The Sun / sky colour table** (`cloud_v2_sunlut.comp`, `include/cv2_sunlut.glsl`, march bindings 13 storage /
    14 sampled, a 256 x 76 x 2 RGBA16F array: layer 0 the Sun's colour, 1 the zenith sky's): both are functions of
    height and Sun zenith cosine alone (x = cube root of the cosine, y = 800 m levels, so linear filtering IS the
    old interpolation between levels). Baked when `cloud_sun_rayleigh` / the sky's Rayleigh or Mie gain change
    (`cv2SunLutKey`) and after `shaders reload`. It replaced a per-ray cache recomputed at every level crossed
    (a 12-step zenith integral with Chapman columns; one ray refreshing stalled its group): -16..-18% from orbit,
    -6% at 58 km, ~-3% near the ground; old vs new at the noise floor at eight views incl. an eclipse and
    twilight. `sunTransmit` / `sunColorAt` / `skyZenithAt` now live in the bake (copies remain in
    cloud_v2_far.comp and cloud_v2_lightning.comp: keep in step). The march's fog pass reads the table too.
  - **Light steps merge in pairs under large pixels** (`lodN` in `lightAmbOD`: fp > 2 x the first light step,
    i.e. from ~100 km up): 7 steps -> 4 samples of the same lengths. Orbit -19%, nothing below ~60 km. Day
    cumulus identical; anvil lids at a grazing Sun read ~2 levels lighter — as they do with a 25-m view step,
    while 12 light steps read darker: a lid's brightness there is the light march's discretisation whichever is
    used. Its sample length must be written out at both uses (hoisted into one variable: 220 registers).
  - **Ground shadow gate** (cloud_march.comp): no `cloudGroundShadowV2` march where the texel and its eight
    neighbours have resolved cloud transmittance < 1% (the tent's reach). 0.5-1.3 ms near the ground, 2.6 ms
    from 58 km. That march runs for every half-res ground pixel every frame (32-72 field samples): it is still
    ~0.6-1.5 ms where the ground shows.
  - **Dead ends, measured:** a view-step floor at 0.2 x the pixel footprint (-17% from orbit, but faint contour
    rings on anvil lids; its dithered forms: 231 registers); the tower search rewritten by storm-cell blocks
    (4 hashes instead of 18: 255 registers, +20-40%), with a fake cheap role (220), or skipped in light samples
    (215) — every edit INSIDE `cv2ColumnSigma` flips the register count, edits in the march file mostly do not;
    `DontInline` patched into the SPIR-V for the field functions (honoured by the driver: 3-4x slower).
  - **What is left** (same views): near the ground the bucket is ~9 ms of which ~5 is not the march (composite
    ~2-3, resolve, the lattice bakes, ground shadow); the SKY PASS is now the larger item there (10-18 ms: its
    atmosphere loop ~9.5, the god rays 2-4 of that). From orbit: full rate vs sparse 38 vs 14 ms, Cb columns
    -30% (content as much as search), step base 120 m -15%, light steps 2 -14%, high layer -13%. The user's
    `shadow_ring1_cell_m` 100 (default 250) costs ~0.5 ms; `fovea_radius` does nothing with the adaptive rate off.
- **Cloud rebuild experiments (2026-10-21/22, `.plans/CLOUD_REBUILD_PLAN.md`, runs and `summary.txt` in
  `harness_runs/s0/`).** All OFF by default (Weather tab "Rebuild experiments", slots 302-308) except the flash buffer's
  placement. Measuring tools kept: `-DCV2_EVAL_COUNT` variants count field evaluations per caller (`cv2EvalCount` at the
  flash buffer's end, `state` -> `clouds_v2.eval_count`; compile by hand, load with `shaders reload march=`), and a 9-stamp
  per-pass GPU timer in the cloud bucket (`state` -> `clouds_v2.pass_ms`). Coarse view steps are 70-94% of field
  evaluations but **evaluation count is not cost**: empty-air evaluations are cheap (early-outs).
  - **Occupancy clipmap** (`cdc_skip` 2, `cv2_cdc.glsl`, `cloud_v2_cdc.comp`, march binding 23): 8 levels x 128 x 128 x
    64 cells (100 m doubling, 320 m tall) on the layer map's anchored plane, toroidal, stamped (wrap, epoch, a staggered
    sim-time quantum); each cell = the max coarse extinction over 36 points (3 x 3 x 4) of a 2-cell box at the march's
    own footprint; a coarse step in a known-empty cell takes its stride without the field, unknown cells evaluate it.
    Two passes (check + list, then a 4-lane bake: a one-pass bake serialised one lane per warp, 5-12 ms). 6 levels used;
    off above 4x time. -4..-9 ms below orbit at full rate, ~0 from orbit, images at the noise floor, motion (tstab) equal.
    Coarse-footprint bakes are NOT conservative (more misses); jumping to cell exits gained nothing.
  - **S2** (`cdc_light`, `cdc_light_levels` 3): light steps 3+ read the cell's mean + variance of the light extinction
    through the gamma model E[exp(-L e)] (a cell MEAN alone darkened broken cloud by up to 18 levels: Jensen). Modest.
  - **God-ray froxels** (`godray_froxels`, `cloud_v2_froxel.comp`, `cv2GodFrox` in the flash buffer: 320 x 180 x 64,
    log depths 50 m - 400 km): the sky pass reads it instead of 3 cascade + 24 shaft lookups a step; sky -0.2..-2.9 ms,
    images at the floor (the shafts only within their reach along the view, as the old path). **Sampling (2026-10-23,
    issue 20261023_003455 "pixelation on edges"):** the shafts were a 160 x 90 bilinear upscale (12-px tiles, one ray
    through each tile's centre): thin shafts aliased and every edge was a 12-px staircase, at any resolution (the HQ
    photo too). Now each froxel's ray + depth sit at a per-frame R-sequence point in its cell (`cv2FroxJitter`, `frox.w` =
    the frame index, -1 = fixed centres without the sky TAA) and the lookup reconstructs from the same points, so the
    sky TAA averages a tent over the tile; 2 depth samples a froxel (4 at fixed depths without the TAA; 1 was grain in
    motion: tstab walk 1.17 -> 1.54), read with `cv2StvTFine` / `cv2TerrSunTFine` (finest level out, stopping once
    covered: the froxel pass got CHEAPER at 8x the reads; the march keeps `cv2StvT`). Report view (1920x1009, 0.67):
    error vs the exact direct path in the crop 2.37 -> 1.51 levels, GPU +0.1 ms (sky +0.2, froxels -0.15), flash buffer
    +11 MB, tstab walk 1.17 -> 1.20 / pitch 0.65 -> 0.71. **The HQ photo uses the direct path** (`requestPhoto` sets
    `cv2GodrayFroxels` 0 for its duration): per-pixel shafts, photo 3.9 -> 6.3 s at scale 2 (the direct path is +22 ms
    a frame at 1920x1009 0.67: never for play). The cinematic HQ export keeps the froxels.
  - **Flash buffer in device-local host-visible memory** (ON; `SATLIGHTSIM_FLASH_HOSTMEM=1` restores system RAM): the
    Sun shadow lattices were read across PCIe by every lit sample and sky pixel; sky -0.9..-2.3 ms.
  - **Mie** (`tools/make_mie_lut.py` -> `assets/textures/cloud_mie.rgba16f`, march binding 24): `mie_phase` = the physical
    droplet phase (flatter clouds: ~20x less side scattering than the tuned HG pair, the MS term was tuned with the HG),
    `mie_optics` = the tuned phase x the table's structure (corona, cloudbow, glory), `droplet_reff_um`.
  - **Cap lobes** (`shape_caps`, noise mode 3): the shape volume's lobe channels as rounded caps, remapped to 1 - F1's
    mean / spread; a second volume, descriptors swapped at the top of recordCompute.
  - **Full-resolution clouds** (`full_res`, "Full-resolution clouds", S9, `cloud_v2_combine.comp`): the CLOUD grid
    (`computeCloudExtent()`: the v2 march / resolve images and the final cloud targets A/B) is twice the half extent;
    `computeHalfExtent()` stays half (scene depth, `cloud_march.comp`'s dispatch). The march always runs sparse there
    (1 full-res pixel in 4 = the half-res full rate's rays; full / half / adaptive rates off) and reads its grid from
    `cv2.grid` (no longer the scene depth's size) and the depth at `pix >> grid.z`. `cloud_march.comp` composites at half
    res into `cmHalf*` (bindings 5/6 rewritten, + 24/25: its storage images are at the 4 floor), taking the cloud as the
    full-res image's 2x2 box (the bilinear tap at the half texel's centre) and carrying the composite's DERIVATIVES with
    respect to the cloud's transmittance and radiance through every step (aurora / dust suppression, plumes in front /
    behind, beam lines, cirrus); the combine pass writes B = B_h + dB/db (b_f - b_h) + dB/da (a_f - a_h) and
    A = A_h + dA/da (a_f - a_h) per full-res pixel, all half terms bilinear: exact for the cloud alone and the affine
    steps, first order for the pow() suppressions. The half-res fix-ups (isolated texel tent, edge lean, missing distance)
    and the alpha's signed distance move into the combine at full res; the ground shadow is a [1 2 1] tent of the half
    texels. The sky pass skips its joint-bilateral upsample when the targets are finer than the scene depth. The resolve
    uses a [1 2 1] tent of the quarter grid (widened under 6-24 px of parallax) where it has no usable history — on EVERY
    sparse grid (half-res too: tstab "chaos" there 7.85 -> 4.91). **ON for Medium / High / Ultra** (the compiled default,
    `applyGraphicsPreset`: off on Low / Planetarium / Potato), 2026-10-22 (the user; Medium too, issue 20261022_180607: half res
    flickered badly at Medium's 67% render scale). The noise LOD stays the HALF-res
    pixel's (`motion.w` = 2): at the full-res pixel it cost +2 ms at 58 km and the sub-pixel noise flickered from orbit.
    **The march's scene depth is parallax-corrected on the full-res grid** (2026-10-22, issues 20261022_210358 "Tremendous
    flicker when moving above clouds", 1278 km at ~700 km/s, and _220214; `cloud_v2_march.comp` after the `tScene` fetch):
    the depth is the half-res texel's, traced through its centre, half a full-res pixel off the march's ray. Near the limb
    from orbit the ground's distance changes by tens of km over that half pixel, so every ray of the even (higher) rows
    stopped at the lower ray's ground, short of the cloud: on the frames whose sparse offset sampled even rows (the y
    offset alternates each frame) whole cloud systems vanished — a period-2 flicker in motion and "lost far masses"
    (`harness_runs/rc1022c-e`: debugview cloudtrans alternated 187 / 243 frame to frame; the sample held at an even row
    never showed the mass, at an odd row always did; a still view's sub-pixel jitter hid it). Fix: + the difference
    between the two rays' hits on the sphere through the depth's own hit (exact for flat ground at any height), bounded
    to +-25% of the depth (a ridge silhouette bleeds at most that on the half pixel it straddles); 128 registers. Before
    finding it, two workarounds went in and stay as A/Bs (`clouds_v2.full_res_moving_mode`, default 0 = the sparse
    rotation): 1 holds the sample at one pixel (no flicker; masses lost, since it held an even row), 2 marches every pixel
    while the EYE moves from altitude (`cv2EyeStillFrames`; +1-6 ms). After the fix, mode 0 vs 2 (`harness_runs/rc1022g`,
    the four issue paths): flicker (pixels reversing > 12 levels a frame) 0.48 / 0.39 / 2.69 / 0.79% vs 0.47 / 0.39 /
    2.69 / 0.93%, far band within ~1 level of settled, GPU 11.7 vs 18.0 ms on the 1278-km flight (before: 3.92% and
    -17.9 levels in mode 0). **The adaptive rate and the half-rate checkerboard never run on the full-res grid** (their
    tile targets are half-res sized: with mode 2, decelerating under 150 m a frame turned it adaptive and drew tile-sized
    squares, and a 165 deg/s pan at 455 km a stair-stepped frame of misplaced cloud — issue 20261022_220214). Also fixed: the
    clipmap motion guard counted a frame as slow when the cloud history was invalid (> 20 km a frame), so the fastest
    flight switched the clipmap back ON with stale cells. **Harness trap:** `report load` restores only the keys saved in
    the report: a `set` of a newer key persists into later variants; and a long `wait settle` at a pose leaves state that
    makes the next run's moving frame at that pose look right — A/B in separate runs or in a fixed order.
    Measured (1920x1009, clipmap on, vs half-res full rate, `harness_runs/s0/s9b`, `lodviews`): cloud bucket 58 km 12.5 ->
    14.9 ms, anvils 10.6 -> 12.1, cumulus 13.9 -> 15.7, Pacific 9.0 -> 10.9; resolve +1.0, composite + combine +0.2..+0.4
    (the probe that ran the composite at full res: +0.6..+5.3); 128 registers. Cloud edges full-res sharp
    (`harness_runs/s0/lod_crops.png`). Motion: tstab cumulus 2.91 -> 3.13, equal but "chaos" (fast flight inside cloud)
    2.1 -> 4.3. Edge flicker (issue 20261021_173131's three shots, `zig.py`, `harness_runs/s0/flk`, share of reversals
    > 12 levels): the user's settings 0.053 / 0.171 / 0.310%, half-res full rate 0.004 / 0.022 / 0.019%, full res 0.014 /
    0.063 / 0.065%. No validation errors. Not done: the lightning pass's engine-light ground term gathers the half-res
    depth with the cloud grid's weights. A user settings.json saved with `full_res` 0 (the key existed off since
    2026-10-21) keeps it off.
  - **Defaults (2026-10-22, the user: "continue forward"):** the occupancy clipmap (`cdc_skip` 2) and the god-ray froxels
    (`godray_froxels` 1) are ON; S2, Mie and cap lobes stay off. Default-settings totals at 1920x1009 (`harness_runs/s0/defs`,
    no validation errors): 58 km 26.3 ms (old defaults 34.0), anvils 29.0 (24.4), cumulus 32.0 (27.2), Pacific 27.7 (25.4):
    full-res clouds cost +2..+5 ms near the ground over the old half-res sparse march, the clipmap and froxels pay part back.
  - **The Sun transmittance volume (S3, 2026-10-22; `cloud_lightning.glsl` cv2Stv / cv2StvT, bake in
    `cloud_v2_lightning.comp` logical workgroups 5-17, each split over 8 physical ones of 32 rays x 8 threads (a ray's 48
    knots, 6 a thread, joined by a prefix of their optical depths; dispatch 5 + 13 x 8 — 2026-10-23: one thread a ray on 13
    workgroups was ~0.9 ms of latency with the GPU idle), UBO stvL0-3 / stvH / stvV):
    THE Sun shadow for the cloud key light, the god rays (froxels or direct reads in sat_sky.frag), the fog / dust and the
    GROUND (one lookup in cloud_march.comp). **The fine grid, the two shaft rings, the cascade and the layer shadow map are
    DELETED** (2026-10-22, the user: "delete it"): their bakes, buffers, UBO fields (sunCasc0-8, layEye / layRef / layBake;
    `layA/E/N` stay as the clipmap plane's axes, `cv2CdcAnchor`), `cloud_v2_layers.comp`, `cv2_layer_map.glsl`,
    `cloudGroundShadowV2`, and the settings "Far cloud shadows", "Far cloud shadow range", ring 2 cell / depth, "Layer
    shadows", "Cirrus self-shadow" and "Sun shadow volume" (slots 269, 282, 287, 288, 296, 297, 312 free); march set binding
    22 is unused (bound to the blue-noise tile). "Haze near clouds" moved to `mid3.w`, the resolve's fill gate to `cu2.w`.
    The flash buffer shrank ~5.5 MB. Sections below that describe those structures are history. 4 nested levels of
    128 x 128 parallel rays toward the Sun, cells = "Sun shadow cell (m)" (key `shadow_ring1_cell_m`, slot 286) x 1 / 4 / 16 / 64 (250 m .. 16 km, reach 56
    cells: ~14 .. 900 km), riding with the clouds and toroidal with the rings' wrap stamps; each ray holds the
    transmittance BEYOND its point at 48 HEIGHTS (0 .. the shell top + 1 km) on its SUNWARD branch (s = sC + sqrt((R+h)^2 -
    cL^2), cancellation-free), so a level covers every point within its reach across the Sun at any distance along it (no
    along-Sun window, knot shift or one-knot bias); a sample on the far side of its ray's lowest point reads the lowest knot.
    Windows centred on the eye, or from above mid-shell at mid-shell under it (the cascade's rule: from orbit the clouds
    below sat hundreds of km off across a low Sun). Bake: from the top of the shell DOWN each ray, steps ~2 cells growing
    1.2% with distance (<= 4 an interval), the field at a quarter cell (>= 150 m), 2 sub-rays (4 on a leading edge),
    running mean of 4; thin layers as the light march counts them (1 - 0.8 thin - mid2.z soft), rain at 15% (as the old
    ground shadow; full rain blacked out the sea under curtains at a low Sun). Level 0: 512 rays a frame (32-frame
    round), levels 1-3: 256 (64); levels 0-1 four times more while catching up (`stvV.z`: > 0.5 level-0 cells a frame,
    the 128 frames after, a fill's first 64). The lookup takes the finest level reaching the point, blended over 48-56
    cells; the key light reads it where the light march ends, half a knot up. **Measured against a brute-force reference**
    (the march compiled with `-DCV2_SUN_REF`: the field to the shell top in 100 m + 1% steps; `harness_runs/s3/ref1-3`,
    `orb2`, `orbtw`), mean |diff| old -> volume: cumulus under a storm 32.4 -> 2.4 (the old lattices missed the storm's
    shadow: the foreground lit), orbit 388 km 21.8 -> 15.9, anvil from above 4.3 -> 3.4, 58 km and twilight equal (both
    soft at ~100 km: an eye-centred lattice of 128 rays cannot resolve a far shadow's edge). Cost (bakes +0.3..1.0 ms,
    composite -0.3..-3 ms, march sometimes cheaper): defaults totals within +-1 ms. Motion: tstab cumulus 2.95 -> 2.21
    (walk 4.60 -> 1.90, walkpan 6.63 -> 1.95; boost 2.28 -> 3.22). Hop settle (45 km, `harness_runs/s3/hop*`): pixels past 16
    levels at frame 10 / 40: old 1.0 / 0.5%, volume 1.4-1.75 / 0.7-0.8%, equal by 160. 128 registers, no validation errors.
    The old structures remain behind the toggle; deleting them is the plan's next step.
    **Running mean floor (2026-10-22):** a ray's value is a running mean of its jittered bakes, 1, 1/2, 1/3, then a
    floor the CPU sets from how far sim time moves the clouds over one level-0 round (`stvV.z`'s fraction: 1/32 still or
    at 1x, 1/4 under warp; the integer part is the catch-up flag), stored with stochastic rounding (10-bit T). At a fixed
    1/4 the stored shadow kept the bakes' noise for ever: with time PAUSED the ground under an overcast wandered +-2-3
    levels over tens of frames and cumulus shadows shimmered (user: "shadows fade in, then go away"). Ground std under an
    overcast 1.17 -> 0.40; cloud_suite motion: overcast ground 1.29 -> 0.70, under a cumulus base 1.36 -> 0.77; cost 0.
  - **Cloud regression suite** (`tools/harness/cloud_suite.py`, docs/HARNESS.md): 11 daytime scenes at ordinary angles
    (cumulus from the ground / 3 km / cloud height, broken and overcast decks, sea, tropical towers, cumulus shadows on
    land at a 34 deg Sun from 3 km / 800 m / the ground, an overcast at 33 deg), each settled, time paused for 1200
    frames, time running at 1x, and five player-speed motion paths. Use it beside the sunset anvil views.
    **For anything that moves or changes altitude, run the MATRIX suite first** (`tools/harness/matrix_suite.py`,
    2026-10-22, docs/HARNESS.md "Matrix suite"): altitude rows 0 m .. 5000 km over one place x motion columns (still noise,
    walk, strafe, boost, look, rise, descend at the player's speeds for that height) + context rows (storm, sea, night,
    Reflect beams, aurora ground / orbit, ISS and Starmind in follow mode, a launch plume, the 2037 eclipse), one launch
    (~13 min `--quick`, ~22 full); `--fragment` pastes into a larger batch (captures `mx_*`), `compare` flags regressions.
  - **Anvil undersides are lit from below** (2026-10-24, issue 20261023_230636 "Anvil darkness", 6.6 km under a Cb at
    0.3 S 98.9 E): seen from below an anvil was near black navy (0.02 linear against 0.10 for the shadowed tower walls
    beside it, 0.74 for its sunlit top). Two causes, found with a march variant writing one pixel's terms to the eval
    counters (harness_runs/sl in the storm-look worktree): (1) the ground BOUNCE used `keyCol`, which carries the clouds'
    shadow AT THE SAMPLE (the Sun transmittance volume): every sample of an anvil's underside is in the anvil's own
    shadow, so its light from below was exactly 0. A HIGH sheet's underside sees the ground and the cloud tops far out
    round it, mostly outside its shadow (which falls h / tan(Sun elevation) away): from 4 to 9 km up the bounce takes
    `kBounceOpen` (0.6) of the UNSHADOWED Sun over an albedo rising from 0.15 to `kBounceAlbHigh` (0.3, cloud tops);
    low decks unchanged (their ground is the one they shade). (2) an anvil-reaching tower's flared head took the tower's
    whole zeta as `hf` (0.75-0.9 at its overhang), so `(1 - hf)` / `(1 - hf)^2` gave its underside no light from below:
    black blotches between the shield's lit underside. cv2ColumnSigma's `hf` is now (zeta - 0.7) / 0.3 in the head
    (`bestFull`), its density still by zeta. Underside 0.02 -> 0.14 linear (tower walls 0.10), grey with darker (not black) mammatus-like blotches;
    storm views elsewhere within ~1 level; cost within noise; 128 registers.
  - **Anvil puffiness** ("Anvil puffiness" `anvil_puffiness` 1, "Anvil top lift (m)" `anvil_lift_m` 800, slots 310-311, UBO
    `anvil3`; `cv2ColumnSigma`): the anvils are the anvil-reaching towers' flared heads, and their lobes and erosion were faded
    out over the head's last 1.5 km for the map-fed shield's thin lid (off by default): smooth plates with knife edges, "an
    entirely different material", and the light march's grain on the smooth tops. Now, without a shield: the head keeps its
    lobes (x (1 + 1.5 puff) "Cb head lobes"), its top is LIFTED by them (`bestLiftG`, the margin's rate of change with a
    vertical lift of the rounded top, carried out of the tower loop as one float; upward only, `max(lobes + 0.4 lobF + 0.12,
    0)`: troughs dug into the top read as holes) and its edge ramps in over up to 800 m (was 300). The erosion stays faded
    on the head's top (`headTopE`: its cracks read as rock). 128 registers, cost within noise (`harness_runs/anv2`: ab1-ab3,
    views at 25 N 134 W at sunset placed from snaps2 index 18). Still-view noise on the lit anvil from above 1.07 -> 1.29
    levels (std): it moves from grain over the smooth top to the billows' lit edges (`anv2/noisemap.png`).
  - The agent's own build tree for this work is `build-clouds` (gitignored), launched through `harness_runs/s0/runx.sh`
    (waits until no app instance runs: another session shares the machine).
- **Issue round (2026-10-13b, issues 20261013_184710 / _181252, `harness_runs/fb14`):**
  - **Edge trails in motion** (cloud_v2_resolve.comp, `dWide` / `eyePx`): a clear pixel with no cloud in its 3x3 new
    samples but cloud in the 7x7 takes that cloud's distance for its eye-parallax floor. It was floored at 50 km
    ("still" at any flying speed), so 2-6 texels behind a cloud's trailing edge the still view's 7x7 box (which holds
    the cloud) passed last frame's cloud at the still weight: granulated copies of every edge, in sparse-grid blocks
    (2.5 km/s at 2.5 km over cumulus; render scale 0.65 makes a block 6 screen px). Not from the batched march (the
    pre-batching shader shows it). Replay: `report load` + `path play fps=60 record=`.
  - **"Light march footprint" defaults to 0** (was 0.08): at 640 m the light march no longer saw a cumulus's own
    body — a backlit cloud was flat and glowing above a straight far-shadow line. The footprint read does not
    conserve optical depth for cumulus since the shape pass; do not raise it without fixing that.
  - **"Silver lining" is forward-scattered TRANSMISSION**: 0.12 HG(0.92) + 0.5 HG(0.8), x exp(-0.3 od), beside the
    single scattering's exp(-od). The narrow lobe alone at full extinction moved a backlit rim ~6 levels from 0 to 3.
    128 registers, cost within noise; five other views change < 1 level mean at 0.5.
  - **"Fast-flight cloud LOD" defaults to off** (the user: it looks terrible). Adaptive rate's default is unchanged.
  - **Thin layers cast no shadow in the NEAR lattices** (issue 20261013_192258; the shaft rings and the fine grid count
    `1 - thin - soft`, i.e. neither the high layer nor the mid layer's sheets). The rings carried the mid layer's
    shadow and the cascade does not deliver it (Sun 4 deg), so every cloud within a ring's reach of the eye was grey
    under altocumulus / altostratus and white beyond: a shadow that followed the observer. The cascade and the light
    march still count the mid layer at "Mid layer shadow". LOST: low cloud under an altostratus overcast is no longer
    shaded at all at a low Sun. The general answer (not built): a 2D map of the mid + high layers' vertical optical
    depth around the eye, baked in the lightning pass, read at the point where a sample's Sun ray crosses each layer.
  - **"Cloud tops too dark" (issue 20261013_193122) is the multiple-scattering setting at a 4-10 deg Sun**: no layer,
    lattice, tower or light-march toggle moved it; `ms_octaves` 4 / `ms_strength` 0.42 (the user ran 2.6 / 0.30)
    whitens the tops and keeps the relief; 6 / 0.5 is flat white. Defaults unchanged.
  - **Round 3 (2026-10-13c, five agents sharing one live app; briefs and diffs in `harness_runs/fb15/<agent>/`; harness
    `shaders reload` gained `resolve=` / `lightning=` / `composite=` so each batch names its own SPVs):**
    - **Sunlit tops grey (193122) was the DECK term** (`odDeck`, cloud_v2_march.comp): sigma x the path to the layer's
      nominal top, which sits hundreds of metres above a lumpy top's real surface — x 1/sin(4 deg) = km of cloud (od ~15
      where the light march found ~1). It now counts only the path beyond 2 x the light march's length. Not the
      multi-scatter settings (those only hid it).
    - **The time-warp fringe (193400 shot 2) is the SKY TAA**, not the cloud resolve: with the eye still it holds the
      still weight while clouds move on their own (drift + wind), and its 3x3 clip cannot reject over the sea (glitter
      widens the box). recordSkyTaa raises the still weight toward the moving one by the clouds' own screen motion at
      max(1.5 km, eye AGL): ~2.7 px a frame at 10x, unchanged at 1x and paused.
    - **Horizon smears climbing (202117)**: a far cloud's own parallax (~0.1 px a frame at 700 m/s) called it still: wide
      7x7 box at the still weight, and near clouds' edges left their history on far pixels. The still box, slack and
      variance-clip ramp follow `mNb` = the NEAREST cloud's parallax in the 3x3 / 7x7 x (1 / still weight); the weight
      itself keeps the pixel's own motionPx. Horizon band vs settled 3.99 -> 2.85 levels, near band unchanged.
    - **The cirrus wedge (192258 shot 3)** was the mid layer's shadow in the fine grid, a 400-km tube along the Sun when
      the cascade is off; fixed by d0ebf6e (the user's exe predated it).
    - **Sunset grain in motion (201800) is NOT fixed.** It is the far light steps (k >= 3 of 7: 0.5-3 km each, one point
      sample: lit or black by chance). Reading them at a 2 km footprint cut the moving grain 8.1 -> 2.2 levels but
      ERASED large clouds' far shadows (a Cb anvil's shadow over a cumulus field: mean +11 levels) — coarse field reads
      lose cumulus / tower optical depth, the same non-conservation as "Light march footprint". Reverted. Needs an
      unbiased estimator (several stratified point samples, or the shadow lattices' baked means for the far steps).
    - Plan for layer shadows: `.plans/LAYER_SHADOW_MAP_PLAN.md` (coarse reads: altocumulus ~2x too thick at 2-8 km,
      cirrus 0.33x; altostratus fine).
  - **Round 4 (2026-10-13d, three agents, `harness_runs/fb16/<agent>/`, verify batch `harness_runs/fb16/verify.satcmd`):**
    - **The horizon band ("since forever", issue 215856) was the sky pass's Earth-shadow test** (sat_sky.frag N_VIEW
      loop): each ~15-km step was lit or shadowed by its MIDPOINT, and near the horizon the first step holds almost all the
      haze (H_M 1.2 km, "Haze near clouds"' 2.5 km), so it flipped as a block at the elevation where its midpoint left the
      shadow (4.3 deg at a -0.84 deg Sun; the 2nd/3rd steps at ~1 / 0.4 deg): a hard brightness/colour step, and below it
      the grey sky-lit air laid over every cloud past ~15 km through `airFront` at the half-res cloud distances — the
      blurred, blocky "ghost" band. Now the ray's interval in the shadow cylinder is solved once per pixel and each step
      takes its sunlit SHARE (`tShA/tShB`, `litF`). Row luminance across the old edge 186.1 -> 177.7 before, smooth now;
      within 2.9 levels of 40 view samples. Same midpoint test still in the sea-reflection marches, `skyZenithSky` and
      the flat layers' twilight block (not changed). A resolve that outputs flags (fb15 fringe `v_DBG2.comp`) is how it
      was found: cloud shapes showed through a resolve that drew none (the air split's distances).
    - **Grain on moving clouds (214939) is the light march's shared jitter `jL`** (raw per-sample std 23.5 levels at the
      user's settings; view jitter, sub-texel, far sample, IGN vs blue noise: no change), let through by the resolve's
      motion clamp: all-or-nothing samples make the 3x3 box collapse and clip the history's mean to one value (blocky
      speckle). In motion the COLOUR boxes (mean colour, radiance) widen to the 5x5 new samples within +-25% of the
      pixel's distance, fading out over 1-3 px of the neighbourhood's parallax (no `lagK`); opacity keeps the 3x3 clip.
      Walk grain 3.66 -> 2.16, 1 s after stopping > 16 levels 13.1% -> 0.4%, still view unchanged. Not changed: the
      shared `jL` itself (per-step `fract(jL + 0.618 k)`: std 23.8 -> 17.2 but the settled image ~18 levels darker,
      i.e. less Jensen bias — the user's call); the orbit flicker (small puffs, shot 4).
    - **Far shadows broke when moving (220718): the shaft rings are toroidal**, so slots at the trailing edge became the
      leading edge holding the shadow from ~a window away until their bake (every 32 frames), then blended out at 1/4.
      Ring values now carry a STAMP (`cv2ShaftPack`: 10 bits of T + the slot's offset from the eye in bins); a lookup
      drops slots that changed hands (the next ring / the cascade stands in) and a bake takes them whole. Rings are
      cleared on a fill start (`kCv2SunShaftOffset`), fade in over 16 frames (was 33 + 8), the cascade from frame 4.
      Fill: > 16 levels 41.8% -> 0.7% by frame 20 (was 130 frames to the floor); a 6-km hop 15% -> 1%; +0.3 ms. Exact up
      to ~60 ring cells per 32 frames (~12 km/s at 40 fps at 159-m cells); a wrap count via modP 256/64 would make it
      exact to ~50 km/s. The cascade is gated off above 60 km eye altitude (could go to ~600 km with the range scaled).
  - **Round 5 (2026-10-14, `harness_runs/fb17/<agent>/`; two live apps in parallel — fine while not timing fps):**
    - **Orbit / high flicker (214939 shots 4-5) was the resolve's motion clamp under TIME, not the camera.** With time
      running the drift moves the eye ~0.7 m a frame relative to the clouds; the eye-parallax floor capped the distance
      at 50 km and Round 4's lagK (x 1 / still weight = 50) turned that into "moving": tight box + variance clip, and every
      sub-pixel puff / wispy edge (hit or missed by the jittered ray) flipped between sky and cloud each frame, its age
      reset by the rejection. Paused, the sky TAA's FROZEN mode hid it (sky TAA off: paused flickers too). Now the cap
      is max(50 km, 2 x eye height) and a view whose TRUE parallax (no lagK) is under 0.05-0.15 px takes its history
      unclamped (`uK`). Per-frame pixels past 8 levels: 158 km 0.10% -> 0.0002%, 20.8 km 1.14% -> 0.00002%; tstab at
      shot 1 (paused and live time) and 184710 within the base-vs-base floor. **Harness trap:** `path play` with time
      paused holds sim time (`simRate` = 0) — `time play` first or the path measures a frozen sky; `report load` pauses.
    - **Light march: EQUAL steps, each its own jitter** (`kLightRatio` 1, `fract(jitter + 0.618 k)`; merge threshold kept
      at the old first step). The 1.6 geometric series made the far steps single point samples for 1-3 km, and the
      history converges on the mean of exp(-od) (Jensen): under a low Sun, clouds in another's shadow were 30-60 levels
      too bright vs a 128-step reference; now +8-10 (raw std 26.5 -> 9). Look: shadowed sides darker (shot 1 -7.6, the
      201800 sunset -11.2 levels mean), high Sun / orbit unchanged; GPU within noise; 128 registers.
    - **Far shadows at any speed:** ring coordinates go to the GPU mod 1024 cells / 256 knots so a stamp records which
      of 8 wraps a slot holds (exact to ~8 windows between bakes); 2 bits per value count bakes (per-slot mean 1, 1/2,
      1/3, 1/4); moving, one workgroup per ring bakes the 2-4 columns at the leading edge (`sunCasc5.zw` = the eye's
      motion in ring 1 cells) while the other keeps a 64-frame round; the fine grid stamps knot 16; the cascade stays on
      while the march draws clouds (was cut at 60 km: a pop), rings and god rays still stop at 60 km. 4.5 km at 30 km/s,
      pixels past 16 levels after 10 / 40 frames: emulated HEAD 6.0 / 2.5% -> built 2.6 / 2.3% (the rest is the user's
      0.02 history weight: 3.4% at 160 frames with the previous resolve, 2.4% now).
    - **Earth-shadow midpoint tests** (Round 4's band) also fixed in `skyZenithSky`, the flat layers' twilight zenith,
      `skyAmbientTerrain` and the sea's sky-reflection march (`earthShadowSpan` / `earthShadowLit`); offline replicas
      52% -> 2.5% max step per 0.01 deg of Sun. Root of the class: `optDepth`'s 2 Sun-path samples (the user's light
      samples 2.4) under-count a grazing path; `sunTransmit` (Chapman) there would remove it (look change, not done).
    - **Follow-up (2026-10-14):** the unclamp is gated by `mid3.w` (CPU: 0 until the Sun shadow lattices have filled
      for 64-96 frames after a reset): unclamped at the still weight, shadows arriving after a load darkened the clouds
      over seconds. And **ring 2 was cleared EVERY FRAME with time running at a 6-18 deg Sun**: its refill test compared
      `dsS2 + cell2` exactly, and dsS2 (5000 m / sin el) follows the Sun. It now refills on a new cell or a > 10% change
      of its knot spacing (`cv2Shaft2KnotUsed`). Shot 5 (20.8 km) running: 0.88% -> 0.00002% of pixels past 8 levels a
      frame; load fill > 16 levels at frame 20 / 40: 1.0% / 0.04%. **2026-10-14 02:33 the PC rebooted with two live
      harness apps running in parallel** (freezes not fixed after all): one app at a time.
    - **Shadow settle after fast moves (2026-10-14, the user's shot 7: "move backwards: cloud shadows fade in"):** after a
      30 km/s hop from shot 7 the clouds' own history settles in ~10 frames and the shadows took ~160 (pixels past 16
      levels at 10 / 40 frames: 2.4 / 1.5% with far shadows, 0.1 / 0.01% without). Ring 0 is the culprit (at 2-km cells
      the same hop settles to 0.27 / 0.01%): a 45-km hop moves the user's 159-m ring ~280 cells, so the whole ring changed
      hands and refilled over 32-frame rounds and four more bakes of its running means. Now: workgroups 17-18 (dispatch
      19) bake ring 0 a second time while it catches up (`cu2.w`, CPU `cv2ShaftBoost`: moving > 0.5 cell a frame, the 128
      frames after, its first fill): 4 more leading-edge columns moving, a second round half a round out of phase still;
      and a LEADING-EDGE bake (a slot's first) takes 4 sub-rays instead of 2. Interleaved A/B over the hop: frame 10
      1.6% -> 0.4-1.2%, frame 40 0.45-1.05 -> 0.1-0.5%; +0.2-0.35 ms of cloud time while moving fast, none still. The
      CASCADE got the rings' wrap stamp too (its last knot, always 1, carries 3+3 bits of wrap and a bake count; the CPU
      passes its lattice coordinate mod 1024 / 256): a slot that changed hands is rebaked whole instead of blending the far
      side's shadow out at 1/8 a bake. Neutral in this test (the motion barely wraps the visible cascade). Benchmarks:
      `harness_runs/fb17/back.py` (backwards flight at 1 / 5 km/s vs settled poses: ~3% of pixels, mostly the clouds'
      history, not shadows) and the hop batches there; compare a hop against its OWN late frame (two fresh loads of one
      pose differ by ~1.8 levels: the settled shadows depend a little on the lattice's phase).
    - **Layer shadow map (2026-10-14, `include/cv2_layer_map.glsl`, `cloud_v2_layers.comp`; `.plans/LAYER_SHADOW_MAP_PLAN.md`):**
      the mid and high layers' shadow on everything under them, at any distance. A 2D map on a gnomonic plane tangent at an
      ANCHOR fixed in the drifted cloud frame (CPU `cv2LayAnchor`, re-anchored past 1500 km: a refill), two levels of 256 x 256
      toroidal cells (1 km; 6 km doubling to 48 km with the eye's height so it covers what can be seen from orbit), every value
      stamped (window wrap + epoch, a cleared buffer reads invalid). Device-local buffer, march set binding 22 (6 MB).
      - **Columns** (mode 0, 32 workgroups a frame, each cell every 16 frames): per cell a running mean of each layer's
        VERTICAL tau, tau^2 and tau-weighted height moments, from jittered points read at a 100-m footprint (coarse reads do
        not conserve the layers' optical depth, plan F2). `cv2MidColumn` / `cv2HighColumn` are HAND-KEPT COPIES of
        `cv2MidSigma` / `cv2HighSigma` (2D terms once per column, kLayK = 6 stratified heights; keep in step), with
        cv2Field's two suppressions (mid under Cb storms, cirrus under anvils). 4 points a bake on a cell's first bakes, then
        1; the mean converges (1 / bakes, 64 at most) and moves only as fast as sim time moves the layers (`layBake.y` = the sim
        time between bakes / 60 s: 0 paused). Re-jittered every bake with a 1/8 floor it never stopped moving, and the history
        took seconds to settle after a hop.
      - **Slant** (mode 1, a quarter of the cells a frame): per cell X on the layer's reference sphere (5 / 9.5 km), the Sun's
        transmittance through that layer along the whole Sun ray through X: 16 FIXED taps from where the ray rises out of the
        band's floor to its top, each tap the overlap of its height stretch with that column's band (its centroid +- the
        uniform-slab thickness), gamma model E[exp(-m tau)] from mean + variance, x the shadow share ("Mid layer shadow";
        cirrus 0.2, delta-scaled ice). First cut took the FAR root of the band floor (the ray's lowest point, 670 km back and
        underground at 6 deg): slant tau 0.16 where the columns said ~8.
      - **Lookup** (`cv2LayerShadowT`, one call site in the march's key light, 128 registers): where the sample's light march
        ends, the Sun ray's crossing of each reference sphere (the root on the Sun's side of the ray's lowest point), four slant
        cells bilinear (near level, the far one where it is not baked or reached), T^(share of the band above the point). The
        light march counts what it saw; the map the rest. At a 35 deg Sun it changes nothing (+0.09 levels).
      - With the map on, the cascade drops the mid layer too (all layer shadow from one place: the cascade's mid shadow on the
        AIR drew shot 3's fake sunbeam wedges; the air under a sheet is now unshadowed by it, god rays from the map are a
        follow-up), and the deck term leaves the mid layer's share to the map.
      - Validated against a brute-force march of the field along the Sun ray (`harness_runs/fb18/l_dbg4.comp`): T at 0 / -10 /
        -20 km from the eye under the shot-2 altostratus 0.85 / 0.27 / 0.006 vs 0.95 / 0.02 / 0 (the map softens the sheet's
        edge). Issue 192258: shot 2 deck -5 levels mean (the sheet ends ~35 km sunward, so the deck under the eye is lit from
        under its edge at 6 deg), sideways -8.5; no edge following the eye; none from 300 km. Hop 45 km at 30 km/s: >16 levels
        at 10 / 40 / 160 frames 0.60 / 0.02 / 0% (off 0.35 / 0.03 / 0%). Cost +0.2 ms GPU. Settings "Layer shadows (mid / high)"
        `clouds_v2.layer_shadows` 1, "Cirrus self-shadow" `cirrus_self_shadow` 0 (slots 296-297). Harness `state` ->
        `clouds_v2.layer_map` (the probe at 1.5 km under the eye: columns, slant T and tau where its Sun ray crosses).
    - **The cascade from orbit (2026-10-21, issue 20261021_002345 "Odd shadows from space": a sharp-edged trapezoid of black,
      ray-streaked cloud under the eye at 333 km).** Two bugs, both only above the shell:
      (1) **The lattices' axes (`cv2SunProf[2..7]`) were written only with the eye inside the shell** (lightning thread 1's
      Sun-profile branch). The cascade stays on from orbit, so every lookup used the axes of the LAST place the eye had been low
      (another ENU frame, another Sun): each cloud sample mapped to a wrong ray and knot and the window drew its rays' total
      optical depth. After a fresh load at altitude the axes were 0 and the cascade silently did nothing (which is why a
      `report load` could not reproduce it: visit a low point first, `harness_runs/` r13 in the fix's worktree). Now written
      every frame.
      (2) **The knots ride with the eye along the Sun, not with the clouds**: a and b are integrated (above), but a ray's knots
      stand at fixed distances from the window's centre under the eye, so a bake read a few frames later (or a running mean)
      was shifted along its rays by the eye's motion since — at the height-scaled speed from orbit (~330 km/s) by several knots:
      a dark rectangle under the eye while flying. The CPU integrates the eye's motion along the Sun (`cv2CascS`), keeps each
      ray half's knot origin at its last bake (`cv2CascBakeS`; half (frame >> 1) & 1 bakes on odd frames) and passes the shift
      since (`cv2.sunCasc8.xy`, UBO 1056 -> 1072 B past the types); `cv2SunCascT` reads each corner's row half at its own knot,
      and a bake taken while the eye moved replaces the slot's running mean (`sunCasc3.w` = 1). 128 registers; the ground storm
      view 38.4-38.8 ms old vs new interleaved, the shot's view 11.0 vs 11.7 ms cascade on / off. Harness `state` ->
      `clouds_v2.sun_cascade` / `sun_cascade_ubo`.
      The same shot's faint circle round the nadir was the terrain march's range ("Terrain fade end", 600 km there): only terrain
      hits took the ground bounce (`bounceK`), so snow inside the range was ~15% brighter than the same snow past it. Land past
      the range takes it too now (`cityLand && tdEnabled()`).

  - The user's settings.json was lost to release.bat's wipe on 2026-10-13; a copy from that morning is
    `harness_runs/perf13/user_settings.json`.
- **Multiple scattering = a delta-Eddington diffuse field (2026-10-26, branch cloud-ms; `cv2DiffuseField`,
  cloud_v2_march.comp; the user, after a cloud-plugin developer's notes: "edge darkening is not present in our march").**
  The octaves (`msOctaves`) knew only the optical depth toward the light, so every sunlit sample (a 50-m wisp or a 5-km
  tower) took the same multiple scattering: flat-shaded noise, no dark wisps, a front-lit cloud a flat disc to its rim.
  Now each lit sample's multiple scattering is the diffuse field of a conservative delta-Eddington slab along the light
  (f = g^2, t = (1 - f) tau, Eddington I0 + mu I1 with Marshak boundaries, closed form): tauT = the light march, tauB = a
  march AWAY from the light (`lightAmbOD` k = nL + 5..6, a quarter of `ms_back_m` then the rest; the clipmap's mean
  extinction where `cdc_light` bakes it, else the field; past a 150-m pixel the path to the column's base, no reads).
  Toward the eye: I0 + 0.3 g mu I1 + the forward peak's extra beam scattered once more, x e^(-0.05 t) (light escaping
  sideways), x the leak through the face the eye looks at, 1 - e^(-(1 - f) x), x = the view's optical depth INTO THE
  CLOUD THE RAY IS IN (`Tent`, reset at every empty step: from the eye, the next lobe's face read as interior = bright
  veins on cloud bases), AVERAGED over the sample's step (`bH`: a first sample is often several optical depths thick, and
  at its start x = 0 it got none). The three corrections and the 1.6 are fitted to a path-traced reference
  (`tools/cloud_ms_reference.py`: soft / hard / lumpy / cauliflower blobs lit from above, the front, behind, the side; one
  gain for all): correlation old 0.78-0.92, new 0.89-0.96; level vs reference old 0.32-12.8x (thin cloud 4-13x too
  bright), new 0.41-1.54x. The reference's multiple scattering is SMOOTH at lobe scale: the old crisp lobe shading was
  too strong; lobe relief comes from the single scattering (unchanged). Thin ice keeps the octaves; "Powder" applies to
  the old model only. Settings (Clouds tab, slots 342-345): `clouds_v2.ms_model` 1 (0 = the octaves, the A/B),
  `ms_diffusion_g` 0.85, `ms_diffuse_gain` 1 (~ the octaves' energy in a thick cloud; ~1.6 = path traced),
  `ms_back_m` 1200; UBO `cv2.ms` appended (+16 B). 128 registers. Cost (RTX 3070 Ti, 1920x1009, matrix `--quick`, user
  settings): cloud march +0.1..+0.8 ms, storms +0.1 (15 km) / +0.8 ms (ground), orbit -0.3; still noise and every motion
  column equal or better (3 km look 1.14 -> 0.79, noise p99 0.73 -> 0.46). The light-LOD gate now reads the transmittance
  within the current cloud (`bTr`), not from the eye. Tried and dropped: the slab alone at physical strength (3-7x the
  old energy, flat: a slab along the light assumes mu0 = 1 and holds the light in), the octaves x the slab's reflectance
  over the sample's neighbourhood (saturates in dense cumulus: no visible change).
- **The sky's light on the clouds = the hemispheres with multiple scattering (2026-10-26, branch cloud-ms;
  cloud_v2_sunlut.comp layers 2-4).** The march took the ZENITH's single-scattering radiance x "Sky ambient" (2 in the
  user's settings) x the twilight boost ("Twilight sky light") and the dusk fade: the zenith is the darkest part of the
  dome, by a different share at every Sun elevation. The Sun / sky table (now 5 layers) bakes in three passes (one
  dispatch each, the dispatch's z count = the pass): Hillaire's multiple-scattering function Psi = L2 / (1 - f) (EGSR
  2020; layer 4's 32 x 32 corner, mu_s x sqrt(height); 64 directions, the ground at "Ground albedo (sky light)"), then the
  upper hemisphere's mean radiance (8 equal-solid-angle rings x 12 azimuths, 20 quadratic steps each, single + Psi
  multiple scattering), then the lower one (the air below + the sky-lit ground: albedo x the upper hemisphere at the
  ground; the SUN-lit ground stays the march's ground bounce). Baked when a gain or the albedo changes. The march: each
  hemisphere x 0.5 (half the sphere a sample scatters from), the upper through `ambVis`, the lower x `ambLow / 0.35`; no
  twilight terms. Settings `clouds_v2.sky_light_model` 1 (0 = the zenith), `sky_light_gain` 1 (physical),
  `sky_ground_albedo` 0.25 (slots 346-348, UBO `cv2.amb`). At physical gain the daytime look matches the user's
  zenith x 2 within a few %; twilight clouds a little darker (the old boost over-lit them); anvil undersides lighter
  (the lower sky). 128 registers, +2 texture reads per lit sample.
- **The droplet phase (2026-11-04, branch qa/20261104/cloud-phase; `cv2Micro*` + `cv2DiffuseField`, cloud_v2_march.comp;
  `cv2.micro`; the user: "water droplets have a very peaky phase ... it changes with droplet size ... the phase isotropizes
  the deeper light travels ... clouds are really bright in their cores").** Water and ice cloud with a light march (not the
  thin high layer, not rain: those keep their own phases) take a per-sample microphysics instead of the tuned lobes:
  - **Size**: the effective radius grows with the height above the cloud's base (adiabatic liquid water ~2 g/m^3/km, half
    of it realised, ~200 droplets/cm^3): r_eff = 1.15 um x (dz + 40 m)^(1/3) x "Droplet size", clamped to the Mie table's
    4-30 um; dz from the sample's height fraction and its column's top (hf (top - h) / (1 - hf)). ~5 um at a cumulus base,
    ~12 um 1 km up, ~20 um 5 km up a tower. **Ice**: the air (sea level under the eye less 6.5 C/km; `cv2SeaTempC`) glaciates
    from -10 to -35 C, so Cb heads and anvils take an ice phase (a 0.45 peak at g 0.995 on HG(0.55): mean cosine 0.75, no
    bow or glory). One temperature for the view (the eye's sub-point): from orbit far clouds take the eye's climate.
  - **Single scattering** = the Mie table (`cloud_mie.rgba16f`) at the sample's radius, its full share (row by a quadratic
    fit in log r to the table's radii, `cv2MicroRowV`, to 0.2 of a row; the load logs a warning if a regenerated table no
    longer matches). The "Mie phase" share, "Mie optics", the tuned lobes and "Silver lining" stand aside in this model.
  - **Low orders** (`cv2MicroOrders`): light scattered k = 1..3 times on the way in (Poisson in the optical depth toward the
    light) then once toward the eye, at the droplet phase composed k + 1 times. Composition multiplies Legendre moments
    (HG(g) * HG(g) = HG(g^2) exactly), so the peak goes as share^n, HG(g^n) and the rest is an HG keeping the composed
    mean cosine gtot^n; the peak shape per size is Jendersie & d'Eon 2023's HG + Draine fit (d = 2 r_eff). This IS the
    silver lining and the backlit glow, and the order-by-order isotropization: a thin wisp is lit by orders 1-2 (peaky:
    dark from the side, bright toward the Sun), a thick core by the diffuse field.
  - **Diffuse field** (orders past 3) = `cv2DiffuseField` at the droplets' own mean cosine (0.86-0.88 water, 0.75 ice),
    without its Eddington-phase forward-beam term (the low orders carry it), x T / (T + 2) (T = od + odBk, the chord along
    the light: a small cloud loses diffuse light out of its sides) x "Droplet multi-scatter gain" 1.8 (x "Multi-scatter
    diffuse strength").
  - **Fit** (`tools/cloud_micro_reference.py`: the ms reference's blobs path-traced with the JdE phase at d = 20 um, its
    peak capped at g 0.97 to keep next-event estimation from spiking; levels = model / reference, 14 blobs x lights):
    the shipped HG model (+ silver) against droplet cloud: geo-mean 0.69, backlit 0.38-0.42, thick tops 0.66-0.72; this
    model: geo-mean 1.01, thick 0.99-1.08, backlit 0.68-0.78, thin soft blobs 0.9-2.4 (they were 0.9-2.0). Without the low
    orders backlit drops back to ~0.3-0.5; the T / (T + 2) term cut the level spread 1.46 -> 1.36.
    At the centre of a FRONT-lit blob (Sun behind the eye; ms + single scattering vs reference ms + exact single):
    thin (tau 2) old 0.61 / new 0.22 / ref 0.19; tau 5 0.84 / 0.53 / 0.46; tau 30 1.38 / 1.48 / 1.46; lumpy 60
    1.50 / 1.69 / 2.20; cauliflower 80 1.54 / 1.84 / 2.39. The tuned pair's backward lobe (0.4 HG(-0.25): ~7x the droplets'
    side-to-back scattering) lit thin front-lit cloud 3x too bright and left thick cores dark; the droplet model brightens
    the cores and dims the thin cloud (the thickest still ~0.77 of the reference: e^(-0.05 t) and T/(T+2) saturate).
    The fit holds at d = 10 um (geo-mean 1.04, spread 1.37).
  - In the app (user settings, 1600x900, `harness_runs/cloudphase/`, luminance levels on changed pixels, new - old):
    shaded deck under a low Sun +5 (cores and bases lighter), Pacific towers +2.8 (their darker parts +2.6), front-lit
    trade cumulus -4 (its bright tops -6), the fair-weather field from 400 km -9, from 30 km -4, backlit ~0 (the low orders
    replace the silver lining at about its level), cumulus from below -2.5 with less lobe relief (the Mie single
    scattering is flatter at the side than the tuned pair, so the relief is the diffuse field's, which is smooth).
  - Cost (RTX 3070 Ti, 1600x900, two other agents' apps on the GPU, interleaved x3 against the branch compiled out, A-vs-A spread 0.1-0.2 ms): towers
    / cumulus from 3 km / towers from 6 km / orbit all within the A-vs-A spread (<= +0.2 ms GPU); 128 registers both;
    binary 389,760 -> 394,752 B.
  - Settings (Clouds tab, slots 486-488): `clouds_v2.droplet_phase` 1 (0 = the tuned phase, the A/B), `droplet_size` 1,
    `droplet_ms_gain` 1.8. Needs "Multi-scatter model" 1 and the Mie table. UBO `cv2.micro` appended (+16 B).
- **The sky pass's multiple scattering + ozone (2026-10-26, branch cloud-ms).** The sky (sat_sky.frag's atmosphere loop,
  sky_ap.comp's volume, the sea's reflected-sky march) was single scattering: the air in the Earth's shadow was lit by a
  hand-made "sky-lit shadowed air" term (review 7), and the Sun's path had no ozone, so the twilight sky read olive /
  brown-grey. Now each step adds the Psi_ms of the clouds' bake (Hillaire 2020: the sunlit air's multiply scattered light
  per unit of scattering, by height and Sun cosine) x (beta_R rho_R + beta_M rho_M) x the camera transmittance x the
  eclipse factor, and the review-7 term is skipped. cloud_v2_sunlut.comp's pass 1 also writes Psi into the flash buffer
  (`cv2SkyPsi`, 32 x 32 vec4 after the terrain sun horizon, `kCv2SkyPsiOffset`; cloud_lightning.glsl `cv2SkyPsiAt`, a
  manual bilinear: the sky set's sampled images are at the 16 floor). The Sun's path to each sample is exact
  (`sunPathTauExact`, sky_atmos.glsl: the Chapman R / M columns + ozone) instead of optDepth's midpoints. **Ozone**
  (atmosphere.glsl `atmOzoneTau`, every copy: sunlut, far layer, lightning, sky) is a uniform layer from 15 to 35 km
  crossed for the ray's exact chord: it was a thin shell at 25 km x the clamped secant, infinite at the tangent, so for
  points above 25 km the ozone jumped 0 -> ~1.6 optical depths where their Sun ray grazed it (a hard-edged ghost in the
  twilight sky once the sky used it). The volume's additive term (shadowed air / MS) is stored x `kSkyApShScale` 1e4
  (sky_ap.glsl): ~1e-7..1e-5 per unit air is below a half float's normal range and drew contours. Setting
  `clouds_v2.sky_multiple_scattering` 1 ("Sky model (0 single / 1 multi-scatter + ozone)", slot 352, UBO `cv2.amb.w`).
  SKY_LITE / Potato / SKY_ENV keep the old model (`sunPathTauExact` is a stub in SKY_LITE: 222 of 225 KB). Look: twilight
  blue-violet zenith, day sky slightly deeper blue, the sea's reflection follows. Cost (RTX 3070 Ti, 1920x1009): sky pass
  -0.3..+0.2 ms (the exact column is about as cheap as the sampled path).
  **The ground and the fog follow it (2026-10-26):** with the sky model on, `skyAmbientTerrain` is `cv2GroundSkyAt` (flash
  buffer `cv2GroundSky`, 64 x 16 vec4 after `cv2SkyPsi`, `kCv2GroundSkyOffset`; sunlut pass 2's COSINE-weighted upper
  hemisphere mean, every 4th column, rows 0-12 km) x 1/3.2, so the uses' x 0.4 x "Terrain sky light" (8) give the physical
  irradiance; the civil-twilight gate is off there (`skyAmbGate`). The old zenith x 3.2 matched it within ~4% at a 60-deg
  Sun (harness_runs/ms/gs1c: debug views 29 `skyambhemi`, 30 `skyambraw10`, both x2) but was ~1.5x too blue by day and
  olive at twilight. The land's direct Sun takes `sunPathTauExact` (ozone). The fog/dust march's sky light is the
  hemispheres' mean x `cv2.amb.y` (with `sky_light_model`), and the fog's multiple scattering the clouds' delta-Eddington
  field (with `ms_model`; far side = the fog below the sample, no finite-cloud leak). Cost -0.1..-0.3 ms. Not yet: dust's
  diffuse term (0.9) and ice fog's (0.3) are constants; SKY_LITE / SKY_ENV keep the zenith integral.
  **The ground under cloud (2026-10-26, `clouds_v2.ground_overcast` 1, slot 353, UBO `cv2.gnd.x`; needs the sky model).**
  Before, only the direct Sun was shadowed: under an overcast the ground took the CLEAR sky's light, blue and undimmed.
  The lightning pass's last 4 workgroups bake `cv2Vt` (cloud_lightning.glsl, flash buffer after `cv2GroundSky`,
  `kCv2VtOffset`): 5 nested 32 x 32 levels on the gnomonic plane at the eye's sea-level point, cells 1 / 4 / 16 / 64 / 256
  km, one level a frame; per cell 2 jittered vertical columns a visit (sea level to 14 km, 20 steps, field at cell / 8,
  >= 100 m, thin ice x 0.2), running mean 0.15 (restart on a stale level or a half-cell move). Stored as MEANS over the
  columns, not of tau: x the sky light's diffuse transmission 1 / (1 + 0.11 tau), y the Sun scattered down ((1/2 + 3/4 mu)
  x that, Eddington, less the direct beam), z tau. A mean tau treated scattered cumulus (tau ~25 over a third of a cell)
  as a uniform tau-8 sheet that turned the Sun diffuse: the ground between clouds +40%. `cv2VtAt` (finest level first,
  3-cell edge blends, clear past the last / when stale): `skyAmbientTerrain` = the sky's x x + sunSpecTint x 1.5 mu y (the
  direct term's units). Under a thick deck shadowed ground keeps ~its brightness but goes neutral grey (sky B/R ~1.5 ->
  ~1.3 on the ground, the diffuse Sun white). Cost +0.1..0.15 ms; still-view shimmer 0.07 -> 0.09 levels. Debug view 31
  `groundtau` (R log2(1 + tau) / 10, G sky transmission, B Sun scattered down). harness_runs/ms/ov3-ov5.
  **Clouds under a deck take it too (2026-10-26):** the bake also stores `cv2VtUp` (the same cells: the means of the sky
  light's diffuse transmission through the cloud ABOVE 1 / 2.5 / 5 / 9 km), and the march's new-model ambient
  (`cv2.amb.x`) takes the deck over each lit non-thin sample beyond its own column (`cv2VtUpCell` + `cv2VtUpH`, per sample:
  tau_deck = the map's equivalent tau over the sample's own cloud TOP, `f.topH`; 2026-10-27, issue 20261027_183553: it was
  the map's tau over the sample's height less its `tauUp`, two quantities of different kinds, and under an anvil the cumulus
  cores, tauUp past the map's, lost the deck and took the clear sky's blue while their edges took its diffuse sunlight: dark
  blue cores, bright white veins over the anvil's whole shadow; mottling high-pass 8.0 -> 4.1 levels, shadow B/R 1.10 -> 1.04,
  the cumulus under the cuair overcast darker and lumpier, march -0.8 ms there): both hemispheres x its diffuse transmission, plus the
  Sun it scatters down, 2 mu ((1/2 + 3/4 mu) Td - e^(-tau / mu)) x sunCol above and x the albedo below. The probe (250 m
  up) and the column estimate see only the sample's own cloud: cumulus under an overcast took the clear sky (navy, issue
  view cuair: -37.8, 145.0 from 3 km at 02:06 UTC) and now read neutral grey. Cost ~+0.4 ms of march from orbit
  (per-pass timers), ~0.3 near the ground; one lookup per batch or a per-ray cache cost the same (a group of rays runs it
  when any one needs it). 128 registers. harness_runs/ms/cup1-5.
- **Two perf skips (2026-10-26, reworked from the parked ee29203):** a FAINT cloud sample (sigma x the light march's length
  < `CV2_FAINT_OD` 0.2, i.e. sigma < ~8e-5 /m: haze, Sun-lit, no beam) takes no light march and no sky probe: its Sun shadow is the transmittance
  volume at the sample, and it gets NONE of the delta-Eddington field (`CV2_FAINT_MODE` 0): with tau toward the light 0
  it read as the lit face of a thick slab and haze under cumulus glowed beige (the column estimate as its far side). Mode 1
  (a 2-step march) saved nothing. And the sky pass skips shading a surface whose cloud transmittance in front is under
  `SKY_SURF_HIDDEN_T` 1e-3 (the sea's TAA flag kept). Interleaved (1920x1009): cloud march -0.3..-1.1 ms, sky up to
  -0.9 ms from orbit, images within noise. harness_runs/ms/faint1-2. The threshold was 0.02, then 0.2 (harness_runs/ms/fo1-2:
  base-vs-base floor at every view up to 1.0; 0.5 changed cumulus lobe rims seen from 3 km): from 6 km INSIDE the mid layer
  (Brisbane, -27.5 153.0 at 02:06 UTC, az 60 el -4; its haze was 25 of 34 ms) 33 -> 25.5 ms; matrix suite total -4.2 ms,
  every quality metric unchanged (harness_runs/mxc/on vs fo02).
- **The unified volumetric composite (2026-10-26, branch perf-frames; the user: the plumes, smoke and tornadoes "feel entirely
  distinct in quality" and clip against rain, dust, smoke and clouds).** The dust list's Gaussians (launch plumes, flames,
  wildfire smoke, tornado funnels, haboob far field, Kessler dust and fireball trails) are still composited in cloud_march.comp
  at half resolution, but THROUGH the cloud by depth: the v2 march writes each ray's cloud **opacity profile**
  (`cv2Profile`, march binding 22 / composite binding 26, `cv2ProfileBuf`, one uvec2 per 2x2 block of the cloud grid, sized
  with the cloud targets: first contact, half opacity, last contact as log2-metre 16-bit codes, + the cloud's transmittance;
  written before the fog / dust march, so fog is not in it). The composite scales it to the RESOLVED cloud's opacity, splits
  each Gaussian's optical depth and light over its four depth segments (`dustSegW`: three erfs, skipped where the ray has no
  cloud) and composites front to back: in each segment the cloud's light there (its share of the resolved radiance) dimmed by
  the plumes in front and half the segment's own, the plumes' light by the cloud in front (its opacity at the segment's
  middle); legacy cirrus / aurora behind it all. Emissive light is dimmed by the cloud in front of it the same way (it was
  "front if nearer than the mean distance", else the old 3rd-power suppression). The S9 derivatives follow (`gBB` = the
  plumes' transmission over the cloud's light, `gBA` + the plumes' / emissive light x their mean opacity share). No profile
  this frame: the mean distance x (0.85, 1, 1.15). It replaced ONE front / behind blend over +-(15% + 150 m) of the cloud's
  mean distance: a funnel 3 km away in a rain shaft was hidden whole (the rain's mean distance in front), a plume inside a
  cloud was wholly in front or behind.
  **Lit by the clouds** (plumes and smoke, not tornadoes: the CPU lights those through their storm's probed column): the Sun
  through `cv2StvT` at the Gaussian's lit point, the sky share through the deck above (`cv2VtUpCell` / `cv2VtUpH`) and the
  sunlight the deck scatters down (2 mu ((1/2 + 3/4 mu) Td - the direct), isotropic), below 16 km. Multiple scattering is
  per GAUSSIAN now (was of the whole stack along the ray, which the segments no longer sum). Not yet: the fog / dust march in
  the profile, the rain particles (post-TAA) against plumes, the plumes' sky light from the hemisphere tables (still the CPU's
  constant blue below 25 km) and their night x10 boost. Composite 128 registers. Cost (RTX 3070 Ti, user settings,
  interleaved x3): cloud bucket tornado / haboob / launch within noise .. +0.2 ms, a big wildfire's smoke +0.5 ms (each of
  the two lookups ~0.15). Scenes: `harness_runs/uv/` (u8 = the tornado in rain, `storms tornado dist_km=3 alt=60 el=12`).
- **AgX tonemap (2026-10-26, branch cloud-ms; sat_sky.frag `skyAgX`).** The sky pass tonemapped 1 - e^-x per channel
  ("Highlight roll-off" blends a longer shoulder): each channel saturates alone, so bright orange skews yellow and sunlit
  cloud flattens to paper white (where the physical multiple scattering puts it). AgX (Sobotka; Wrensch's polynomial
  fit): an inset gamut, a log2 curve over -12.5..+4 EV about mid grey (0.18 -> ~0.18, the exposure means what it did),
  then a look (power = contrast, saturation about the luminance) and display gamma back to linear for the sRGB
  swapchain. Not in SKY_LITE / SKY_ENV (`SKY_SUN_SHADOW` guard; the probes are HDR). Settings (Photometry > EXPOSURE AND
  MOON, slots 349-351, UBO `cv2.tone`): `clouds_v2.tonemap` 1 (0 = the old curve), `tonemap_saturation` 1.15,
  `tonemap_contrast` 1.15. With it the multiple scattering's physical level (`ms_diffuse_gain` 1.6) no longer clips lit
  cloud; a low Sun's disc stays a disc in its glow; a twilight band goes pinker (hue kept, not clipped to yellow).
  Post-tonemap terms (Milky Way, stars, the orbit grade) are unchanged.

- **User feedback round (2026-09-29, after the sprint):**
  **Lightning** toned down (glow x2, bolts x60 in `lightningCS`; were x5 / x150). **Bolts (review 4):** a
  FRACTAL tree built in one sequential pass per pixel (the main channel's random walk carried along, not
  re-summed per vertex): a branch from ~60% of the main vertices (6 segments, angling down and out,
  shrinking with height lost) and twigs off ~55% of branch vertices (4 segments); each segment
  (`boltSeg`) draws a core whose PEAK is capped (so an overexposed stroke keeps a pixel-ish width instead
  of swelling into round-capped tubes whose radius jumped along the channel) plus a faint halo, dimmed
  SMOOTHLY where it passes behind the cloud; a bounding test skips rays far from the tree. Default glow
  0.088 (the user's: at 1 the flash's cloud glow drowned the bolt). Cost ~0. **Red sprites** (easter
  egg, "Lightning sprites (chance)" `lightning_sprites` 0.05, slot 191, `misc2.x`): a ground stroke may
  set off a sprite ~75 km up (flash list kind 2, `spriteCS` in cloud_march.comp: a deep-red head,
  8-13 tendrils to ~40-50 km, each forking twice as it falls and spreading out (`spriteSeg`), the tips
  leaning violet, ~0.1 s; review 3 made it darker and branched — 5-8 unforked tendrils read as a comb); thunder and the harness skip/label it.
  **Cb columns**: "Cb fill" (`cb_fill` 0.5, slot 189, `column3.w`) — the share of lattice cells holding a
  tower at all (the user's sparsity ~0 put one in every cell: an even grid of columns); centres jitter
  over 0.15-0.85 of a cell (was 0.25-0.75), so the 2x2 search is exact to 0.65 cell and the 3x3 to 1.15
  (`limM` 0.62 / 1.12, cell >= reach / 1.12) — this MOVES every tower again; "Cb tower radius" reaches
  20 km. `cloud_v2_lightning.comp` applies the same fill and jitter. "Storm cumulus variation"
  (`cb_cumulus_variation` 0.3, slot 190, `column3.z`): the storm cumulus top varies +- that fraction by
  region (the cluster Perlin). **Motion**: WASD moves ~0.08 rad/s of arc (~510 km/s, boost ~3200 km/s),
  i.e. ~17 km a frame, so the history is useless and a frame is one march — whose IGN start jitter lined
  neighbouring rays' steps up into corduroy bands ("clouds lose all form"; a 550 m/s harness path shows
  nothing wrong). When the eye moves > 150 m in a frame (`misc2.z`) the march jitters from a 64x64
  void-and-cluster BLUE-NOISE tile (`makeBlueNoise64`, built at init, march binding 15, SSBO) + a golden
  step per frame, and the resolve blends toward a 3x3 tent of this frame's samples as the parallax passes
  6-24 px (or with no valid history). White noise was tried first: clumpier. Harness:
  harness_runs/motion2 (fastG = before, fastG4 = after, paths at the real walking speed).
- **Adaptive rate / dynamic sampling (2026-09-29, `cv2AdaptiveRate`, "Adaptive rate while moving (0/1)",
  key `adaptive_rate`, slot 192; default OFF since 2026-10-20 (the compiled defaults = High / Ultra; Low / Medium /
  Planetarium / Potato set it on, `applyGraphicsPreset`); "Adaptive parallax (px)" `adaptive_parallax_px` 1.6, slot 193,
  `misc2.w`; rate `misc.z` = 3):** where the march used to go full rate because the view moved, it runs the
  sparse grid (pass A, `cloud_v2_march.comp`) and marches at full rate only the 32x32 half-res tiles whose
  clouds show more than that parallax a frame (|eye delta| + the volumes' slide over last frame's resolved
  depth, march binding 16) or whose history comes from off screen (a pan's uncovered edge). A classify pass
  (`cloud_v2_tiles.comp`, one 16x16 workgroup per 32x32 tile, same set) lists them (`TileBuf`, binding 19:
  indirect args + tile list + flags, reset by fill/update each frame) and pass A returns on a listed tile; pass B (`cv2MarchPassBPipeline`, the same shader with
  spec constant 2 = 1, `vkCmdDispatchIndirect`, 4 workgroups per tile) marches them into `cv2FullImg` /
  `cv2FullDepthImg` (bindings 17/18) AND writes their sparse-grid samples, so the quarter grid stays
  complete; the resolve (bindings 6-8) takes a listed tile's pixels as fresh (`tileFull`, `freshAt`).
  Why tiles and two passes: skipping pixels inside a full-rate dispatch saves nothing (a warp costs its
  slowest lane). Not used without history, in fast flight (> 150 m a frame: full rate + blue noise) or
  with a workgroup size other than 16x16. **The vote was first inside the march, and it (together with a
  one-line `dbg != 11` exemption) flipped the march to ~224 registers: +19% at full rate** — it is its own
  pass for that reason, and debug view 11 reaches only the resolve (`fog2.z`; the march sees view 0). The
  off-screen test has NO margin: a pan uncovers ~2 half-res px a frame, and a 1% margin (~10 px) meant
  no edge tile ever voted. Measured at 128 registers (harness `path play` reports `gpu_ms_mean`): storm
  views flying / panning 20.2 -> 9.5 ms, 22.5 -> 7.9-10.6; light views ~equal — there the frame is set by
  the longest (horizon) rays, so sparse and full cost about the same (a still anvil view: 6.0 vs 6.2 ms).
  Images match full rate (mean |diff| < 1/255, <= 0.2% of pixels past 12, no tile seams at 8x). At 550 m/s
  nothing passes 1 px (the deck below is ~0.5-1 px). Debug view 11 tints the full-rate tiles red. Harness
  `state` reports the rate (`clouds_v2.rate`: sparse / full / half / adaptive).
- **Fog/dust/ice fog review (2026-09-29, the user's snapshots):** (review 2: the variation is GENTLE —
  regions 0.3-1 at ~500-1500 km, plumes 0.35-1 with a wide threshold; hard-edged regions x plumes read
  as distinct blobs; the ice fog's Antarctic term needs land, it covered the Southern Ocean) dust is patchy by REGION
  (`cv2DirNoise`, an analytic value noise on the direction at ~300-1000 km: a dust event covers a region,
  the next stays clear; the whole dry continent sat at one level) with a lower plume floor, and fades
  seen from above (x0.08 by 60 km of eye altitude since 2026-10-06, was x0.25 by 80 km: from orbit at night its
  city-lit glow drew soft tan blobs round desert and Mediterranean cities; mostly terrestrial). Its exponential height profile
  (and the ice fog's) is integrated EXACTLY over each march step (a sample at one height per step drew
  the step pattern as rings on the ground from 70 km), steps crowd at the far (ground) end when the eye
  is above the band, and the 150-km cap keeps the ground end from above. **Ice fog / diamond dust**
  ("Ice fog amount" `ice_fog_amount` 1, slot 204, `fog2.w`; "Ice fog density (1/m)" `ice_fog_density`
  0.0002, slot 205, `sunE.w`): over Antarctica, Greenland's high interior and the high Arctic's land
  under a clear sky, scale height 250 m, patchy; lit with the ice optics at a halo-rich habit (22/46
  degree halos, sundogs, parhelic circle, CZA) plus a sun pillar (`cv2PillarOptics`), Sun or Moon.
  Since 2026-10-30 (issue_20261030_204305) the ice mask is also multiplied by its own large-scale regions
  (`cv2DirNoise` at 6.64 and 10.86, x0.12..1, ~590-960 km): the ice fog is no longer one uniform film
  from orbit (the dust path has had regions since 2026-09-29; the ice branch did not). No eye-altitude
  fade, unlike the dust's x0.08 by 60 km: the effect is wanted at altitude; an open-ocean exclusion would
  need a sea-ice / shelf mask (the `1 - smoothstep(-80, -72, lat)` fallback exists because shelves read
  the sea's 0 m in the DEM).
  Knockout 2048 switches it off with the fog. Harness: observer lat=-78, `camera look sun`.
- **History weight moving** ("History weight moving" `history_weight_moving` 0.7, slot 203,
  `motion.y`): the resolve's new-sample weight once the view has parallax (from a quarter half-res
  pixel), with a variance clamp (mean +- 1.25 sd) in motion. "History weight" (0.05) stays the still
  view's: at 1 each pixel alternated between its own ray and its neighbours' estimate (a checkerboard
  flicker), and the fixed 0.35 ceiling in motion let small clouds ghost (user snapshot 4). How it moves
  is the user's to judge; a harness path over 150 m a frame is the fast-flight mode, not this.
  **The still weight is capped at 0.15** (slider and settings load, review 2): at 1.0 the user's clouds
  were a woven checker and deck tops showed "wood grain" (snapshots 4, 6); 0.3 still weaves visibly.
- **Fog + dust (2026-09-29, replaced v1's `fogMarchCS`, a global 1.4 km noise haze):** `cv2FogDust`
  (clouds_v2.glsl), driven by the weather cube. FOG over the SMOOTHED ground (`cv2Ground`, ~15 km), so
  valleys fill deeper and ridges stand out: radiation fog on clear-to-broken nights in valleys (ground
  below its ~80 km mean) and on low ground by the sea, burning off by ~12 deg of Sun; sea / advection fog
  on the coast and at sea under a stratiform regime, day or night; light mist in rain; patchy on the
  cluster field; its top undulates with the low shape noise (a flat sheet read as a painted plane). DUST
  over dry land (the map clear over ~80 km, not sea, not rain) in plumes on the coarse cluster field,
  falling off with height; tan albedo, forward phase, a diffuse multiple-scattering term (without it a
  dusty sky read as a grey veil). **Not part of `cv2Field`:** any addition to the main march loop — even an
  unused `CV2Field::dust` member — flipped the march to ~224 registers. They get a 20-step march of their
  own AFTER the clouds' (u^2 spacing through their band, cut at the scene depth and 150 km), lit cheaply
  (the key light at the band's middle x the map's cover overhead x the path to the fog's top; sky zenith;
  Moon and city light at night), composited by distance around the clouds' mean depth. Cost ~0 at the
  benchmark views; from above the band it is skipped where the cloud's T < 0.01 (2026-10-04: its 20 steps under
  opaque cloud were ~3 ms of an orbit storm view, user snap 1; image within run-to-run noise). Settings (section "Fog & dust", slots 194-199): `fog_amount` 0.6, `fog_depth_m` 250,
  `fog_density` 0.012, `dust_amount` 0.5, `dust_height_m` 1500, `dust_density` 0.00025. Knockout bit 2048
  (was "fog layer"; the Low / Planetarium / Potato presets set it) switches both off. Harness scenes:
  harness_runs/fogdust (Po valley at sunrise from 2.5 km is the reference look). Not yet: fog in the light
  march / ground shadow / beams, wind-driven dust storms (needs a wind field).
- **Light volume + godrays (2026-09-29; DELETED 2026-10-13 — `cloud_v2_lightvol.comp`, its image and pass are gone, march bindings 13/14 are the Sun / sky colour table; kept here for the reasoning):** `cloud_v2_lightvol.comp` bakes a
  camera-centred 128 x 128 x 32 R16F volume of sun transmittance (march set bindings 13 storage / 14
  sampled; +-"God ray range" `godray_range_km` 400 about the eye, sea level to 16 km, gnomonic columns; 4 of
  its 32 levels per frame, each voxel 16 growing steps toward the Sun, ~127 km) — the plan's phase-3 light
  volume, so far feeding only the godrays. "God rays" (`godrays`, 0 = off and no bake, slot 187; range slot
  188): for SKY pixels the march scales its output transmittance by the share of the ray's single-scattered
  airlight that lies in cloud shadow (16 samples + a 4-sample tail to the atmosphere's exit), so the
  composite dims the sky by it. Two first cuts failed: SUBTRACTING the shadowed airlight (negative radiance
  from the march's air model, which does not match the sky pass's) overshot to black under an anvil at
  sunset; scaling T on surface pixels darkened the sea, which already has its own cloud shadow. As it
  stands the effect is subtle (a low Sun's airlight comes mostly from beyond the volume) and unjudged —
  real shafts need a dedicated jittered sub-march over the volume's reach — NOT the sky pass's N_VIEW
  loop, whose ~10 km steps (100 km / viewSamplesMin) are far coarser than a shaft, and which would take
  `sat_sky.frag`'s 16th and last sampled-image slot besides. Ground shadows and beam shadowing
  could read the same volume later.
- **Soft ground contact (pass 10):** the march fades extinction over the last max(40 m, 3 pixel
  footprints) before the half-res scene depth: dense cloud meeting a slope ended on a hard,
  stair-stepped line.
- **Rain-only samples stand for four coarse steps** (2026-10-04, `rK`): each runs a light march whose steps evaluate
  the whole field (towers included). Under a snowing storm at night (user snapshot, Dushanbe, 2 km) the cloud march went
  40.6 -> 27.2 ms; in review 22's daytime storm 8.3 -> 6.8, a shaft's edge ~3 levels brighter. Tried: skipping the
  rain light march (-20 ms, but sunlight would reach shafts under thick cloud again), reusing it over 1.5-4 km of rain
  (222 registers).
- **Rain + optics** (2026-09-28, `.plans/ATMOS_OPTICS_AND_STORMS.md`): rain shafts are cv2FieldLow's
  below-base branch (`CV2Field.rain`; the shell floor is 0 m while Rain > 0); rain at the eye drives a
  streak overlay in cloud_march.comp (after the resolve, so it animates). Halos / sundogs / parhelic
  circle / circumzenithal arc (`cv2IceOptics`) and the bows (`cv2RainOptics`) are Snell's-law positions
  per colour channel added to the single-scatter phase of ice (`thin`) and rain samples, per RAY in the
  march, for the Sun or the Moon. Debug views 5 (rain optical depth) and 6 (rain at the eye, streak
  mask). Ice optics are gated per REGION by crystal habit (`cv2IceHabit`, evaluated at a ray's first ice
  sample): a 22 deg halo in ~1/3 of cirrus, sundogs less, the CZA/parhelic circle rarely, 46 deg very rarely.
  The sun path to every cloud sample is the Chapman column (`sunTransmit`, include/atmosphere.glsl) plus
  ozone: a 12-step midpoint rule along a grazing path misses the tangent point's dense air.
- **Tonemap controls** (Photometry tab, EXPOSURE AND MOON section since 2026-10-06 — they were deep in the Clouds
  tab's lighting section; slots 158-160/163 and the keys unchanged; `clouds_v2.exposure_ev / highlight_rolloff /
  white_balance / auto_exposure`, UBO `exposureScale / highlightRolloff / whiteBalance`, the old cloudsV2
  pads): **one GLOBAL exposure** (`globalExposureEV()` = Exposure (EV) + the metered auto offset) scales the
  sky's radiance, the point sources (the point style's ref/limit magnitudes shift by `exposureGainMag()`,
  like the zoom gain) and the Milky Way / zodiacal light; `skyExposure()` mirrors it. **Auto exposure**
  meters the displayed frame (`recordScreenshotCopy`: the central 60% blitted to 64x36, read next frame in
  `readExposureMeter`), steps toward a linear mean of 0.32 and harder when > 4% clips, only DARKENS (to
  -3 EV x "Auto exposure (day)") and only by day. Harness `state` reports it under `exposure`. Highlight
  roll-off blends the tonemap toward 1 - 1/(1 + x + x^2/2) (same toe, long shoulder); White balance adapts
  to the sunlight's colour at the observer (a low Sun is yellow; without it every cloud read beige).
- **The Earth from space vs the Artemis II photographs (2026-09-30, `harness_runs/artemis_ref/`, Wikimedia
  Commons JPGs).** Measured: in the photos sunlit cloud is ~10x the open sea in linear light; the sim showed
  ~2.5x (a sky-cyan sea under a milky veil). The raw sea surface was right (debug view 43: navy); the cause
  was exposure + tonemap. (1) **Auto exposure spot-meters the lit Earth from altitude** (`readExposureMeter`,
  100 -> 1500 km ramp): the mean over pixels brighter than 0.08 (the whole-frame mean counted black space, so
  a small bright Earth never darkened) toward 0.42 (the photos' lit-Earth mean in the meter's units), clip
  counted against the lit pixels, down to -5 EV, gated by lit Earth in view instead of the Sun at the
  observer's nadir (the night-side crescent blew out). (2) **Orbit colour grade** after the tonemap
  (`sat_sky.frag`, `cloud.taaJitter.z` = "Orbit colour grade" x a 30 -> 300 km ramp, slot 219, key
  `clouds.orbit_grade`): y = 1.16 x^2.2 display-linear (fitted to the photos' values) + 30% desaturation.
  **Only on the sunlit Earth** (review 11): weighted per pixel by the Sun at the surface point (the tangent point for
  a ray that misses), over -6 .. +3 deg; on the night side its x^2.2 crushed city light, moonlit cloud, airglow and
  aurora ~10x (the night side from orbit read nearly black).
  The Mie and Rayleigh gains had NO effect on the veil from 70,000 km — do not chase it there.
- **Rainbows are done** (user-approved 2026-09-28): they show at storms on the terminator (low Sun behind
  the observer); a harness run that does not frame one is a location problem, not a render one.
- **Target A's alpha is a signed distance in KILOMETRES** (`include/cloud_occlusion.glsl`): >= 0 opaque
  cloud, < 0 the mean distance of translucent cloud. In metres an RGBA16F alpha was +inf past 65.5 km, so
  from orbit clouds dimmed every satellite in front of them. Point sources use `cloudPointVisibility()`.
  **A texel with no cloud but a plume / emissive light carries that light's distance, FLAGGED** (2026-10-20): the
  half float's lowest mantissa bit (`cloudAlphaFlag` / `cloudAlphaIsEmit`, cloud_occlusion.glsl; cloud_march.comp
  clears it on every real cloud distance, 1/1024 of the distance). Gathered texels are exact, so the bit survives;
  a filtered read is not a flag. Anything treating the alpha as "a cloud is there" must skip flagged texels (the
  engine-light and lightning passes in lightning.frag do).
- `sat_sky.frag` upsamples the half-res clouds joint-bilaterally where a surface edge crosses the half-res
  grid (the ridge/cloud stair-steps).
- **Passes** (inside the `cloud_march` timestamp bucket, `recordCloudsV2`): `cloud_v2_march.comp`
  marches one pixel per 2x2 half-res block per frame; `cloud_v2_resolve.comp` reprojects history
  (exact: rotation about a known eye + the eye's ECEF delta), clamps, and blends at `look2.w` (0.1)
  rising to 0.35 with reprojection motion; the clamp box WIDENS for a still view (a tight box against
  the quarter-grid neighbours made blocky 2x2 squares in far cloud); the result is copied to history
  and cloud_march.comp composites it. **Full rate from altitude** (`misc.z`, `cv2FullRateNow`, above
  `full_rate_above_km` = 30): every half-res pixel is marched every frame — from orbit the sparse
  march's history is invalidated by motion and the fallback was a 1/8-resolution upsample (the
  "pixelated mess" from space). The march targets are half-res sized for this; sparse frames fill
  their quarter-size top-left. Riding the ISS at 1x is clean (harness_runs/cloud_v2_o). sat_sky.frag skips flat layer 0 (unless the march is knocked out, bit 32768) and the
  3x3 cloud blur.
- **Jitter per VISIT, not per frame**: a sparse pixel is marched every 4th frame, and 4 x 0.618
  stepped its golden-ratio offset by ~0.47 per visit, so each pixel of a 2x2 block kept its own two
  clusters of offsets and its own bias — far clouds (long steps) showed 2x2 squares.
- **The march**: steps grow with distance FROM THE SHELL ENTRY (and the max distance is measured from
  there — from orbit the Earth is further than the limit); the switch from coarse to fine steps lands
  on a jittered lattice (without it a flat deck top drew contour rings around the nadir). When the back-up
  to the fine lattice passes the segment start it restarts JITTERED there: clamped to the start itself,
  every ray from inside a cloud or rain stepped one lattice from the eye (contour rings of the cloud base
  looking up through rain, review 3). A ray that runs out of budget (`misc.x`,
  presets 160-640) is FILLED with the path's mean extinction and the cloud's mean colour, not left
  transparent (that was the horizon "seeing through" the clouds).
- **Key light**: the Sun, or at night the Moon through the same light march (`misc.y` x `moonGain`);
  sky ambient is occluded by the cloud above and by the sample's own density (detail on shaded sides).
  **Night (review 5):** the ambient carries the terrain's moonless "Night sky light" (`terrainErosion.z`,
  x 0.2 onto the moonlit-sky scale), ungated like the terrain's; and `sat_sky.frag` keeps the city glow's
  in-scatter IN FRONT of the clouds (`accumCityFront`, up to the cloud's distance from `cloudA.a`) out of
  the cloud's attenuation: `color * A + B + cityGlowFront * (1 - A)`. The composite multiplied the whole
  night sky by the cloud's transmittance, and the march's airlight has no city glow, so the clouds along a
  night horizon were a black, stair-edged band darker than the ground under them (snapshot 6: band 24 ->
  38, ground 37). **Review 6:** the split distance is the NEAREST cloud of the four half-res texels
  (`textureGather` of the alpha: the filtered alpha blended a cloud's distance with the no-cloud -60000 km
  and changed every frame — a black flicker on night cloud edges), and the term fades in over 40-150 km
  of cloud distance (a near cloud's resolved distance is noisy and moved the split through the dense low
  air; from above most of the glow is below it anyway). Still-frame flicker 1.2% -> 0.27% of pixels
  (0.19% without the term).
- **Review 7:** **Shadowed air is sky-lit.** Air in the Earth's shadow gets isotropic in-scatter of
  half the zenith sky's radiance (faded as the clouds' sky ambient, `skyDusk`), in the march's airlight
  (shadowed steps, `sR`/`sM`) and in sat_sky.frag's loop (`shR`/`shM`, `skyZenithSky` at the shadowed
  stretch's mean point), off from orbit (x (1 - terminator gate)). Single scattering left it black:
  facing away from a just-set Sun, the far clouds along the horizon were a near-black band (user
  snapshot: 18 against ~70 for the clear horizon; now ~40, the clear horizon 65 -> 84). What remains is
  real: those clouds hide the still-sunlit air beyond them. Day views unchanged (mean |diff| 0.05).
  **Motion grain:** the march's blue-noise jitter now switches on at 1 m of eye motion a frame (was
  150 m): with a moving history weight of 0.7-1.0, IGN's regular structure showed as a dotted
  honeycomb on lit tops during a slow strafe. The resolve, as the moving weight rises, gives the
  refreshed sparse pixel the smoothing its three interpolated neighbours have (half its four axial
  neighbours) and the same weight (`mk` also from `wNew`); full-rate pixels lean toward their 3x3 tent
  by 0.4 x mk x w. At "History weight moving" 1 in sparse tiles the image is still single-frame rays:
  grain, no longer a checker.
- **Review 8/8b:** **The air in front of every cloud is the sky pass's** (`airFront` in sat_sky.frag,
  accumulated in its atmosphere loop up to `tAirFrontM` — the four half-res texels' distances weighted
  by bilinear share x opacity, each step by its covered fraction — and composited x (1 - A)); the march
  outputs `attn * L` only. Its own 8-step airlight read far darker than the sky's at a grazing Sun
  (horizon clouds were dark silhouettes 100-600 km out), and a distance split between the two (review 8)
  disagreed at cloud edges: dark 2x2 specks and flicker along silhouettes. **Blue-noise jitter always**
  (per visit): the history's residual of IGN was a fine crosshatch over still clouds. **"History weight
  moving" <= 0.5** (default 0.4; load clamps): at 1.0 motion showed the quarter grid's raw rays.
  **Low bases over plateaus:** the regional ground (weather alpha mip 4, ~80 km, never above the local)
  lifts in full, only the relief above it at 0.6-0.9 — over Tibet the decks sat inside the plateau (a
  clear disc with a ring rim from orbit). **Ground shadow** (`cloudGroundShadowV2`): the field without
  the far-field substitution (`gCv2NoFar`), rain curtains at 15%, read footprint <= 3 km (coarser, the
  weather mips drew straight-edged squares), and past the terminator it marches along the horizon (the
  sun ray into the Earth met no cloud: a hard line along the ground terminator). The far field has a
  16-km octave. Harness note: with `auto_exposure` on, frames after `wait settle` still drift in
  brightness — set `clouds_v2.auto_exposure 0` before measuring flicker.
- **Review 9:** the air split's distance ignores texels with a 0 distance (resolve-blended edges) and
  falls back to the nearest real cloud (not the filtered alpha); the resolve clamps history in MEAN-COLOUR
  space (rgb / opacity, `cv2ToMean`); cloud_march.comp replaces an isolated cloud texel by its 3x3 tent
  and leans edge texels 60% on it; a thin (cirrus) sample stands for two steps only past a 60 m pixel.
  The flickering specks along cloud edges seen from 9 km at 62 N were LOW cloud glimpsed through gaps at
  the edges of the high layer (which the eye is inside there), in its shadow: reduced ~40%, not gone
  (high layer off removes them). **Ground shadow:** a 2x2 ORDERED start offset (was white noise: grain)
  with sat_sky.frag's shadow blur a [1 2 1] tent at 1 texel, which averages a period-2 pattern exactly.
- **Review 10 (2026-09-30):** a texel with cloud but NO distance (the resolve blended history's cloud into a
  texel whose own sample found none) takes its neighbours' opacity-weighted distance (cloud_march.comp);
  an OPAQUE texel stores max(tHalf, mean distance) — along a grazing path through a cirrus veil the half
  point landed IN the veil (~20 km) in front of a cumulus at ~55 km, and the sky pass split the air there
  (every point source is 100+ km away, so their occlusion is unchanged). The cirrus shadow on lower cloud is
  delta-scaled harder in the light march (`1 - 0.8 thin`, g ~0.8: real cirrus barely dims what is under it),
  and seen from INSIDE the high layer (view footprint 4-20 m) the fibres give way to the layer's mean haze.
  Specks along cumulus edges seen from inside cirrus (user snapshot, 9 km at 62 N) are reduced, not gone:
  debug views 46-49 (harness `debugview cloudairsplit|cloudtrans|cloudrad|cloudalpha`) show where the air
  split steps. Tried and reverted: w^2-weighted mean distance, a dark-outlier despeckle in cloud_march.comp
  (both global, neither measurably helped here).
- **Review 12 (2026-09-30):** **the resolved DEPTH has history** (`cloud_v2_resolve.comp`, binding 5 is
  read back: a still view, motionPx < 0.5, blends 0.1 of the new distance): a sparse block took its one
  sample's distance, so the sky pass's air split (`tAirFrontM`) moved on the 4-frame cycle and the air in
  front of the clouds flickered — from orbit, near the limb, while still (std > 5: 2.6% -> 0.13% of pixels).
  The resolve's motion also has an EYE-PARALLAX floor (the eye's move across the ray at the pixel's depth,
  capped at 50 km): moving up/down, a near beam shaft in front of far cloud ghosted. Point sources test
  cloud per TEXEL (`cloudPointVisibilityAt`, four gathered texels weighted bilinearly): the filtered alpha
  mixed a cloud distance with the no-cloud sentinel and dimmed the AI ring's satellites at cloud outlines.
  Aurora from orbit: the dark-sky gate opens over 40-100 km of eye altitude and the extinction starts at the
  shell's inner sphere (looking down, the path to infinity ran into the ground). City night: arterials
  continue to a 150-m footprint (was 10) with their share fading 4-150 m (the street lines stopped in a ring).
  Intro: vantage 300 m SSW (the eroded ridge filled the right of frame); Q/E starts from the ground
  (an offset left below it, 0 after the intro, had to be climbed out of invisibly).
- **Review 13 (2026-09-30):** **opaque cloud texels store the MEAN distance** (cloud_march.comp `occKm`; the
  half-opacity point only when there is no mean). The sky pass splits the air at that distance, and opaque
  texels used max(tHalf, mean) while translucent ones used the mean: tens of km apart along a grazing
  horizon, so the bright air in front of far clouds stepped wherever a texel's opaque/translucent class
  flipped — hard 2-4 texel blocks at any resolution (debug views cloudairsplit / cloudalpha showed it;
  transmittance and radiance were smooth). The mean is the transmittance-weighted origin of the cloud's
  light, i.e. the right split. Also: the march's ray moves within its texel each visit (an R2 sequence,
  not in fast flight) so the history integrates the texel's footprint.
- **Review 14 (2026-09-30):** **no dust or ice fog under the ground** in the fog march (`pp.h < cv2Ground - 30`;
  their height terms come back clamped at 0, so a sample below the surface read the densest value). Past the
  detailed terrain march's range the scene depth lands below the real surface, and from 180 km over the Ronne
  ice shelf the ice fog whitened everything beyond a hard ring ~550 km out (user snapshot 3; the radiation fog
  is exempt — it pools below the smoothed ground in valleys). With no surface at all the march ends at the
  sea-level sphere (it ran to the band's exit on the far side of the Earth). The Antarctic ice fog's land gate
  counts everything south of ~78 S as ice and tests the regional (~80 km) ground elsewhere (the shelves read
  ~0 m and were "sea").
- **Review 15 (2026-09-30, temporal stability):** **the resolved DEPTH's history is gated on the texel's whole
  shift, rotation included** (`shiftPx`): review 12 gated it on the parallax alone, which a pure turn leaves at 0, so
  while the camera PITCHED each texel took 90% of the distance of another direction (sky above the horizon, a cloud
  300 km out below it) and the sky pass split the air there — horizontal streaks and dark shapes along the horizon
  (user screenshot; yaw pans hid it, the horizon's structure being horizontal). On the sparse grid fast motion leans
  on the bilinear estimate, not the block's one sample (`blur` was `nc`: 4x4-pixel squares). Measured with
  `tools/harness/tstab.py` (docs/HARNESS.md "Temporal stability": player-speed paths vs settled references; pitch
  mean |diff| 5.4 -> 1.3, > 16 levels 10.9% -> 0.2%); the harness's `path play record=` had rendered ~12 frames per
  recorded frame (it waited on the PNG encode), which is why no earlier motion test saw the ghosting. Lowering
  "History weight moving" changed nothing measurable; the adaptive rate below 30 km (full_rate_above_km 0) cut the
  boost error 1.5 -> 1.0 at ~3x the cloud cost (8 -> 24 ms): not adopted.
- **Review 16 (2026-09-30):** **the light march has its own jitter** (another cell of the blue-noise tile, its own
  golden step): it took the VIEW march's, so a sample's depth inside a flat cloud top and its light steps' positions
  moved together and their product never averaged out — contour rings round the nadir on the Cb heads' lids from
  16 km (4 light steps over 2.5 km; light steps 8 hid them, the view step's size moved them, the storm noise did not).
  The heads still SHIMMER at a grazing Sun (two settles of one view differ by 7.4 levels at 4 light steps, 5.3 at 6,
  4.5 at 8, 0.8 with the towers off; the light LOD and lightning are not involved): each long light step lands in the
  lit skin under the top or above it. Medium's light steps are 6 (were 4). Tried: lighting heads by the path to their
  top — no effect at a grazing Sun, reverted. A variance-adaptive still history weight (x0.4 / x0.25 where the 3x3
  new samples are noisy) measured within run-to-run noise (4.85 / 5.29 vs 5.3): reverted.
- **Review 17 (2026-10-01): the "blocky far reflections" at sunset were the CLOUD SHADOW on the sea** (debug view 50,
  `oceanshadow`; knockout 256 removed them): (1) `cloudGroundShadowV2`'s dense stretch ended at 3.5 km / max(sun up,
  0.05), ignoring the Earth's curvature — ~70 km out and ~400 m up near sunset — so a deck at 1-2 km fell to the upper
  stretch's ~30 km steps and was hit or missed per texel offset (rows of unshadowed texels: a stair-stepped bright band);
  now the curved-ray distance to 3.5 km. (2) A low Sun's ~100-km shadow path made any coverage over the clear threshold
  full shadow, i.e. a binary map of the source JPEG's 8x8 blocks: the shadow reads the weather map coarser as the Sun
  drops (`gCv2WxLod`, up to mip 3, C1; written only by the shadow, so the march folds it away — 128 registers).
- **Cloud morphology (review 17, 2026-10-01; `tools/make_cloud_morph.py` -> `assets/textures/cloud_morph.rgba8`,
  march binding 20, `CV2_MORPH_BINDING`, UBO `anchorMorph`/`morph`, sliders "Morphology (orbit)" 0.8 / "(near)" 0.5 /
  "period (km)" 320, slots 234-236):** four tiling channels made from scratch (FFT noise, Voronoi on a torus),
  rank-equalised to uniform so a threshold at 1 - cov covers cov: R closed cells, G open cells, B streets, A clustered
  cumulus. `cv2MorphZ` picks them by type (decks closed; convective over the sea poleward of ~30 deg open; streets in
  some regions; cumulus fields elsewhere) through a triplanar cube projection. Far field: the morphology replaces the
  Perlin share and the map sets the cloud FRACTION over ~20 km (the early-out's mip-2 read `wCoarse`, half with the
  5-km cov; decks capped at 0.9 so cell rims open) — thresholded on the 5-km texels it moved 3.5% of an orbit view's
  pixels. The mid layer's regime takes the closed cells too (one read). Only the view march binds it (the shadow,
  beams, lightning, light volume keep the Perlin). **A second weather read for the fraction flipped the march to 228
  registers** — reuse `wCoarse`. From orbit much of what shows is the mid and high layers stacked over the low: the
  low layer's cells show (faint honeycomb, `mid_amount 0 high_amount 0`); the map's own hole shapes still dominate.
  Open: the high layer (cirrus regime) is still Perlin; the morphology is not wind-aligned (streets follow the cube face).
  **Benchmark** (`tools/cloud_stats.py`: cloud mask by brightness - saturation, Otsu; perimeter-area dimension D, object
  size exponent b, the MASK's spectral slope, holes per 1000 km^2). Eight public-domain MODIS scenes (Wikimedia Commons:
  open/closed cells, streets, popcorn cumulus, a frontal band; harness_runs/cloudref, not committed): D 1.48-1.72, mask
  slope 1.5-2.3, 3-16 holes / 1000 km^2, thousands of objects. Ours, nadir from 420 km at five broken-cloud places
  (harness_runs/fb24/nadir.satcmd): D 1.31-1.36, slope 2.5-2.7, 0.5-1.4 holes — the outlines are the 5-km map's
  coverage thresholded; the morphology moves every number the right way but little (holes +20-50%, objects +25-40%).
  Tried: the ~20-km fraction alone (inflated the coverage: fewer holes), a flatter threshold (x0.55: 231 registers
  for +15% holes). **Compare at one ground resolution** (`--scale`: the renders' clouds are half-res, the MODIS thumbnails
  sharp; at full res every render reads D ~1.33 whatever the layers): at ~1 km/px real D 1.47-1.73, slope 1.5-2.35,
  holes 1.9-9.8; ours (morph on) 1.37-1.42, 2.27-2.41, 0.6-1.3. Isolating layers changed nothing (low only = all);
  baking the coverage FRACTION into the weather cube and taking the far field's fraction from ~20 km alone (local branch
  `coverage_frac`, unmerged) changed nothing either. The remaining gap is 1-5 km structure, which the half-res march
  cannot resolve from orbit (the morphology's fine term is filtered to its mean there). The mid layer's sub-pixel haze
  takes the closed cells at its regime's area fraction (`uMid`).
- **Review 18 (2026-10-01): MEO flicker, imagery morphology, true coarse coverage.** (1) **Flicker** of scattered
  puffs from medium orbit: each visit's ray lands at another point of its 5-20 km texel (the R2 sub-texel jitter),
  and the far field's 2D reads were filtered to the footprint itself, so a puff near the threshold was hit or missed
  per visit. The far field reads at twice the footprint (`fpF`), stretched by 1/|dir.up| at grazing views
  (`gCv2Stretch`, set by the view march only), and the still history weight falls to 40% from 300 to 3000 km
  (CPU, `look2.w`). 8000 km: pixels with std > 8 levels 1.0% -> 0.31% (`tools/harness/flicker.py`,
  harness_runs/meo). The sparse vs full rate and the step size made no difference (sub-texel sampling, not steps).
  (2) **`cloud_morph.rgba8` is real imagery** (`tools/make_cloud_morph.py`; the procedural set is
  `make_cloud_morph_procedural.py`): four MODIS scenes from NASA GIBS (closed cells off Peru, open cells in the
  North Atlantic, streets off Japan, popcorn cumulus over the Amazon), cloud = the darkest linear channel minus
  the clear background (a local opening for the land scene), clear pixels ordered by blurred distance to cloud,
  Moisan's periodic component (seamless), locally standardised over ~30 km, clear stretches filled from another
  scene, rank-equalised. Credited in THIRD_PARTY_NOTICES.txt and the Attributions tab. (3) **The weather cube's
  coarse mips hold the brightness whose coverage is the TRUE mean coverage** of the fine texels
  (`cloud_v2_weather.comp`, N x N taps, `remap` push constants; mip 0, the types and precipitation unchanged;
  Coverage / clear / full are in the bake's settings hash). (4) **Far field:** the fraction 85% from mip 2
  (`wCoarse`, was 25%), the 5-km map's zero contour no longer drops a far-field sample (`cov <= 0` early-out
  only when the 20-km fraction is 0 too), and the strength sets the optical THICKNESS (`d *= 0.02 + 0.98 e^2`,
  thin parts translucent). "Morphology from (m/px)" (`morph_far_footprint_m` 1400, slot 237, `morph.z` = log2) =
  where the far field is full (in from a quarter of it), "Morphology breakup" (`morph_breakup` 0, slot 238,
  `morph.w`, pulls the far fraction toward 1/2: measured little; left at 0). Cost +0.4 ms at 420 km, +0.65 ms at
  8000 km (cloud march, 1920x1009). **What the morphology can show is bounded by the map:** from 420 km the far
  field is already full (the half-res pixel is ~1 km at fov 60), and over most of a broken region the map's
  coverage, even averaged over 20-40 km, is near 0 or 1, so the imagery only shapes the edges and the inside
  thickness; its 1-3 km structure (rims, puffs) is below the half-res pixel. The limb from 20000 km shows
  alternating half-res columns in cloud (open; not the far-field reads, the march settings, the TAA or the rate).
  (5) **The FAR CLOUD LAYER** (`cloud_v2_far.comp`, the user's design: "from afar switch to the detail textures
  instead of running the march"). What resolution buys was measured first: an HQ photo at 2x (the march at window
  resolution), downsampled, read D 1.51 / slope 2.03 / 19 holes per 1000 km^2 against 1.31 / 2.58 / 1.7 — MODIS is
  1.47-1.73 / 1.5-2.35 / 1.9-9.8. Sharper far-field reads in the half-res march gave only 1.38 ("Far-field sharpness",
  `far_sharpness`, slot 238, `morph.w` = 2^-it; left at 0). The layer: per FULL-RES pixel, the ray meets a sphere
  1.5 km up; `cv2FarColumn` (clouds_v2.glsl, a hand-kept copy of cv2FieldLow's far branch: same flowed and warped
  weather, the 20-km fraction, the morphology) gives the strength e; the optical depth is the type's extinction x a
  thickness from e (presence ramps in from e = -0.15: the march's lobes put cloud a little outside e = 0) and it is
  lit as a two-stream slab (R = a tau / (1 + a tau), a = 0.75 (1 - g), g 0.85) by the march's own `sunColorAt` /
  `skyZenithAt` (copied), with relief from e's gradient (two more columns a footprint away), the Moon and city light
  by night; the mid and high layers are sampled inside their lenses (3 heights each, placed from their own cluster
  reads) and added over it by the ADDING method (a plain attenuator drew grey camouflage over the decks). Output: full-res
  RGBA16F (`cv2FarImg`), march set binding 21, sky set binding 29 (imageLoad; the sky set's storage images 3 of 4).
  `CloudParams.farBlend` (was v1's unread hgG; `SatelliteSim::cloudFarBlend()`, smoothstep of the eye's altitude between
  "Far cloud layer from (km)" 600 and "... full at (km)" 1500, slots 239-240): cloud_march.comp fades the march's
  clouds out by it (and drops their occlusion distance once not opaque), sat_sky.frag composites the layer behind the
  half-res composite (`cloudA += farBlend L T_march; cloudB *= mix(1, T_far, farBlend)`; the air in front splits at the
  layer's distance for its share), and at 1 the v2 march and resolve are not dispatched. "Far cloud layer sunlight" 3
  (matched to the march's brightness from 8000 km, p97 232 vs 234) / "sky light" 1 (slots 241-242, `cv2.farLight`).
  Measured: 8000 km cloud bucket 9.3 -> 1.4 ms, 2000 km 11.9 -> 3.1-4.0 ms; still-view flicker at 8000 km 1.0% (start
  of review 18) -> 0.04% of pixels past 8 levels, 20000 km 1.4% -> 0.27%. **Not in the layer:** Cb towers and the
  map-fed anvil shield (storms from far orbit are the low layer's deep types), cloud shadows on the ground, rain.
  **No scene-depth test:** the half-res depth read per full-res pixel alternated over land from orbit (a dot grid
  through the layer). The march's scattered small puffs at ~2000 km (low layer, independent of the morphology: the
  margin's lobes) are not reproduced; the cross-fade band (600-1500 km) matches.
- **Review 19 (2026-10-01): ONE cloud placement at every distance.** Climbing straight up over one place, review 18's
  clouds were REPLACED between ~35 and 450 km: the near field (the cells thresholded by the 5-km map) and the far field
  (the imagery thresholded to the ~20 km fraction) were two placements switched by the pixel footprint (`farK`). Measured
  with `tools/harness/climb.py` (docs/HARNESS.md "Climb consistency"): detail seen from 40 km correlated ~0 with any view
  from 450 km up. Now `cv2FieldLow`'s placement is the near field PLUS the imagery at a fixed share ("Imagery share",
  `imagery_share` 0.5, slot 235, `morph.y`: 0.3 x it x clamp(zMs) in field units, zMs = the Perlin / morphology mix of
  "Morphology (orbit)"), thresholded at 1 - max(cov, 0.5 x the ~20 km fraction), the same at every footprint; a wider
  footprint only filters it. Cells too small for the pixel become PARTIAL COVER (`presFar` = 0.5 + (field - thr) /
  (2.5 sigU), sigU the cell variance the mip averaged away), the sub-pixel haze's opacity — never a second placement.
  `farK`, the far thinning (`d *= 0.02 + 0.98 e^2`) and "Morphology from (m/px)" are gone (`morph.z` unused).
  `cv2FarColumn` computes the same field and fraction (`CV2Far.frac`, the far layer's optical depth x it). EVERY consumer
  binds the morphology now — `cloud_march.comp` 22 (the ground shadow: shadows fell under the old placement), beam
  occlusion 9, lightning / light volume 20 — or its clouds sit elsewhere. Climb (user snapshot, fov 40): r vs 40 km at
  1500 km -0.06 -> 0.72, vs 200 km at 2500 km 0.25 -> 0.57, worst step 0.49 -> 0.70; close-ups match review 18's;
  cost unchanged. Tried first and dropped: the imagery ALONE everywhere (consistent, but the map-driven cumulus masses
  became scattered puffs and stratocumulus decks flat pancakes — the near look the user liked). **Resolve:** a pixel
  with no cloud of its own reprojects at the nearest cloud distance of its 3x3 new samples (`dNear`): at infinity, in
  motion low over small puffs, its history came from where a puff had been and the parallax floor (50 km) called it
  still — every small cloud smeared into a streak and faded (snapshot 2: walk |diff| vs settled 5.2 -> 3.1).
  **The sea:** the sky reflection's march was gated on reflStr (Fresnel ~0 within ~34 deg of the nadir) and the foam and
  surf take reflColor as sky light, so at dusk a hard light-blue disc sat under the observer; the open ocean's water level
  decoded to 2e-5 m (a "lake"); reflected clouds come only from on-screen texels.
- **Review 20 (2026-10-01, the user's night snapshots over SoCal):** **dust is lit by city light and the moonless sky
  at night** (cloud_v2_march.comp fog/dust march; the fog had city light, the dust only the Moon): from inside its band
  (eye under ~1.5 km) a long horizontal path through it hid the city-lit sky and gave nothing back — a near-black band
  along every night horizon that vanished on climbing (horizon 7 -> 42 levels; dust off reads ~60), and where a far
  cloud's half-res silhouette hid the dust behind it, a pale blocky band (snapshot 1: lower contrast now, its blocks
  remain). The dust reads the night map at ~80 km (`mix(3, 4, fd)`), `kDustCityK` 1. **The resolve reads its history
  Catmull-Rom** (sky_taa.comp's 9-tap form) and **"still" is stricter**: the moving weight, the tight box and variance
  clipping ramp in from 0.05 / 0.05 / 0.1 px of parallax (were 0.25 / 0.3 / 0.5). Climbing at the player's Q speed,
  horizon clouds 30-100 km out move ~0.1 px a frame and kept the still weight and wide box, so depth errors built up
  into a soft vertical smear (snapshot 4, beside the Reflect beams — not the shafts: shafts off changed nothing):
  tstab rise 0.87 -> 0.65 (full rate 0.51 at +5.8 ms), review 15's scenarios 1.21 -> 1.18, still floor unchanged.
  The adaptive rate's parallax threshold is not the lever there (0.2-1 px identical: nothing passes it).
  **Review 20b — the "chunkiness":** (1) **the resolve also clamps history in plain RADIANCE** (the new samples' rgb
  box, same slack): the mean-colour space divides by opacity (floor 0.02), and inside the Reflect beams every clear
  pixel carries shaft light at ~0 opacity, i.e. x50 there — a cloud-edge history texel clamped up to it came out as
  bright blue-white 2x2 blocks along every cloud edge in motion (user snapshot, 5.4 km over the Anchorage site; rise
  >16 levels 1.0% -> 0.1%). (2) **The resolved distance is the 3x3 new samples' opacity-weighted mean** (`dW/dWs`),
  in motion and into the still history: a translucent haze's mean distance is a noisy per-ray estimate, in motion a
  sparse block took its one sample (the depth history is still-only, review 15), the sky pass split the air tens of km
  too near, and dark 2x2 blocks flickered along distant cloud edges (snapshot 2, 11 km; debugview cloudairsplit in a
  `path play record=` shows it — transmittance and radiance were smooth). Still, the same noise left a thin dark
  stair-stepped line there and blocks in the horizon band (snapshot 1). Still-view flicker (420 km limb, 2000 km
  nadir) identical; review 15's set 1.21 -> 1.18.
- **Review 21 (2026-10-01):** **Sunset ground shadows** (`cloudGroundShadowV2`): the low stretch takes as many steps as
  keep them <= 2.5 km (20-60, read at 1.2x the step, <= 3 km). At a 0.3 deg Sun the stretch is ~200 km and a 1-km cloud's
  height band holds the ray for ~190 km: at 20 steps of ~10 km each step that landed on a small cumulus drew its own disc,
  one cloud's shadow a chain of separate ellipses (user snapshot, 8 km at sunset). Now one long streak (a faint ladder
  remains on thin ones). +2.3 ms of cloud march in that view, nothing above ~17 deg of Sun. Tried: the step's whole length
  as the footprint (the scattered cumulus averaged away: no shadows) and per-frame jitter (needs the sky TAA, which is OFF
  below render scale 1 — the user runs 0.85). **The far cloud layer's light** ("Far cloud layer sunlight" 10, was 3;
  "sky light" 0.3, was 1): from orbit most of a cloud pixel is the haze in front of it, and at 3 the layer's own light was
  a third of the march's and its blue sky term dominated (debugview cloudrad, cloud cores: far R/B 0.76 vs march 1.43) —
  bluish clouds from ~1500 km. The orbit grade is not involved. At 10 / 0.3 its radiance matches the march at 1460 km and
  its image at 3000-8000 km (review 18's "3" was matched on p97 at 8000 km, where few pixels are cloud). Saved settings
  keep the old values. Also: partial cover is an area mix in the layer (share x full-depth light), not one thinner cloud
  (no visible change here). **Sea:** waves out to "Sea wave range (km)" (`ocean_wave_range_km` 100, slot 237, UBO
  `oceanWaveRangeM` = v1's unread marchSteps; was a fixed 3-8 km) and "Sea wave sharpness" (`ocean_wave_sharpness` 0.1 since 2026-10-20, was 0.5,
  slot 243, `oceanWaveFootK` = v1's unread lightSteps: x the footprint the octaves are filtered at; 1 = review 14); +0.1 ms.
  The reflection no longer decays with distance (exp(-d / 40 km), from the first waves). The reflected clouds' screen fade
  lies just OUTSIDE the frame (review 19's fade inside it took every row's last 8% off the clouds at a horizon view:
  bright bands at both edges). Open: the dark line where an overcast's far underside meets the sea horizon (not the sea
  shading: oceansurf is uniform there). Note: a harness `set clouds_v2.dust_amount` changed the whole scene there (clouds
  and Sun gone) and `knockout +fog_layer` nothing — not understood.
- **Review 22 (2026-10-02, the user's notes + snapshots 1-9 = profile_log records 9-17, record N = snap N - 8;
  plan in `.plans/REVIEW_22_PLAN.md`):**
  - **Cumulus lobes taper with height** ("Cumulus lobes (bottom)" 0.2 / "(top)" 0.07, slots 244/245, keys
    `cumulus_lobes_bottom/_top`, UBO `farLight.zw`): on convective types they replace "Lobes (3D)" (form.x, same
    units) over smoothstep(0.2, 0.9, zeta) of the cell; the old 0.8 -> 1.25 rise with height is gone. The old look
    is ~0.26 / 0.40. Tops are now smooth domes over lumpy bases.
  - **Rain falls ~10 m/s** near (8.4 in far layers; was 8). It cannot follow the rain rate: a drop's offset is v x t
    with t up to 600 s, so any change of v moves every drop at once.
  - **Rain-only samples take the thin-ice path** (one sample per two coarse steps, no fine steps, light LOD): under
    the record-10 storm the cloud march was 22 ms, ~15.5 of it the shafts' fine steps; 31.3 -> 20.2 ms GPU total.
  - **Drops follow the rain volume**: cloud_v2_lightning.comp's workgroups 1-4 fill a 32 x 32 rain-rate map (40 m
    cells, +-640 m about the eye at its height, `cv2RainMap` + `cv2RainWgMax` after the flashes in the flash
    buffer; host offset `kCv2RainMapOffset`); each drop layer reads it at its own point (`rainMapAt`), so a shaft's
    drops appear in the far layers first. It replaced a cv2FieldLow at the eye PER PIXEL in cloud_march.comp
    (cloud bucket ~20 -> ~12 ms in the storm). The rain ambience reads the map's centre +-120 m on the host;
    `terrainFrame.w` is no longer written.
  - **Heavy rain under the dominant towers**: `gCv2RainCore` (set by cv2ColumnSigma, read by cv2FieldLow's rain
    branch; cv2Field calls them in that order — any other caller of cv2FieldLow sees 0) is a core under each
    anvil-reaching tower's base, 2.5x longer downwind; the shafts take max(map rate, core) at up to 2x density.
    The tower search now runs down to the ground while it rains (it stopped at 400 m). The curtains read 1.5 mips
    coarser with a softer threshold (the striped on/off pattern).
  - **The Sun behind the clouds toward it** (`CloudParams::sunCloudT`, was the unread `cloudsV2`): the lightning
    pass's thread 1 marches 400 km toward the Sun from the eye (below the shell top; 1 above it) into
    `cv2FlashPad2`; the host eases it (0.15 s, `sunCloudTEased`) into the UBO. It multiplies the Sun disc, glare,
    corona and lens flare (min with the pixel's cloudBlock), the Sun's bloom seed (`sunRefIntensity`), the direct
    in-scatter of air within ~50 km below 8-14 km AND within ~3 km of the eye's line to the Sun (sat_sky.frag; over
    the whole sky one cumulus in front of the Sun would darken everything), and the key light of RAIN samples within
    ~100 km (the rain phase's sharp forward lobe drew the Sun's disc in the shafts; with rain_amount 0 the glow is
    gone). A dim, soft glow remains.
    Bounding the key light of ALL cloud samples near the Sun's direction was tried and reverted: it punched a dark
    hole where the cloud toward the Sun is thin. Harness `state` -> `clouds_v2.sun_cloud_t`.
  - **No concentric rings round the nadir** (snap 8): the ray's start jitter now spans the COARSE interval (two
    steps); over one step the coarse lattice took half its phases and a thin deck seen from above was skipped at
    some, found at others. Test: a 30% change of "Step base" moved +-16-level bands before, 2 levels after
    (`harness_runs/r22_ringdiff*.png`). The fine restart's jitter is fract(1.618 j + 0.5). A per-interval sample
    position or a second blue-noise read flipped the march to 222 registers.
  - **"Fast-flight cloud LOD"** (slot 246, `fast_flight_lod`, default on; state `clouds_v2.fast_lod`): boost held
    and moving, or > 150 m a frame: <= 2 light steps, 60% of the iteration budget, a 1.5x base step. Only -0.5 ms
    climbing through the storm in the harness. Value 2 = whenever the eye moves (> 2 m a frame), boost or not (the
    harness holds no keys): boosting at record 10 (109 m, ~11 m a frame) cloud march 11.5 -> 9.8 ms but tstab boost
    1.80 -> 2.35 and the worst frame unchanged (~26 ms; `path play`'s `gpu_worst_frame_ms`: the spikes are the
    cloud march, 18-22 ms against a ~12 ms mean).
  - **"Foveated full rate (radius)"** (slot 247, `fovea_radius`, morph.z, default 0): an experiment — centre tiles
    at full rate while moving made strafing WORSE (tstab 1.60 -> 2.10) at the same cost. Left off.
  - **Far cloud layer**: the key light's reflectance is the two-stream slab's at the beam's incidence (`slabRmu`).
  - Looked at, not changed: snap 4 (record 12, 14 km) is the anvil lid's own horizon with the high layer seen past
    it and a taller storm beyond (debug view 7); snap 7 (record 15) shows no visible flicker in the harness (0.04%
    of pixels past 8 levels, on lit cloud bottoms under a 2-deg Sun); review 21's `set clouds_v2.dust_amount` "blanking"
    is the cloud history's reset (one noisy frame, then recovers; the settings hash includes it); snap 6 descending (record 14, tstab `fall`
    3.8 vs `rise` 1.8): history lag as the deck expands toward the eye, and the sky TAA's band as the eye's height
    changes.
- **Review 22b (2026-10-02, the user's follow-up):** **the Sun hidden by a storm, and the clouds in front of it
  dark:** at record 10 the composite's cloud transmittance toward the Sun was ~0.005 — the glow was the clouds' OWN
  forward-scattered light, the light march (a few km) not seeing the storm between them and a low Sun. The lightning
  pass's Sun march now writes a PROFILE (`cv2SunProf` in the flash buffer: its length + the optical depth from the eye
  at L (k/16)^2, k 0..16; the march includes cloud_lightning.glsl at binding 12): a cloud sample near the eye's line to
  the Sun takes exp(-(depth beyond its point)) on its key light, faded by its distance from the line (Gaussian, 2 km;
  8 km darkened a sunset-lit deck 3 km above the line). Clouds beyond the blocker keep their light (silver linings).
  The first cut overwrote the profile's last value with 8 after a full march (every cloud near the Sun line went
  dark): fill only after an early out. 128 registers. **Clouds through satellite meshes:** sat_sky.frag skips the cloud
  composite on a mesh pixel when the cloud's distance (|alpha|, km) is beyond the mesh — the march clamps only to the
  half-res depth (thin panels and truss missed) and the far layer has no depth test. **Go to:** 0.8-2.5 s
  (`followFlightDuration`), the view eases from where it looked to the satellite over the first third, and the arrival
  point is on the line from the satellite back to where the flight came from (`followFlightAlign`), so it settles in
  the facing it approached with. **Far sea:** the reflected clouds are averaged over 5 taps along the screen's vertical
  (spread by distance and roughness) and lean 30% more to the sky's own reflection past 3-30 km: a mirror finish to the
  horizon.
- **Review 23 (2026-10-02):** **Go to flies straight along the line of sight** when the Earth is not in the way
  (`followFlightLine`): the distance to the satellite closes by a constant ratio, along the start's line, onto the arrival
  point on that line; the arc (kept for a satellite behind the Earth) ended in a vertical climb to a point beside the
  satellite, where the facing came from a near-zero horizontal view and spun. The follow aim's facing keeps the previous
  one near the zenith / nadir (`setView`). **The aim is FREE on arrival**, held in the satellite's frame
  (`followBasis*`), so the satellite stays put on screen without the lock. **The sea's body** (kSeaBase, the crest and
  the subsurface term) is lit by the Sun + sky irradiance on the sea (`seaE`, as the foam), not by the cloud shadow alone:
  it kept its noon brightness up to the terminator and read cyan at sunset. **Sea grid:** a periodic 2D warp of the whole
  wave field (24 / 8 cells, `seaNoise2P`) and each octave's second component on a 45-degree lattice; the warp the
  octaves had was a shift along the diagonal only, and the ridges read as a square grid.
- **Review 24 (2026-10-02, the user's snapshots = profile_log records 2-7, harness_runs/r24_snaps.jsonl):** **Go to
  rides the OBSERVER'S line of sight both ways** (the parked ground observer to the satellite, re-aimed every frame;
  `followHomeEcef`, `followFlightS0/Delta0`): the distance changes geometrically, the arrival offset is on that line,
  and the way home keeps the satellite in view for ~60% before turning to the saved view, so a flare the observer
  sees stays in view in and out (benchmark: snap 3, Starlink Gen3 Direct-to-Cell n=7350). **Far-sea ripple**
  ("Far sea ripple", slot 248, `seaTune.x`): where the footprint filter has removed the wave octaves, a ripple normal
  tilted along the view, its cells ~3 along-view footprints (resolvable, so no aliasing; long thin bands on screen),
  on the waves' lattice with periods dividing kSeaCells — the far band was a mirror. "Sea warp" / "detail" (249-250,
  defaults 3.5 / 1.2: 4.5 stretched the ridges in places). **The far cloud layer was ~0.2 clearer than the march**
  (mean transmittance 0.76 vs 0.58 at nadir from 900 km): the march's 3D lobes put cloud past the 2D field's edge.
  "Far cloud layer coverage bias" (0.12, slot 252, `farTune.y`, field units) matches it (0.580 vs 0.576 nadir,
  0.43 vs 0.41 oblique at 668 km); "slant coverage" (251, apparent cover 1 - (1 - f)^(1 + k tan z)), "density" (253),
  "edge softness" (254). **Its key light went as mu0** (a flat slab) and went dark ~200 km before the terminator where
  the march's 3D tops stay lit: mu0^(1 / (1 + "low-Sun light")) (255, 0.5; far/march by strip across the terminator
  0.3-0.66 -> 0.92-1.23). The "Flat layers" sliders are v1's 2D layers: drawn only in reflections / probes, SKY_LITE
  and Potato, not the main view while the march runs. **Exposure from orbit**: the lit gate 0.08 -> 0.04 (review
  23's darker sea fell under it) and the lit target 0.42 -> 0.47 (snap 4: -1.37 -> -1.09 EV).
- **Review 24b (2026-10-02, records 8-9):** the far layer's defaults are the user's tuning (full at 8900 km, slant 0.91,
  bias 0.12, density 1.2, low-Sun 0.38, sunlight 4.1, sky 0.6). **Go to on moving satellites** (benchmarked at 1x,
  view-to-satellite angle per frame, harness_runs/r24b_goto2): the arrival takes the offset where the flight ends (the
  line rotated during the flight: a 1.4 deg jump on landing), and the way home stays on the satellite to the end and
  lands looking at it (turning back to the start's view swung the camera up to 55 deg while zooming out); 0.0 deg
  both ways. **The sea's waves fade with altitude over 20-60 km** (`altFadeW`; was 3-8 km, a pop of the wave texture
  and glitter path descending through ~7 km); the footprint filter does the rest. Mirrored features keep 3-8 km.
- **The far layer's edge is the march's ERODED edge (2026-10-21, issue 20261021_173412: "2D and volumetric clouds do
  not match", climbing 980 -> 6656 km over 7.9 N 131.8 E).** At the shipped "Detail" 0.77 the march draws cloud only
  where a column's density at the cloud's widest point survives the mean erosion; the coverage it draws moves a lot
  with "Detail" (from 980 km: 94% of the frame at 0, 37% at 0.77, 17% at 1.5). `cv2FarColumn` thresholded the plain
  field at `1 - covT - bias`, i.e. ignored the erosion and the convective profile, and the review-24 bias (+0.12, tuned
  against a march with less erosion) pushed it further: the far layer had 90% cloud where the march had 45%
  (r 0.59 at 980 km). Now it finds the margin xeMin at which cv2FieldLow's eroded density is positive (10-step
  bisection on the same terms: small-cumulus lift, eS / capS, reqMin from eB, flatK / convW, phK / phP, subPix /
  presFar, the mean erosion with billow and wisps; lobes left out) and the pixel share ramps up from there over
  1.25 sigU (+ a lobe width while the cells resolve). "Far cloud layer coverage bias" defaults to 0 (was 0.12; a saved
  settings.json keeps its own). Kept in step with cv2FieldLow BY HAND. Measured (`harness_runs/iss173412`, march alone
  vs far layer alone at the same altitudes, red > 0.6 cloud fraction / 3-px-blurred r): 980 km 0.44 / 0.36 r 0.93,
  2200 km 0.31 / 0.28 r 0.96, 5000 km 0.19 / 0.18 r 0.97, 9000 km 0.13 / 0.13 r 0.99 (before: 0.45 / 0.90 r 0.59, 0.32 /
  0.86 r 0.55, 0.19 / 0.61 r 0.59, 0.14 / 0.37 r 0.86). A diagnostic far shader that integrates cv2FieldLow down each
  ray (`harness_runs/iss173412/mkdiag.py`) checks the edge against the march at any "Detail" (mask IoU 0.78 / 0.95 / 0.69-0.75
  at 0.77 / 0 / 1.5).
- **Review 25 (2026-10-02, records 10-12):** **a straight line across the sky in the clouds** (from under / inside a deck
  at 65 S) was review 22b's Sun-path darkening, applied only for cosSun > 0: with samples within ~2 km it was full on one
  side of the great circle 90 deg from the Sun and off on the other (debug view cloudrad stepped; transmittance and the
  air split were smooth). (The one-ray profile is gone since 2026-10-10: see the Sun shadow grid below.) Behind the eye the distance to the eye's Sun RAY is the distance to the eye (continuous at
  90 deg); sat_sky.frag's near-air dimming doubled it there (also fixed). **Far-sea ripple cells are 5 along-view
  footprints** (3 drew value-noise blocks ~3 px tall: "pixelated") plus a component on the 45-deg lattice.
- **The Sun shadow grid (2026-10-10, replaces review 22b's profile; user snapshots = profile_log records 10-12,
  harness_runs/sunshadow/snaps.jsonl, snaps2.jsonl):** cloud_v2_lightning.comp's workgroups 5-8 (the dispatch is 9)
  march a 32 x 32 grid of parallel rays toward the Sun, 250 m apart across it about the eye, and store the Sun's
  TRANSMITTANCE beyond 17 points of each ray (`cv2SunGrid`, appended to the flash buffer with `cv2SunWgMax`: each
  workgroup's largest optical depth, all 0 = the march skips the block; ice at the light march's 0.2; `cv2SunProf`
  now holds only the rays' length [0] and the two axes [2..7]). A cloud sample takes the four rays around it
  (bilinear) at the point where its own light march ENDS (2 x `cv2.light.x` further along; 0 for thin ice, which
  has no light march), fading out 3-3.9 km from the eye's line. The profile was ONE ray applied within a 2-km
  Gaussian of it, from the sample's own point: (a) the Sun slipping behind a cloud's tip darkened every cloud within
  ~2 km of the eye's Sun line — 340 m of walking turned the whole overhead cloud dark blue-grey; (b) the blocking
  cloud took its own depth twice (light march + profile), so the spot the Sun stood behind was the darkest. Thread
  1's own ray still gives the Sun DISC's transmittance (`sunCloudT`). With the offset at 0 the grid draws each near
  cloud's self-shadow as hard radial wedges converging on the Sun: the offset is what keeps them out.
  **Three things keep it from aliasing** (the first cut, 7 x 7 rays 500 m apart, drew straight-edged SQUARES of light
  and shadow on a sunset deck that flipped with ~100 m of climb; user record 12, Afghanistan, 7.7 km): each ray is
  the mean transmittance of four sub-rays over its cell; 48 steps per sub-ray, each sub-ray at its own phase of the
  step (the far steps are ~15 km: one ray hits or misses the cloud tops); and the rays' length is NOT a function of
  the eye's height (`cv2SunGridLen`: with (shell top - eye) / sin(el), every sample slid along its ray as the eye
  rose, and the far shadows changed with it). Test it with a FINE climb (120-m steps from the snapshot: the first
  check, at 2-4 km steps, showed nothing). 128 registers; cost within noise (14.9 ms vs 15.2 at record 12). Not
  re-verified: a low Sun behind a DISTANT storm with cumulus in front (the case 22b was built for).
- **Anvil pass (2026-10-10, user snapshots = profile_log records 13-17, harness_runs/anvil/snaps.jsonl; 25 N 134 W, Sun 0.6 deg):**
  - **Surface refinement in the view march** (`cloud_v2_march.comp`, `tLo/tHi/refN`): a fine step is 1.2% of the distance
    (~460 m at 40 km) and a tower's lit skin tens of metres, so the first sample inside dense cloud sat at a random depth
    under the surface: each ray came back sunlit or self-shadowed by chance. That was the "grid" on distant anvils (grain
    at the sparse grid's 4-px scale that the history never averaged out) and a ~17% dark bias against a 25-m-step
    reference (`set clouds_v2.step_growth 0; step_base_m 25; max_iters 6000`). When a sample is opaque over its step
    (sigma x step >= 1), the one before it was not, T > 0.1 and the step is past (2 + 1.5 x jitter) base steps (~7-14 km,
    dithered), the crossing is bisected three times with unlit field reads at the SAME call site (`continue`) and the
    bracket's inner end is shaded. Distant towers are brighter and smooth; snap 4's high-pass noise 8.3 -> 2.6 levels,
    still-view flicker std 1.25 -> 0.78, tstab 2.03 -> 1.79; +0.4-1.1 ms still, +1.0-1.3 ms moving; 128 registers.
    Tried: no distance gate (+3.4 ms looking down into near cumulus, no gain there); two bisections for shorter steps (a
    hard-edged darker oval where they gave way to three: the brightness follows the bracket's width); shading where the
    view ray's optical depth reaches ~0.7 from the density slope across the bracket (corduroy stripes, 222 registers).
    Open, visible now the grain is gone: hatching in the towers' self-shadow penumbrae and faint contour arcs on the
    heads (the light march's long jittered steps; the light LOD's second step is ~2.4 km).
  - **The towers' lobes flow up and out** (`cv2ColumnSigma`, `kCv2ColFlowSpan` 0.35): every noise anchor slides along ONE
    direction of the drifted ECEF frame (`windDir`), and the towers stand still in that frame, so the storm lobes crossed
    them at whatever angle that direction makes with the local vertical — at these snapshots 56% of the wind straight
    DOWN ("clouds falling down their columns"). The towers now read the storm shape volume at a wind-free anchor
    (`anchorCol.xyz`, the unused 3D-lattice anchor's slot) displaced against a flow — up the stalk, and in an
    anvil-reaching tower's head outward from its axis — at two phases half a cycle apart, cross-faded and renormalised
    (`anchorCol.w` = the phase, 1.5 x "Wind" over 0.35 of a period: ~11 min a cycle at the user's 22.8-km storm period).
    The erosion detail reads a wind-free anchor too (`lightning2.yzw`). A paused view's lobes differ from before (another
    part of the volume). **Register notes:** carrying the outward VECTOR out of the tower loop, or a second inlined
    `cv2ShapeSmooth`, each gave 222 registers; one carried float (the bearing, `bestAng`) and a 2-iteration fetch loop keep
    128. Not changed: the low layer's lobes (storm cumulus, every cumulus) still slide along `windDir`, so they sink or
    rise by place; the map-fed shield (`cv2AnvilSigmaP`, off by default) still reads the wind anchor.
  - **The Sun shadow CASCADE** (`cv2SunCasc` in the flash buffer, `cv2.sunCasc0-2`, "Far cloud shadows (0/1)" slot 269,
    `clouds_v2.sun_cascade`, default on). A sample's light march reaches ~5 km and the fine Sun shadow grid covers
    3-3.9 km about the eye's line to the Sun, so a tower 20 km off stood sunlit behind a storm that shaded it and the
    shadow popped in on flying up to it (user snaps 1-2; `harness_runs/anvil`: the approach test). The cascade is the
    same thing over the view: 128 x 32 parallel rays across the Sun's direction (the fine grid's axes: a horizontal,
    3.2-km cells, +-205 km; b the other, cells = (205 km x |sin el| + (mid-shell + 2 km) x cos el) / 16: 0.8 km at
    sunset, ~10 km at 45 deg), the window centred at mid-shell under the eye, 49 knots along the Sun (40 uniform
    intervals through the window, ~10 km at sunset, 8 stretched sunward by shell top / sin el, at most 500 km), each
    knot = the transmittance BEYOND it.
    - **The lattice rides with the clouds and is stored toroidally.** The CPU integrates the eye's motion relative to
      the clouds (the resolve's `Rz(dDrift) eye - prevEye`) along a and b into the eye's lattice coordinate
      (`cv2CascLatA/B`, double; never an absolute coordinate: a change of axes or cell size then pivots the lattice about
      the eye and moves no ray by more than metres between refreshes); ray (I, J) lives at [(J mod 32) x 128 + (I mod 128)].
      A ray keeps its slot while the eye flies, so a bake up to 32 frames old is still in the right place.
    - **Same bake cost as the fine grid alone:** with the cascade on, odd frames bake 2048 cascade rays (workgroups 5-12,
      2 sub-rays x 48 steps) and the fine grid refreshes on even frames only: the same field reads per frame as before.
      The dispatch is 13 workgroups. A reset (`!valid`: a teleport, a time jump, a settings change; Sun below -6 deg; eye
      above 60 km) zeroes its weight for 33 frames, then eases it in over 8 (`sunCasc2.x`). **Harness: `wait settle 160`
      after anything that resets it** (the default 40 frames captures it empty).
    - **Lookup** (cloud_v2_march.comp, after the fine grid's): four rays bilinear, two knots, read ONE KNOT SUNWARD of
      where the sample's light march ends (`sunCasc2.y`): a knot interval holds the sample's own cloud, and without the
      bias every sunlit face is shadowed by the body behind it. Final shadow = mix(cascade, fine grid, the fine grid's
      weight). 128 registers.
    - **Measured** (RTX 3070 Ti, 1920x1009, on vs off): still views' cloud march 0.3-0.8 ms LOWER with it on (shadowed
      samples end their rays' work sooner), moving (tstab paths) total 16.6 / 18.1 / 19.1 vs 16.6 / 17.6 / 20.1 ms;
      tstab pitch 1.14 -> 0.86, rise 1.27 -> 1.19, walk 2.94 -> 2.58. No new validation messages.
    - **Limits:** blockers 5-15 km sunward of a sample fall in the bias (the light march sees the first 5 km); above a
      ~15 deg Sun a knot plus the bias is several km of HEIGHT, so an anvil's shadow on the cumulus under it is mostly
      missed (it would need knots by height); no shadow beyond +-205 km; a paused harness view must settle 90 frames.
      Not yet read by anything else: godrays (the air's in-scatter along the view ray x this transmittance) and far
      ground shadows are the next consumers.
  - **Cascade round 2 (2026-10-10, user snaps = profile_log records 18-20, harness_runs/anvil/snaps2.jsonl; "insanely
    blocky and positionally unstable"):**
    - **Both bakes jitter their sub-rays per bake and keep a RUNNING MEAN** (`cv2.sunCasc3.zw` = the new bake's weight:
      1 / bakes since a reset, at least 1/8; an R2 offset in the cell and a golden-ratio step phase from each ray's
      hash). With fixed sub-rays a ray knew a cloud top's height to a quarter cell (62 m fine, 380 m cascade), and at a
      0.5-deg Sun that is 7-40 km along the ground: shadow edges were stairs. The stored value is now the cell's mean.
      **Harness: `wait settle 160`** after a reset (33 frames to fill + ~32 for the mean).
    - **The fine grid's lattice rides with the clouds and is toroidal too** (`cv2GridLatA/B`, `sunCasc3.xy`): it bakes
      on even frames only while the cascade is on, and an eye-centred grid a frame old sat a cell off at the player's
      speed. With the cascade up it is used only within 3-6 km of the eye (`w *= 1 - smoothstep(3 km, 6 km, t)`): as a
      tube along the whole line to the Sun it drew its own answer through the cascade's on whatever stood down-Sun.
    - **Thin ice casts no shadow in either bake** (`od += sigma x (1 - thin)`): along a grazing ray the high layer
      shadowed itself over tens of km. Thin samples read the cascade at the MIDDLE of their layer (`f.topH`, `f.hf`):
      at a jittered height in it the shadow speckled.
    - **The lookup is `cv2SunCascT(rl, sAlong)`**, one function for the cloud samples' key light and the godrays (128
      registers with both call sites).
    - **God rays = the sky pass shadowing its own air by the cascade** (sat_sky.frag `SKY_SUN_SHADOW`: every variant
      but SKY_LITE and the SKY_ENV probes). Each N_VIEW step's sunlight is x the mean of three `cv2SunCascT` reads
      along the step (half a knot sunward; one read per 10+ km step drew a shadow's edge as bands), and the shadowed
      share is lit by the sky like the air in the Earth's shadow (`shR` / `shM`: or a storm's shadow is a black wedge).
      `airFront` takes it too, so the haze in front of a cloud is shadowed with the sky beside it. The cloud samples'
      key light reads the same lattice: one Sun shadow for the clouds and the air between them. "God rays"
      (`clouds_v2.godrays`, slot 187, `cv2.lightVol.w`) is now 0..1 = how much of the shadow the air takes, default
      1 (a saved 3 loads as 1); section renamed "Cloud shadows & god rays". Review 22's `sunCloudT` dimming of the air
      near the eye's line to the Sun gives way to it (x (1 - sunShK)).
      - **Bindings:** the sky set grew to 32: 30 = the clouds' flash buffer (its 10th storage buffer; the device check
        asks for 11), 31 = the clouds v2 UBO (`clouds_v2.glsl` with `CV2_PARAMS_ONLY`). `cv2SunCascT` lives in
        `cloud_lightning.glsl` behind `CV2_SUN_CASC_FN` (it needs the cv2 UBO).
      - **It replaced the march-side godrays** (and `cloud_v2_lightvol.comp` is no longer dispatched: `bakeLightVol`
        false; the volume and its bindings remain). The march could only scale a pixel's transmittance, and the sky
        pass (since review 8b) dims only the air BEHIND a pixel's cloud distance: a clear pixel was never dimmed and
        one with the faintest veil was, by all of it. The user's "pixelated shadow on the cirrus" (snap 2, "God rays"
        3) was that sheet, with the stair-stepped outline of "any cloud found" (`debugview cloudtrans` smooth,
        `cloudairsplit` blue = no distance in the hole) - the same edge whichever volume it read. Tried and reverted
        while chasing it: a wider footprint for the air split's distance, and the mean cloud distance taken in
        airlight space (1 - exp(-t / 40 km)).
      - **Measured** (1920x1009, sky_taa off as the user runs): sky pass +0.7-1.0 ms, GPU frame 19.55 vs 18.80 ms
        looking at the Sun; still-view flicker std 0.12-0.17 levels (0.04 off); no new validation messages. Looking
        down-Sun at sunset the shadows converge on the antisolar point (anticrepuscular rays).
    - **Near-field shafts + haze (2026-10-11, in progress, unjudged):** a third lattice, `cv2SunShaft` (flash buffer,
      128 x 128 rays at 250 m across the Sun, 32 toroidal knots along it spaced so +-16 reach ~5 km of height,
      `cv2.sunCasc4/5`, CPU `cv2ShaftLatA/B/S`), baked by lightning workgroups 13-14 (512 rays a frame, all in 32;
      each starts from the cascade at the window's sunward end; dispatch 15). The sky pass samples it 24 times over
      a step's first 14 km (`cv2SunShaftT`, haze-weighted). "Haze near clouds" (`clouds_v2.haze_aloft` 1, slot 188,
      the old "God ray range"; `cv2.sunCasc2.w`) adds Mie with a 2.5-km scale height, 0.25 + 0.75 x the weather
      cube's cover (+ rain), so lit shafts glow toward the Sun. The fog/dust march's sunlight reads the cascade (one
      dithered ray, `sunC`; NOT gated by "God rays" yet). Cost: sky pass 4.5 vs 2.7 ms with god rays off (cumulus
      view, haze 3). **Register note:** removing the old march-side godray block left `godT` the constant 1.0 and the
      march went to 220 registers; `godT = (cv2.lightVol.w > 1.5) ? 0.0 : 1.0` brings 128 back.
    - **Cloud tuning pass (2026-10-11, the user's snapshots = profile_log records 21-29, harness_runs/r11):** the
      compiled clouds_v2 defaults are the user's tuned settings (High's march budget 430 / light steps 7 / growth 0.014
      follow). New, slots 270-282 (`kCloudSliderSlots` 283), UBO `tune / mid2 / mid3 / samp` (appended):
      "Small cumulus" (`small_cumulus` 0.06: the low field lifted at the coverage threshold, `max(xe, min(xe + d,
      d + 2/3 xe))`), "Erosion / Lobe variation" (one regional value `rV` from cl4.a), "Erosion size" / "Lobe size"
      (the detail / shape periods, moved to Shape), the mid layer's "Altostratus density", thickness, field size, cover
      bias and "Mid layer shadow" (`CV2Field.soft` = the mid layer's share; the light march and the Sun shadow bakes
      count 1 - mid2.z x soft of it: a sheet crossed at a grazing Sun drew a hard horizontal shadow line across every
      cumulus rising through it), "Cirrus direction warp" (five whole-count waves on the across-wind coordinate + the
      bundles broken into patches), "Far cloud shadow range (km)" (the cascade's half width, was a fixed 205).
      **Resolve: per-pixel AGE** (`cv2AgeImg` / `cv2AgeHistImg`, R32F, resolve bindings 9 / 10, copied with the
      history): weight max(w, 1 / (age + 1)), young pixels lean on the neighbourhood estimate, clamp rejection lowers
      the age ("Accumulation", "New pixel smoothing", "Motion clamp (sd)"). tstab at record 27: pan 2.19 -> 1.90,
      pitch 1.72 -> 1.36, walk 3.81 -> 3.54. **Register notes:** a second regional value (cl16.a) carried to the
      erosion block, and a smoothstep form of the small-cumulus lift, each gave 222. **Benchmark trap:** records
      28-29 were saved with `detail` 0; `snapshot` restores a record's settings, so compare with `settings=off` and
      the user's settings.json when judging the current look. With detail 0 the sub-pixel cover (presFar) shows
      20-30% cloud from 450 km where the resolved field has ~1-2% below 300 km; with detail 0.77 the erosion removes
      those marginal puffs at every distance. Open: the mid layer's shadow still has an edge; cirrus bundles from
      orbit are still wavy bands; "Small cumulus" at 0.15 visibly fills gaps in near views.
    - **Grain root cause (2026-10-11b, records 30-33, harness_runs/r11/snaps2.jsonl): the LIGHT LOD.** With the user's
      8-km light march and "Light LOD footprint" 16 m (so everything past ~5 km, and every sample behind T < 0.5),
      the 2-step light march's second step is ONE jittered point sample standing for ~7.5 km: in a cloud or in a gap,
      so each view sample came back sunlit or black. Raw frame-to-frame std (full rate, history 0.15, no TAA, a
      520x320 crop) 3.69 levels with it, 0.93 without; it is also far BRIGHTER than the 7-step march (crop mean 173
      vs 117), a lit / shadowed switch at a fixed distance from the eye (the "observer-centric" shadows). The default
      is now 0, and the CPU ignores the setting for a light march over 3 km (record 34 was still saved with 16 m and looked the same as before) (+2.5-3 ms GPU at those views, full frame ~30 -> ~32 ms). **"Light march footprint (x length)"**
      (`light_sample_footprint` 0.08, slot 283, `samp.w`; `kCloudSliderSlots` 284): every light sample reads the field at
      that x the march's length (640 m of 8 km; 0.04 left ~150 specks in record 34's lit top, 0.08-0.1 a handful, +~5% brightness): std 0.93 -> 0.6, pixels past 4 levels 3.6% -> 0.3%, same mean.
      It must be ONE footprint for all of lightAmbOD's calls: a per-step one gave 221-224 registers three ways.
      Settled view at record 32, old -> new: dark specks (5x5 median high-pass < -10) 2728 -> 193, temporal std
      0.229 -> 0.090. The specks were mostly the SAME pixels frame after frame: a running mean of ~100
      all-or-nothing samples still differs by several levels between neighbours. The resolve's age drop on clamp
      rejection is kept in still views too (without it 313 specks instead of 193). A saved settings.json keeps its
      own `light_lod_footprint_m`: set it to 0 there. Debug note: `debugview cloudrad` showed the specks in the
      march's radiance while cloudairsplit / cloudalpha / cloudtrans were smooth.
    - **Dark, blue cloud tops under a low Sun (2026-10-11c, records 35-36, harness_runs/r11/snaps4.jsonl; Sun 10-12 deg):**
      not the shadows (cascade off, light footprint 0, mid / high layers off, light march 2.5 km: all within 1 level).
      The user's "Multi-scatter strength" was 0.026 (from 0.52), so the key light was single scattering alone: at a
      grazing Sun that is the top few metres of a flat top, and the sky light (blue) carried the rest. Two octaves
      could not give both brightness and relief (raised, the second octave lights the whole interior: flat white).
      **"Multi-scatter octaves"** (`ms_octaves` 2, 1-8, slot 284, `mid3.y`; a runtime `[[dont_unroll]]` loop in
      `msOctaves`, 128 registers, cost within noise): at 6 octaves, extinction 0.7, strength 0.5 the top's mean went
      163/177/206 -> 202/207/223 with the lobes' relief kept (41-px high-pass std 2.04 -> 2.08; 0.5 / 0.5 gave 1.22).
      **"Lobe occlusion (sky light)"** (`lobe_occlusion` 0, slot 285, `mid3.z`): `f.ambient` x (1 - k clamp(-4 lobes,
      -0.6, 1)), the lobe noise's own value (creases low, tips high): no extra field read. Subtle at 0.6.
      `kCloudSliderSlots` 286. Defaults unchanged (the user's to judge).
    - **The Sun shadow grids are not reset with the SCREEN history** (`lightValid`, `cv2FieldHash`,
      `cv2ScreenResetOnly`): they live in the clouds' space, so recreating the screen images (the HQ photo's target, a
      render-scale step) or changing the march rate (`cv2FullRateAboveKm`, hashed after `hField`) keeps them. Tied to
      `valid`, the HQ photo's first ~40 frames had no far cloud shadows and no god rays, and they dropped out again
      for ~0.7 s in the window after it (user report). A teleport, time jump or field setting still resets them.
    - **A lightning flash in a paused snapshot lights the shadowed cloud** (snap 1's "grey slab" was the flash's glow on
      the shadowed stalk): `time add 5` before judging cloud lighting.
    - **Measured** (snap 18, 1920x1009, on vs off): cloud march 13.0 vs 14.9 ms still; moving (tstab paths) total
      16.8 / 17.4 / 19.0 vs 18.4 / 18.4 / 21.2 ms; tstab pitch 0.81 -> 0.49, rise 1.52 -> 1.32, walk 1.73 -> 1.56; a
      10-km strafe returns to the same image (0.57 levels mean difference).
- **Purple twilight clouds (2026-10-12, user records 39-40 = harness_runs/purple/snaps.jsonl; 38 S 117 E, Sun -1.7 deg):**
  the clouds' sky light (`skyZenithAt`, cloud_v2_march.comp + its copies in cloud_v2_far.comp / cloud_v2_lightning.comp) is
  a 12-step zenith integral (was 6). At six, one sample at ~8 km stood for the whole troposphere (deep red under a grazing
  Sun) and the next, at 25 km, for the stratosphere (blue), with nothing between carrying green: from about +1 to -3 deg of
  Sun every cloud's sky-lit side was magenta (linear R/G 1.7, B/G 1.9 measured; ambient gain 0 removed it, sun gain 0 and
  white balance did not) under a sky that was not. `harness_runs/purple/zen.py` replicates the integral: 6 steps 1.53 / 1.72,
  converged 1.00 / 1.29, 12 steps within a few percent from +10 to -5 deg (equal-mass exponential strata were WORSE: they
  starve the stratosphere). After: 0.96 / 1.23 in the render. 128 registers, cost within noise (it runs only on the
  sun-colour cache refresh). The terrain's own zenith integral (`N_ZT` 4, sat_sky.frag) is unchanged. "Terrain sky light"
  defaults to 8 (range 0-20): the user's tuning for twilight terrain.
- **Sky-coloured blobs on cloud bases (2026-10-12, user record 41 = harness_runs/purple/snap3.jsonl; 50 N 27 E, Sun 6.4 deg,
  under an overcast):** the ambient's light FROM BELOW (`ambLow`, cloud_v2_march.comp) is now the ground's: x (0.3 + 0.7 x the
  column's diffuse transmission 1 / (1 + 0.11 tauUp)) and 70% neutral (the sky light's luminance; 30% its hue). It was 0.35 x
  the ZENITH sky's colour, unshaded by the cloud above: an overcast's base glowed as blue and nearly as bright as clear
  sky, and the user's "Lobe occlusion (sky light)" 1 broke it into sky-blue blobs between warm seams (ambient gain 0
  removed them; sun gain 0 left them pure blue). Bright patches' linear B/R 1.31 -> 1.18. The lobe occlusion still mottles
  the base in grey at 1 (0 = a smooth base). 128 registers, cost within noise. The far layer and the rain light have no such term.
- **Cumulus shape pass (2026-10-13, user issues 20261012_235712 (9 shots) / _235159; `harness_runs/iss13/`, reference
  photos in `refs/` there, not committed).** What was wrong, in `cv2FieldLow`, and what replaced it (slots 289-295,
  `kCloudSliderSlots` 296, UBO `cu` / `cu2` appended, 928 -> 960; keys `cumulus_height_variation` 0.6,
  `cumulus_breakup` 0.6, `cumulus_top_headroom` 0.5, `storm_cumulus_fine_lobes` 0.5, `cb_fine_lobes` 0.5,
  `cumulus_wisps` 0.5, `silver_lining` 0.5):
  - **Pancakes** = (a) the lobes were damped to 5-30% at the base and reached full strength 700 m up, so a cloud
    thinner than that had none; (b) T (a cell's top) had a 0.25 floor and came from the coarse cells, one value per
    neighbourhood; (c) a stratiform TYPE at low coverage stayed a deck: a lens km wide, a few hundred metres thick.
    Now: `hVar` (the top varies by region, cl16, and by fine cell on shallow types; floor 0.12); `lowCov` / `flatK`
    (a deck type other than stratus is shaped as cells where covT < 0.25-0.65: placement unchanged); the lobes are
    read at `lobeRef` above the base (extruded down: puffs with flat bottoms) and `AshK` carves a shallow cloud in
    proportion to its own strength.
  - **Flat mesas ("blocky layers")**: the profile's requirement at the cell's top is 1 + 0.5 x headroom, past what a
    column has, so tops are shaped by field + lobes instead of cut at zeta = 1.
  - **No cauliflower ("gumdrops", "lobeless")**: as a margin the lobes moved a dome's top by tens of metres (the
    profile is steep there). `liftK`: to first order they LIFT the upper surface by `kCv2LobeLiftM` (900 m) x their
    strength, at most 0.7 of the cell's own thickness (uncapped, 300 m puffs took 300 m bumps and a clean cumulus
    field broke into shreds). Deep convection read ONLY the storm-scale volume (12x at the user's settings: 10 km
    lobes): now BIG lobes = the storm volume read at 2x its coordinates (5 / 2.6 / 1.3 km at 12x; an anchored
    coordinate may only be multiplied by an INTEGER, or the tile jumps when the observer crosses a period), x2.5, PLUS
    the cumulus-scale lobes on their crowns (`cu.w`); `bigL` gives the same to any convective cloud thicker than
    ~1.2-3.5 km at 0.7 (a tall cumulus was a cone stuck with equal 0.5-km balls: the cumulus volume's strongest octave
    is one size). Tried: the storm scale itself on thick cumulus (smooth blobs); a third, 4x finer fetch and a
    per-cell height variation for deep types by the cell volume's Perlin channel (219 registers each). The Cb towers (`cv2ColumnSigma`) take the same fine volume as a third pass of their one fetch
    site (`Af`, `kCv2ColFineM` 500 m x `cu2.x`; the storm coordinates x round(storm.x), no flow).
  - **Out-of-focus near clouds**: a weak cell never left the foot of the one density ramp (`form.w`). `phK` / `phP`:
    the ramp steepens with 1 / the cell's strength ABOUT the level the erosion cuts at (steepened about zero, every
    small cloud grew by the margin the erosion removes and a broken field closed into a sheet); the edge sharpening
    is x3 by an 8-m footprint (from 80 m) and the detail's fine octave gets +0.35 share there.
  - **Wisps** (`cu2.y`): erosion x1.7 and the sharpening released on weak cells (e < 0.05-0.3) and on the parts of
    a formed cloud the shape volume's Perlin channel picks. x2.5 erased most of a fair-weather field.
  - **Silver lining** (`cu2.z`, march): + 0.12 x HG(g 0.92) on single scattering, water cloud only (x (1 - thin)
    (1 - rain) (1 - soft): on the mid layer's sheets across the Sun it was a blown-out glare).
  - **Contour rings on anvil heads and tower walls** (the "wood grain", open since pass 14; shot 6's "odd texturing,
    a different material"): evenly spaced bands of light following the surface's height, a few per km. NOT the lobes
    (lobes 0 kept them), the shadow lattices, the light jitter, the sub-texel jitter, the density terms or the
    coarse-to-fine hand-off (each tested as a march variant, `harness_runs/iss13/m_*.comp`): they survived even a
    white-noise start jitter, vanished at 15-m view steps or with "Light march footprint" 0.08 (the user runs 0), and
    grew with more light steps. What removed them: **each coarse stride is 1.5-2.5 steps** (2 on average), by a
    per-step golden sequence off the ray's jitter (`cj` in the march's coarse branch), so the lattice is a random walk
    a few steps past the entry instead of one lattice offset once. Why the start jitter alone left a height-periodic
    bias was not explained. Also: the four per-visit offsets (sub-texel x 2, start jitter, light jitter) now step by
    the four R4 constants (the light jitter and the sub-texel x shared 0.7549: locked together for ever). With the
    high layer over a head a finer moire remains at 12 light steps (the cirrus fibres' shadow), faint at 7.
    cv2ShapeSmooth's terraces (zero slope at texel centres) were suspected and are NOT the cause (a two-tap
    half-texel average changed nothing: reverted).
  - **Decks:** the lobe lift applies to a deck's top too (up to ~100 m), and stratocumulus (a deck type that is half
    convective) takes x2.4 on "Lobes (3D)": its tops were smooth mounds.
  - **Registers:** with the shape terms the march went to 222 whatever was folded, reordered or switched off one at a
    time (31 partial-fold variants, six one-feature variants: all 220-224). What brought 128 back was REMOVING code:
    the pass-14 deep cores (`deepCol`, drawn only with "Cb columns" 0) and the pass-11 "old cones" (`legacy`, Tower
    top 0). Both are gone ("Tower top" is 0.3-0.95; with the columns off a storm is tall congestus). The 128 state is
    the one under MORE allocation pressure (its binary is ~306 KB against ~300 KB): folding post-fetch maths into
    pre-fetch scalars did not help. `harness_runs/iss13/regs.py` + `var.py` build text-substituted variants of
    clouds_v2.glsl and print each one's Register Count in one live batch.
  - **Cloud bottoms (issue _235159):** the "sky-coloured noise pattern" under an overcast at a low Sun was the lobe
    occlusion modulating ONLY the sky ambient (blue lobes, Sun-coloured creases). (1) the occlusion also dims the
    multiple-scattering sunlight toward the base (`f.msBright *= mix(min(occL, 1), 1, hf)`: on sunlit tops the creases
    went dirty grey); (2) `ambHue`: sky light that came down through
    thick cloud keeps its hue by the diffuse transmission 1 / (1 + 0.11 tauUp), the rest arrives white.
  - **Measured** (RTX 3070 Ti, 1600x900, the nine shots, GPU total): 19.8-22.5 ms after vs 21.2-24.8 before, 128
    registers. tstab at shot 5 (walk 6.4 km/s, pitch, rise; motion vs settled): 1.45 -> 1.88 (walk 1.79 -> 2.57, rise 1.84 -> 2.07,
    pitch 0.71 -> 1.00, floor 0.31 -> 0.47; the near-field edge tightening at x4 was 0.5 of the walk's loss, hence x3): more relief is more to get wrong while moving; no single slider recovers it (wisps, break-up, fine
    lobes, height variation off: 2.77-2.92; edge sharpness 1.5: 2.51). Looking down from 3 km .. 1500 km the placement
    is the same; from ~40 km a broken field shows more small cloud than before (weak cells are now opaque).
  - Open: a tower's flared head and the map-fed anvil are still smooth (their lobes are the user's "Cb head lobes");
    stratus at full coverage seen edge-on is still a slab; near clouds have nothing finer than the detail volume's
    24-m cells.
- **The blue-noise tile drawn over half-shadowed cloud, and shadows only near the eye (2026-10-12, user issues
  20261012_202341 / _195811; Sun 0.1-0.9 deg, light footprint 0, accumulation 0, history weight 0.02):**
  - **The resolve's still-view clamp box is 7x7 new samples** (`cloud_v2_resolve.comp`, `stillW`, motionPx < 0.05-0.5),
    not 3x3. All-or-nothing samples (a point sample lit or self-shadowed under a grazing Sun): a frame whose nine
    samples agree has a zero-width box and the history's mean was clamped to that value; which frames agree is a
    function of the 64-texel blue-noise tile, so the tile showed (autocorrelation 0.85 at 128 px, 9.3 levels of grain,
    16 levels darker than the mean). 7x7: 0.4 levels, the unclamped mean (5x5: 1.8; a slack of 0.75 x the box mean:
    4.8); cost within noise. Measure with a high-pass std + the lag-128 autocorrelation of a still capture.
    **With "Accumulation" 0 a reset still takes ~500 frames to converge at weight 0.02** (15 levels at 40 frames, 3 at
    240); at 32 it is 0.8 at 40 frames.
  - **Cloud samples read the SHAFT lattice** (`cv2SunShaftT`, cloud_v2_march.comp after the cascade's lookup, at the
    point where the light march ends; 128 registers) within its window (~14 km across the Sun), and **its rays march
    their own far field** (`kShaftFar` 16 quadratic steps from the window's sunward end to `cv2SunGridLen`, the same
    loop and call site) instead of starting from the cascade's value; it is baked whenever the cascade is on (was:
    only with "God rays" > 0). The cascade's cells are 0.8-1.4 km across the Sun's b axis and a grazing Sun's far
    shadow is a layer a few hundred metres deep: it read lit where the fine grid read full shadow, so a cloud was
    shadowed only inside the fine grid, i.e. within 3-6 km of the eye (isolated with march variants: cascade only /
    + shaft / the fine grid as a tube). Cost within noise (43-45 ms either way at the test view).
  - **Two RINGS of the shaft lattice** (`cv2SunShaft[2 x 128 x 128 x 32]`, flash buffer +2 MB; ring 1's lattice in
    `cv2.sunCasc6`, both cells and ring 1's knot spacing / bake weight in `sunCasc7`, UBO 896 -> 928; CPU
    `cv2Shaft2Lat*`): a ring reaches 56 cells from the eye across the Sun. Section "Cloud shadows & god rays":
    "Cloud shadow ring 1 cell (m)" (`clouds_v2.shadow_ring1_cell_m` 250, slot 286: ~14 km; also the god rays' reach),
    "ring 2 cell (m, 0 = off)" (`shadow_ring2_cell_m` 1000, slot 287: ~56 km), "ring 2 depth (x)"
    (`shadow_ring2_depth` 0.5, slot 288: its knot spacing along the Sun = ring 1's x the cells' ratio x this);
    `kCloudSliderSlots` 289. Lightning workgroups 15-16 bake ring 2 (dispatch 17; the field read at a quarter-cell
    footprint); a changed cell refills its ring (~40 frames). The march reads both through `cv2SunShaftRings` (ONE
    call site, a 2-iteration `[[dont_unroll]]` loop: 128 registers), inner ring first, then the cascade. GPU frame
    identical with ring 2 off / 1000 / 2000 m (29.9-30.0 ms). Beyond ring 2 it is still the cascade. Not judged in
    fast flight: a ring's ray is re-baked every 32 frames.
  - Harness: a script's `report load` path needs FORWARD slashes (the parser takes a backslash as an escape);
    `reports.py show` prints it so.
- **Review 27 (records 13-14):** the far-sea ripple's level blend drew RINGS round the observer (worst in motion): a level
  took a different offset as the incoming and the outgoing one (`seaRippleLevel(k)` is now one function of k), and the
  mix of two independent levels dipped to 0.71 contrast mid-way (renormalised). "Far sea ripple size (px)" (slot 256,
  `seaTune.w`, default 1.5 since 2026-10-20, was 5): the cell in along-view footprints; lower = finer ripples, more shimmer.
- **Review 28 (record 15):** **the sea's hard line at the wave range** was the reflection's lost-slope roughness
  (`seaRough`), set only inside the range (blend < 0.99): past it the roughness fell from ~seaState / 2 to 0 in a pixel,
  wherever the range was. It is computed from the footprint everywhere now; "Sea wave range fade" (slot 257,
  `ocean_wave_range_fade` 0.4, UBO `cityLod2.z`) is the share of the range the waves fade over. **Mesh flares sit on
  their glint:** a meshed satellite's sprite (point, bloom, the point-like glare) is placed at the Sun's image in its
  dominant lobe (d = s - 2 (s.n) n from the camera, clamped to the lobe as a square of its area along the group's in-plane body axes (2026-10-06; a circle of half the
  side put it 25 px off a Reflect mirror's Sun image from 500 m), weighted by its specular share;
  CPU double, `GpuMeshKeepList` entry w, octahedral), not its centre, so the flare no longer jumps when the mesh's own
  glints take the glare (12-36 px). Benchmark: Reflect Orbital #2700 from Fairbanks — the observer is 1.26 deg off its
  beam axis (it sees the mirror lobe's GGX tail), so up close the Sun's image is a FIXED point in the sky 1.26 deg from
  the satellite and the mirror grows past it (the "drift" is the optics; with time running it moves as the mirror
  tracks its site). The environment probes are not involved (the Sun glint is analytic; sharp reflections are per
  frame). The GPU-parity log's mismatches at ~170 m are the GPU's float satellite position (0.5 m = 0.17 deg there);
  0.02 mag at 6 km.
- **The low shape volume is read in a rotated frame (2026-10-04, `shapeRotA`, `anchorShapeA/StormA`):** a tiling 3D volume
  sliced by the sphere repeats wherever the local horizontal plane holds a short lattice direction of the tile (an axis,
  a face or body diagonal): the equator always holds z, and with the drift the meridians every 45 deg hold x, y or a
  diagonal - rows of the same puffs (user snapshot, 9.6 N 134.6 W, frame 0's clearance 0.007). The CPU keeps the identity
  where its clearance (min |up . v| over those directions, longer ones x1.3 / x1.6) is >= 0.05, else the clearest of
  three fixed rotations (harness `state` -> `clouds_v2.shape_frame`). A switch reshapes the lobes, never the
  placement. Tried: per-sample warps (Perlin, analytic sines, in place or not) all flipped cv2FieldLow to ~220
  registers (+5-9 ms); a cross-fade of two frames cost 0.7 ms idle. Only the low lobes and storm lobes use it; the
  detail erosion, cells, mid layer etc. still tile in the drifted ECEF frame.
- **`GpuCloudV2Params` mirrors `CloudV2Params`** (all vec4/mat4; offsetof asserts) — keep the order.
- Noise volumes are mip-mapped and read at the pixel footprint (`cv2Lod`). Lighting, shadow and beam
  samples pass detailAmt 0 (MEAN erosion) and the VIEW footprint; the coarse march passes -1 (none).
- All v2 screen images live in VK_IMAGE_LAYOUT_GENERAL (memory barriers only).
