# The Milky Way, skyglow, extinction, Sun, seeing, Moon, planets, stars

The natural sky's objects and the light pollution / extinction models they share.

Part of the subsystem notes (`docs/subsystems/`, map in `AGENTS.md` §4). Moved verbatim from
`CLAUDE.md` on 2026-11-03. Do not read this file whole: `python tools/outline.py docs/subsystems/sky-bodies.md`
lists its sections and topics with line numbers. New notes go next to the section they belong to.

## Subsystem: The Milky Way (2026-10-28, branch feat/milkyway-particles)

Sky binding 13, drawn in `sat_sky.frag`'s Milky Way block (post-tonemap, under the dark-sky gate, moonlight, the
pollution and beam domes, extinction, the Sun's glare, terrain / Moon / cloud occlusion — all unchanged). Full sky
pass and SKY_ENV only: SKY_LITE (Planetarium) and Potato have no Milky Way.
- **The texture**: the ESO / S. Brunier starless panorama, `8k_stars_milky_way.jpg`, 4096 x 2048 (0.088 deg a texel),
  8-bit sRGB JPEG, galactic coordinates with **lon = -l** (u = 0.5 + lon / 2pi; the LMC is at u 0.727), b up the rows.
  Linear luminance: empty sky ~0.0006-0.001 (8-bit values 1-3: every step is 30-50% of the light), faint band ~0.003,
  mid-band ~0.015, the core 0.05-0.13 (peak 100/255): ~2 decades, heavily quantised at the faint end, with JPEG 8 x 8
  blocks. It holds the Magellanic Clouds and the emission nebulae (magenta: Carina at u 0.70, the North America / Cygnus
  region at u 0.27-0.30) but **no Andromeda** (nothing above its noise at l 121, b -22).
- **Since 2026-10-28 it is RGBA16F** (`createMilkyWayBaked`): RGB the JPEG decoded to linear at load (exactly what the
  old R8G8B8A8_SRGB sampler returned; mips 2 x 2 box means in linear), A = `assets/textures/milky_way_glow.r16f`
  ("SLMA" + width, height, mips + float16 levels) from `tools/make_milky_way.py`: a denoised luminance (3 x 3 median
  of log luminance on noise-dominated texels, then a 1.3-texel Gaussian weighted by the texel's 8-bit step; totals
  kept per 64 x 64; a bilateral filter posterised, dropped). Without the file the plain sRGB JPEG and
  `milkyWayBaked` false. VRAM 43 -> 85 MB. Load ~+0.15 s (the CPU decode and mips).
- **The default view is the photograph** (`mwLook.w` = baked && `photometry.milky_way_zoom`): one fetch; the faint
  sky (A < 0.003 .. 0.012, to 0.04 above the air where the gain lifts the mid-tones) takes A's luminance (same fetch),
  the band its texels. FOV 90 old vs new (night vision 0): mean |diff| 0.05 levels, max 8, total light 1.0008.
- **Zoomed in** (`mwZoomDetail`, file scope, `#if !SKY_LITE && !SKY_ENV`): eased in as an input pixel goes from 0.6
  to 0.3 texel (`cloud.mwLook2.w` = 2 tan(fov/2) / skyScreenH, the INPUT pixel: a point narrower than it is hit or
  missed by the TAA jitter). The glow = A through a C1 fetch (texel fraction smoothstep-remapped: no bilinear
  diamonds) x the colour of mip 1.5; dust structure = local contrast about mip 3's mean ((A / A3)^(0.4 detail)) x
  three octaves of value noise (3 / 9 / 27 per texel, each faded in as it resolves) whose contrast is the dust depth
  ln(A3 / A); **star clouds** = a share S ("Milky Way star clouds") of that light in unresolved points
  (`mwStarLevel`): an iso-latitude lattice in (lon, lat) (row j's cell count floor(2 pi cos b / c): square cells, no
  pole pinch, the lon wrap at the anticentre exact), cells c0 x 2^k with c0 = texel / 64 and the level from 4 input
  pixels, two levels cross-faded through ONE call site (a second inlined copy was +9 KB of binary), the 2 x 2 nearest
  cells, 90% holding a point, weight 0.35 u^-0.65 (a power law, mean 1: most faint, a few bright), a stellar tint
  (luminance ~1), a Gaussian of sigma `MW_SIG` 0.9 input px windowed at 0.35-0.55 cells and divided by what the window
  keeps (1 - e^(-0.1035 / sg^2)). Its mean is the glow: zoom on vs off, total light 0.976-0.99 on the ground (the rest
  the display clipping a few points; from orbit at the space gain 0.88: 1.7% of pixels clip). S fades to 0 in the faint
  sky (A 0.003 -> 0.02: points there read as static) and on magenta texels (min(r, b) - g of the chroma 0.06 -> 0.2:
  the emission nebulae stay a glow; the dust-reddened bulge is red-orange and keeps its points). Still view: 0.0003
  levels mean frame to frame; a 0.6 deg/s pan at FOV 8, registered: 0.36 levels mean, p99.9 4.9 (0.6-px points: 7.8).
- **Andromeda** (all variants with the block, SKY_ENV too): analytic, centre / major axis (PA 38) constants in the
  panorama's frame, light 9.6e-6 lum sr = the panorama's measured LMC (1.0e-4) x 10^(-0.4 (3.44 - 0.9)); 30% an
  exponential bulge (0.04 deg, q 0.8, warm), 70% a disk (0.39 deg, q 0.3); both widened by 0.6 of the pixel (flux kept:
  the cusp point-sampled sparkled). Within 4 deg only.
- **The look** (`cloud.mwLook`, GpuCloudParams 12256 -> 12288): blended by atmFracSky (ground 1, above 100 km 0).
  Ground: x "Milky Way gain (ground)" 1, the colour pulled toward a cool grey of the same luminance by "Milky Way night
  vision" 0.5 (rod vision: the faint blue-grey band). Above the air: x "Milky Way gain (space)" 2.5 and a power
  "Milky Way contrast (space)" 1.25 on the SMOOTH glow's luminance about 0.01 (`mwLum`; on the pixel's it lifted the
  zoomed points up to 2x), full colour. "Milky Way zoom detail" 0.6, "Milky Way star clouds" 0.7. Photometry tab slots
  39-44, keys `photometry.milky_way_*`; `photometry.milky_way_zoom` (no UI) = the A/B (false: the old single fetch,
  no denoise, no star clouds; Andromeda and the look stay).
- **The star clouds' tunables** (2026-10-28, issue 20261028_191142; Photometry "MILKY WAY STAR CLOUDS (ZOOMED IN)", slots
  45-53, `cloud.mwStar0-2`, UBO 12288 -> 12336): `photometry.milky_way_star_zoom_from` 0.6 / `_zoom_full` 0.3 (input px
  per texel), `_fill` 0.9, `_size_px` 0.9, `_spacing_px` 4 (cell), `_brightness_spread` 0.65 (the power law's a),
  `_faint_from` 0.003 / `_faint_full` 0.02, `_nebula_exclusion` 1. Defaults = the old constants (the report's view vs
  the old shader: 0.003 levels mean). SKY_LITE is 230272 of its 230400 B after it: the next UBO field needs room.
- **No seam at the anticentre** (2026-10-28, issues 20261028_190641 / _191529): the main fetch is `textureGrad` with the
  longitude wrap taken out of the derivatives; the implicit level took the coarsest mip at u 0/1 (a thin dark line).
- **Cost** (RTX 3070 Ti, 1600x900, interleaved with the old shader in one app): sky pass FOV 90 +0.0 (6.58 vs 6.21 /
  6.56), FOV 8 +0.25 ms (1.29 -> 1.55; every pixel zoomed), ISS +0.07; sky TAA 168 registers both, binary 937 -> 950 KB;
  SKY_LITE SPIR-V +88 B (the UBO). Scripts and captures: `harness_runs/mw_tools/` (azel.py: a target's az / el for a
  time and place; energy.py: linear-light ratios of two captures), `harness_runs/mw_live2/` (untracked).
- Open: the star clouds are drawn
  in the sky pass, so they ride the sky TAA (the catalogue stars are drawn after it); M33 and other faint galaxies are
  not drawn; the panorama's colours are a long-exposure photograph's (the night-vision term is the only eye model).

## Subsystem: Light Pollution Dome

**The skyglow model (2026-10-28, branch fix/darksky-gate).** One physical sky brightness for every night-sky feature:
the ARTIFICIAL sky brightness at the observer as the ratio of artificial to natural sky (Falchi et al. 2016's
convention; natural = `kSkyMagPristine` 22.0 mag/arcsec^2), so the sky is 22 - 2.5 log10(1 + ratio). Code:
`computeSkyglow` / `updateLightPollutionDome` / `skyglowRatioAt` (SatelliteSim.cpp), `darkSkyArtRatio` /
`darkSkySkyMag` (shaders/include/darksky.glsl), `tools/make_skyglow_sources.py` -> `assets/textures/skyglow_sources.bin`.
- **Sources** (the tool, offline): every texel of the 8K night map (~4.9 km) whose light, decoded as linear
  (L/255)^2.2 - (16/255)^2.2 (the map's blue base, luminance 2.5-16.5 worldwide, removed), passes 1e-3, x (1 + 5 f) with
  f the share of CLIPPED texels (L > 235) in the 5 x 5 round it — the map is tone-mapped and clips every town's core, so
  a small town and a megacity's core read alike; the clipped neighbours restore the difference — x its area (km^2),
  with its DEM height. 394,634 sources, 3.2 MB, tiled 64 x 64 texels. Re-run the tool after changing the night map.
- **Propagation** (CPU, every source within 300 km): Walker's law (d^2 + 4^2)^-1.25 (d in km; d0 = the texel's own
  near field), x e^(-d / 120 km) (the long paths' extinction), x the observer's height above the SOURCE's ground,
  0.2 e^(-dh / 8 km) + 0.8 e^(-dh / 1.2 km) (the glow is scattered mostly in the boundary layer: a mountain observatory
  above its island's towns sees a quarter of their glow, an airliner a few percent, nothing from 60 km up), x
  `kSkyglowK` 4.37. Z = the sum (the zenith ratio). Each source also puts d / (d + 15 km) of its share (a source
  overhead lights the whole sky alike, a distant one is a dome on its horizon) into the sector of its bearing (16 x,
  linear between sector centres), blurred +-45 deg: D[16], the directional excess toward each horizon.
- **In a direction** (darkSkyArtRatio, its CPU mirror skyglowRatioAt): ratio = Z x iso + D(az) x dome, iso =
  (0.4 + 0.6 ef) / 0.5556 (1 at the zenith, 1.8 at the horizon: the air path), dome = (ef - 0.259) / 0.741 (0 at the
  zenith, so the zenith has no azimuth seam; 1 at the horizon), ef = 0.35 / (z + 0.35).
- **Recomputed** when the eye moves 200 m or 20 m in height (5-30k sources, 0.01-0.2 ms; nothing in a still view),
  eased (`mwFadeInTimeS` toward a darker sky, `mwFadeOutTimeS` toward a brighter one), x "Skyglow (x calibrated)"
  (`photometry.skyglow_gain`, 1 = calibrated; Photometry slot 6, where "Pollution gain" was).
- **lightDomeBuf** (36 floats, mirrored whole): [0,16) `lightDomeAz` = the legacy point-source wash-out R / (R + 15), R
  the ratio toward the sector's horizon (half at ~19 mag/arcsec^2), read by sat_flare.comp, planets, comets' heads and
  meteors through their old `domeVal = clamp(domeAz x (0.4 + 0.6 elevFalloff))` x a max dim; [16,32) `lightDomeEasedAz`
  = D x gain; [32] `skyglowInfo.x` = Z x gain. Readers of the physical model declare `vec4 skyglowInfo` after the two
  arrays (sat_sky.frag, cloud_march.comp, comet_composite.frag); sat_flare.comp declares only the first array.
- **Consumers of the physical sky**: the Milky Way, zodiacal light and the ocean's Milky Way reflection (sat_sky.frag),
  the aurora (cloud_march.comp: `darkSkySkyMagLinear` is now the same function), comet tails (`darkSkyPollutionMagPhys`),
  the stars' naked-eye limit (updateStars: `Lp (1 + skyglowRatioAt(bearing, z))`; the old linear dome dim on stars is
  applied only with `stars.sky_limit` off — it double-counted the city glow, and at "Pollution gain" 25 dimmed stars 99%
  near any light).
- **Calibration** (2026-10-28; harness `state` -> `sky_quality`, run at a moonless astronomical night 2036-11-19; the
  references are published SQM / V zenith values or, marked ~, estimates for the Bortle class: +-0.3 mag at best):

  | Site | Ref | Before (Milky Way gate / aurora + stars) | After |
  |---|---|---|---|
  | Paranal | 21.8 | 19.51 / 22.00 | 22.00 |
  | NamibRand | 21.9 | 22.00 / 22.00 | 22.00 |
  | La Palma, Roque de los Muchachos | 21.7 | 13.78 / 21.86 | 21.63 |
  | Mauna Kea | 21.7 | 15.82 / 21.98 | 21.99 |
  | Death Valley | 21.6 | 14.87 / 21.96 | 21.90 |
  | Cherry Springs | 21.6 | 14.30 / 21.94 | 21.88 |
  | Kitt Peak | 21.6 | 15.51 / 21.92 | 21.91 |
  | Palomar | ~21.0 | 13.64 / 21.57 | 21.44 |
  | Santiago SW field (-33.95, -71.45) | ~21.3 | 15.28 / 21.85 | 21.84 |
  | Lick | ~20.3 | 13.64 / 21.05 | 20.51 |
  | Flagstaff (lighting code: excluded) | ~20.4 | 13.64 / 21.75 | 19.28 |
  | Mount Wilson | ~19.2 | 13.64 / 20.98 | 18.93 |
  | Boulder | ~18.8 | 13.64 / 21.23 | 18.60 |
  | Santiago centre | ~17.3 | 13.64 / 20.58 | 17.55 |
  | Los Angeles downtown | 17.0 | 13.64 / 19.48 | 17.10 |
  | Manhattan | 16.8 | 13.64 / 18.86 | 17.09 |
  | London centre | 17.3 | 13.64 / 18.86 | 17.23 |

  rms 0.27 mag (Flagstaff excluded), worst 0.54 (the Santiago field, whose reference is the least certain); the Milky
  Way's gate was 5.5 mag rms off, the aurora / stars' linear model 1.5 (cities ~2 mag too dark). Dark sites read 0.1-0.3
  too dark: the natural sky varies (airglow, the solar cycle) and is fixed at 22.0. Naked-eye limit at the zenith:
  7.8 pristine, 7.2 Teide / Arizona (B3), 5.5-5.7 Ames / Ithaca (B6), 3.2 downtown LA. The Kp 9 aurora views of issue
  20261014_193231 (BC, Iowa farm, Arizona) read 21.7 / 21.7 / 21.0 and look as before.
- **Known residuals**: the map does not know lighting codes or shielding (Flagstaff 1.1 too bright; La Palma's ORM is
  right only through the altitude term), clips everything brighter than a suburb, and smears every light over ~5 km;
  the sky pass's own rendered city glow (the orange in-scatter, `accumCity`) is a separate model and is not calibrated
  to this one.
- **What it replaced** (the reason the Milky Way vanished near any village): the dome sampled a 37-km blur of the map
  at six radii per sector with a weighted MAX, compressed d / (d + 0.08), x "Pollution gain" 25; the Milky Way and
  zodiacal light then log-mapped it from 1e-6 (`mwPollutionThresholdLo` 0) to 0.075 toward an artistic 13 mag/arcsec^2
  (`darkSkyCityMag`): a raw 1e-3 was already 61% of the way to "city", so Paranal read 19.5 mag/arcsec^2 and La
  Palma's observatory 13.8. Those three settings and `light_pollution_gain` are gone (not read). Before that the
  dome was a single scalar at the observer's position (session 26 made it 16 sectors).

## Subsystem: Atmospheric Extinction

Session 26 follow-up: the light-pollution dome's `elevFalloff` was, until this was added, the
*only* term anywhere that varied a star's or satellite's brightness by its own viewing elevation —
there was no real horizon-dimming baseline, which is part of why the dome's directional noise (see
above) read as unsubtle: nothing else was smoothly dimming things toward the horizon for it to
modulate on top of.

**Formula (2026-09-23) — the air on the actual line of sight**, one function for every consumer:
`shaders/include/atmosphere.glsl` (`atmExtinctionMag(p, d, L, k)`) and its C++ mirror in
`SatPhotometry.cpp` (`updateStars()`/`updatePlanets()`). Two exponential components — molecular
(8 km scale height, ozone folded in) and aerosol haze (1.2 km) — splitting the sea-level zenith
extinction k 60/40. Each column is the Chapman function, `Ch ≈ sqrt(πx/2)·erfcx(sqrt(x/2)·cos χ)`
(x = r/H; `erfcx` by a one-parameter fit, 0.33% max error), with the tangent-point form for rays
that dip before climbing out and a segment form for finite targets (a satellite at its range, a
planet at its distance; a satellite silhouetted against Earth from orbit gets only the segment that
reaches it). `SatModelTool --selftest` checks it against a brute-force ray integral: 0.3% over
observers from sea level to 400 km, elevations +90° to -19°. Consumers: `sat_flare.comp`
(satellites), stars, planets, `sat_sky.frag` (Milky Way, zodiacal light, and the aurora/Milky Way
ocean reflections — from the water along the reflected ray), `cloud_march.comp` (aurora, beam ray).

It is **absolute** — the zenith from sea level loses k magnitudes, orbit loses nothing — where the
old one was relative to the zenith. It replaced Kasten & Young's sea-level airmass times
`exp(-tangentAlt / 80 km)`: a 10× too slow thinning with height (a 4 km mountain kept 95% of
sea-level extinction, really ~35%; an aircraft at 10 km 88%, really ~15%). `rayTangentAltM`, the
gate it needed, is gone. At k = 0.25: sea level 0.25 / 1.41 / 14.4 mag (zenith / 10° / horizon —
the horizon is above Kasten & Young's ~9.5 because low haze stacks up along grazing paths), 4 km
0.09 / 0.53 / 3.5, 10 km 0.04 / 0.24 / 1.5.

Satellites' day/moon sky dimming (`sat_flare.comp` `atmFrac`) now uses the stars' altitude gate —
full through the flyable atmosphere, fading over 40-100 km — instead of `exp(-h / 80 km)`, so stars
and satellites in the same sky are dimmed alike.

**Tunable:** `extinctionCoeff` — the sea-level ZENITH extinction in magnitudes (V band, clear sky:
~0.2-0.3; the user-tuned default is 0.18 since 2026-10-20, was 0.079), settings slider "Extinction". Reuses `SatFlarePC`'s
`pad2` slot (the one freed by `lightPollution`'s move to `lightDomeBuf`); stars read the same C++
member directly.

`MIRROR_BOOST = 300` — peak multiplier for near-perfect mirror alignment. `mirrorExp = max(specExp0 × 300, 8000)` gives sub-degree angular width (matches solar disc ~0.26°).

---

## Subsystem: The Sun as an object (2026-10-26, issues 20261026_192501, 20261026_203414)

sat_sky.frag's sun block (after the tonemap, display units), full variants only (`SKY_SUN_SHADOW`; SKY_LITE / SKY_ENV keep
the old hand-tinted disc). It was a fixed colour (reddened by its height above the limb) with full glare whatever the
air did to it: "a half faded fuzzball" low down.
- **Refraction, mirages and turbulence: ONE ray tracer** (`Refraction.h/.cpp`, app-independent; `SatelliteSimRefraction.cpp`;
  issue 20261026_203414 "Sun refraction mk 2"). `refrProfileAt` builds the air's profile at the eye, a pure function of
  place and sim time: a standard atmosphere through the air temperature at the eye (`cv2EyeTempC`), refractivity
  N = 77.6 P/T with Edlen dispersion (R / G / B at 610 / 545 / 465 nm: 0.9955 / 1.0005 / 1.0097 of 550 nm — the old
  0.985 / 1.018 was ~3x physical); the SURFACE LAYER over the surface (a log profile, z0 2e-4 m, zRef 10 m): over the sea
  the water 0.2-1.6 K warmer than the air (smooth value noise over 3-deg cells and 12-h steps; 1 day in 5 a colder sea,
  -0.3..-1.8 K: looming), over land the Sun's heating by day and -2 K after dusk, x (1 + 0.25 sin(t/83 s) + 0.12
  sin(t/29 s)) (the convection breathing); an ELEVATED INVERSION on ~45% of days at sea (30% on land), 3-400 m up
  (median ~35), 5-35 m thick, 0.5-4 K, riding 170-s / 53-s gravity waves; Cn^2 = (2e-15 + 6e-15 dT^2) z^-4/3 e^(-z/1 km)
  + Hufnagel-Valley's free troposphere. `refr::build` traces 512 APPARENT elevations from the eye (e = eC + c sinh(alpha
  (u - u0)): 3" steps at the horizon, ~30" at 0.5 deg, the table from the geometric dip - 0.34 deg to + 4 deg), 3
  channels each, 2D in the eye's vertical plane (dd/ds = grad ln n - (.) d, midpoint steps sized by the profile's
  local scale and the ray's curvature), to the top of the air (100 km), the sea / ground (`kHit`) or a duct (4000 km
  IN the air: counted from the eye until 2026-10-27, so from orbit the straight leg to the air's top used most of it and
  every limb-grazing ray read as ducted — issue 20261027_185311, the Sun cut flat 0.8 deg above the drawn limb from 790 km).
  Per entry: the refraction per channel, the Rayleigh / Mie / ozone / haze-near-clouds columns along the BENT path,
  the angle-of-arrival rms (2.91 D^-1/3 int Cn^2 ds, D 1 cm) and the scintillation sigma_I (sqrt(1 - e^(-sigma_R^2)),
  Rytov 2.25 k^(7/6) int Cn^2 s^(5/6) ds). Above the table: x Saemundsson's shape, the jitter as 1 / sin e, Rytov as
  sin(e)^(-11/6) (the zenith ray's). Built on a worker (`std::async`, 6 threads, 7-11 ms), 10 times a second while the
  Sun is within ~4.5 deg of the limb, once a second otherwise, at once on a 2% change of eye height; the first table on
  the frame itself. Shipped in GpuGroundBeams' tail (`GpuRefraction`, 16 KB; sat_sky.frag sits at its storage-buffer
  budget). CPU derives (`refrDerive`): the primary image's inverse map (true -> apparent, for stars), the Sun centre's
  refraction and the visible share of its disc over every image (glare).
- **The disc** maps each pixel's apparent elevation, jittered by the turbulence (a value-noise field ~40" x 12" cells,
  drifting and boiling with sim time, x the table's rms), through the table per channel: flattened, the inferior
  mirage's inverted image under a warm sea (the Etruscan vase merging into the Omega, then the "flat lid"), slices under
  an inversion, a green rim (the green flash when the red has set). Over the far sea (no terrain / mesh / opaque cloud)
  the table, not the drawn horizon, decides: the inverted image shows below the drawn sea line. The OLD pair (Saemundsson
  above the horizontal, 0.01003 (2 e^(-ht/8km) - e^(-h/8km)) at the tangent height below it with a 0.8 rate cap) disagreed
  at the horizontal (0.48 vs 0.57 deg) and the cap halved the bending just below it: every low Sun's disc was cut flat at
  the horizontal with a step (the report's "Cut" / "Odd section").
- **Light:** each disc pixel's transmittance from the table's columns along its bent path (Rayleigh + Mie x 1.1 + ozone +
  "Haze near clouds" x the weather 80 km toward the Sun, as the sky loop's) x the cloud / plume composite `cloudB.rgb` in
  colour; L = exposure x 2.3e5 (4 pi / Omega_sun / (1 - u/3): the disc centre in the sky's units, mean-1 phases) x
  "Sun disc gain" x T x limb darkening (u 0.6) x sunspots, shown per channel 1 - e^(-L/250). It was 300 x luminance with
  the hue kept (~1/600 of physical): a horizon Sun was no brighter than the sky round it — the report's "dim, sharp
  reddened disc". **What reddens it** (asked in the report): only the columns (Rayleigh + the Mie aerosol, 1.2-km scale
  height, zenith AOD 0.03 x `atmos_mie_gain`, + the haze near clouds) — not the fog / dust layers: the composite's
  transmittance toward the Sun was 1.0 in the report's view and fog, dust and haze off moved the disc 0-7 levels.
- **Glare and corona** (veiling light) about the Sun AS SEEN, x the visible share, x 1 - exp(-60 x the centre's
  transmittance luminance) (was 10, with the 300 disc). Added over EVERYTHING in front (clouds, the Earth, terrain,
  the sea; 2026-10-28, issue 20261028_190911 "Sun glow below clouds from orbit": from 1625 km each far-layer cloud
  multiplied the halo by its own transmittance, black holes in the glow): they follow only the light reaching the eye
  from the Sun — the cloud composite (half-res + far layer) and the scene depth at the Sun's own screen place (5 taps
  over its drawn disc; a surface counts only if nearer than the sea-level sphere, so the horizon is not counted twice
  with the visible share), the eye's `sunCloudT`, the visible share. Taps only within 34 deg of the Sun (cost within
  noise). A ridge or deck hiding the Sun removes the halo everywhere; the eclipse corona keeps the per-pixel cloud.
- **Stars and planets** (`updateStars`, `updatePlanets`, `refractEnu`): placed at the primary image's apparent elevation
  (a star just under the true horizon shows; below the apparent horizon it does not), and their record's `meshPx` carries
  1 + floor(aoa urad) + 0.99 sigma_I: star_point.vert jitters the image by the aoa (sines, per star) and twinkles by
  sigma_I / 0.55 (the old artistic airmass ramp only without a table). Planets: jitter, no twinkle. Satellites are not
  refracted yet.
- **The Moon** (2026-10-27, issue 20261027_184332 "Moon Refraction"): its disc goes through the same table as the Sun's
  (sat_sky.frag moon block, full variants only): each pixel's apparent elevation, jittered by the same turbulence
  field, maps per channel to the true direction (flattened low, raised past the limb from orbit, dispersion rims); a
  channel whose ray ends on the surface sees nothing; the shading follows the first channel that hits (green first);
  the extinction is the table's bent-path columns + the haze near clouds toward the Moon (the straight view ray's above
  the table). Its glow (corona, halo) centres on the Moon AS SEEN (`GpuRefraction::spotInfo.z` = the centre's
  refraction, `fillSunspots`), and the stars' Moon cull (`PointDrawPC::moonDirENU`) takes the refracted direction.
  SKY_LITE / SKY_ENV keep the straight ray (SKY_LITE's budget: 229940 of 230400 B). Cost (2026-10-27, the matrix suite:
  the sky pass +0.7..+0.9 ms in every daytime cell with the Moon up, sky TAA 913 -> 934 KB): the lookups, noise and bent
  rays ran for EVERY sky pixel; now only within the disc + 0.035 rad (-0.3 ms), and the table lookup has one call site
  (a 2-pass loop, -3 KB). ~0.2-0.4 ms over the pre-merge sky remains (code size: the block compiled out reads as the
  old cost; nothing per pixel runs away from the Moon).
- **Cloud in front of the Sun** (2026-10-27, issue 20261027_182140 "Sun occlusion on far clouds": "some clouds appear to
  render behind the sun"): the disc is hidden by cloud through its TRANSMITTANCE (a 4x4-texel tent of the half-res
  composite over the disc, `cloudTs`), fading out over 0.1 -> 0.02 (`cloudKnown`: the march stops at T 0.01, so below
  ~0.02 means "optically thick"); it was
  a binary gate on the composite's >= 90% opacity flag per half-res texel (a speckle of disc and cloud along a thick
  cloud's edge) with the disc drawn at full brightness through everything thinner. And the cloud between gets its
  forward DIFFRACTION glow (the aureole: droplets put about half of what they scatter into a ~1 deg peak the march's
  HG lobes do not hold): pi L / E = 0.25 / sigma^2 x tau e^(-tau) x a Gaussian in the angle from the Sun as seen
  (sigma^2 = 0.012^2 + rD^2 / 2), x the Sun's air transmittance, visible share, eclipse share and the eye's Sun-line
  cloud transmittance beyond the march, x `cloudKnown`; combined with the disc as 1 - (1 - disc)(1 - aureole). A thin
  cloud across the Sun glows round it with the disc showing through (in front, not behind). PHYSICS NOTE: the disc is
  ~2.3e5 x a sunlit white cloud, so it still shows (saturated) through any cloud thinner than tau ~8, and the aureole is
  only ~1/400 of the disc: at the report's sunset (the Sun's air transmittance ~1e-2) it is a faint glow and the view
  looks much as before (the grain along the thick band's top is the cloud composite's own at 1.2 deg FOV). By day (air transmittance ~0.8) the model gives a white glow ~1.5 deg across round a thinly covered Sun (not yet framed in a test).
- **The sea glint sparkles are FACETS ON THE SEA** (2026-10-27, issue 20261027_180609: the first cut hashed the screen
  pixel and a 6-per-second time bin, "a screen-space mess ... on a timer" at every altitude). The Beckmann lobe is the
  pixel's mean; the facets that mirror the Sun cover fs = glint / (F x 1.8e5) of the sea. A world-fixed lattice on the
  wave field's coordinates (posM, read at the UNJITTERED sky-TAA ray's sea point), turned to the view azimuth in
  22.5-deg steps (two orientations cross-faded) with cells 0.25 m x 2^k about two pixel footprints along and across the
  view (two levels blended); each cell is one facet whose unresolved slope (N(0, s2 / 2) per axis, the glint's own s2)
  turns continuously at its own rate (two Gaussians rotated, 0.6-2.4 rad/s). It flashes while that slope is within r of
  the slope mirroring the Sun from the resolved wave normal, r^2 = q s2 exp(m^2 / s2) so P = q = fs x the facets the
  cell holds (C_u C_v / 0.09 m^2, at least 150), carrying glint / q; 0.4 of the light stays the smooth path. A sparkle
  stays on its patch of water when the camera moves (a 0.1-deg pan: 43% of sparkle pixels coincide at the world shift,
  4% at the screen position; the old 15% / 24%), and from high up q -> 1, the mean (at 3 km the old speckle is gone).
  Cost within noise. The `gSeaSpark` TAA flag is kept, but sky_taa.comp takes a sparkle whole only while SIM TIME RUNS
  (`eyeDelta.w` 1 or 2; the CPU sends 1.25 for "camera moved, time frozen", 3 for still + frozen): the mirror slope m is
  the pixel's own jittered wave normal's, so with time frozen whether a pixel is in a facet's flash changed with the
  jitter phase, and taken whole every sparkle flickered (matrix suite, 2026-10-27: sea_20m still noise 0.23 -> 0.59,
  storm_gnd look 0.98 -> 1.66 mean / 0.02 -> 0.57% past 16; after: 0.21 / 1.23 and 0.05%). The flash's edge is soft
  (1 - smoothstep(0.6, 1.4) of (d / r)^2, the same area as the disc) and only its core (inside 0.6) is flagged, so with
  time running the edges are averaged and only cores flash whole. storm_gnd look's remaining +0.25 over the old screen
  hash is the facets themselves (a frozen screen hash was identical in the moving frame and its settled reference). A
  cell lattice does not divide the field's period: the pattern
  re-hashes when the CPU wraps the world offset (every ~21-28 km of travel).
  **At the sea surface (2026-10-27, the user: at ~0 m "the glitter falls apart and looks pixelated, and the water has a
  clipping issue")**: (1) cells follow the footprint below 0.25 m too (to 1/32 of it), holding the 150 facets' density
  (150 (C / 0.25 m)^2): floored at 0.25 m a cell was tens of pixels a few metres off and a flash lit it whole (white
  quads); (2) the flash disc is capped where the slope pdf is near-constant over it (r <= sigma, r <= s2 / (2 |m|)) and the
  share of the cell's energy that cap removes stays smooth (wL = flash / q + 1 - pL / q): out in the tail (m^2 > ~2 s2)
  r^2 = q s2 e^(m^2 / s2) outgrew the distribution, nearly every cell flashed at 1 / q and the glint there was up to 50x
  its mean (white cells off the glitter path); (3) the WAVES' eye (the trace, the normal, the glints; not the sky, the air
  or the depth) stands at least 1 m over 0.8 of the field's highest crest for the eye's sea state (static: nothing for the
  temporal passes to reproject), and the trace's bracket never starts behind the eye — the field stands on the sea-level
  sphere (0 .. ~5 m x the sea state) and the 2-m eye was inside it, crests as walls through the camera. Not in SKY_LITE.
  Normal altitudes (20 m, 300 m) unchanged within the run-to-run noise (single captures of one config differ by more
  than old vs new: the waves are not frame-deterministic); GPU within noise. The sea's own sun
  direction is still the TRUE Sun (not refracted).
- Settings (`refraction.*`, no UI yet): `enabled` (true), `mirage` (1, x the temperature steps), `turbulence` (1, x Cn^2),
  `dispersion` (1 = physical), `sun_disc_gain` (1); overrides `force_sea_air_dt` (< -50 auto), `force_inversion_m` (< 0
  auto, 0 none), `force_inversion_dt` / `_dz`. Harness `refraction [rebuild]` (profile, horizon, Sun refraction and share,
  samples, the app -> true map), `state` -> `refraction`. Views + videos: `harness_runs/refr/v2.satcmd`, `v3.satcmd`
  (gitignored; the report's place at 10x, an inversion under a 97-m eye).
- **Cost** (RTX 3070 Ti, 1920x1009, the report's shot 2 view, old vs new SKY_TAA SPV interleaved with `shaders reload
  sky_taa=`): sky pass 21.42 / 21.34 -> 21.44 / 21.40 ms (+0.05), 168 registers both, binary 871 -> 885 KB; the table
  7-13 ms on a worker. SKY_LITE unchanged (230024 B).

## Subsystem: Seeing — the turbulence warp and the twinkle clock (2026-10-28; the eddy band 2026-11-03)

Issues 20261028_191408 (twinkle far too fast), 20261028_193115 (turbulence should distort the whole picture),
20261103_191106 ("Turbulence": big and chunky, a wiggly sea horizon, no regard for time and position, no star flicker).
`SatelliteSimTurbulence.cpp`, `shaders/turb_warp.frag` (+ `taa_fullscreen.vert`), star_point.vert's twinkle.
- **Three sliders, Sky tab "Seeing (twinkle, turbulence)"**: "Turbulence strength (x)" = `refraction.turbulence`
  (x Cn^2 in the refraction profile: the warp's rms goes as its square root, the twinkle's sigma_R^2 linearly; 0 = no
  warp pass at all and no twinkle), "Turbulence cell size (x)" = `refraction.cell_size` (0.25-4, shifts the eddy band),
  "Turbulence speed (x)" = `refraction.speed` (0-4, how fast the warp and the twinkle evolve). Slider ids 411 / 440 /
  441 (`buildCloudSliderRows`). `photometry.star_twinkle_rate` and `photometry.turbulence_warp` (an rms exaggeration,
  0-20) are retired: no longer read or written; at 5.5 the second made the user's report view's chunky wobble.
- **Sim time, not real time.** `buildUI` advances `turbClock_` by the sim clock's change x speed (a step capped at
  +-1 s: time warp and `time set` jumps decorrelate the field anyway) and `twinkleClock_` by the same x
  `kTwinkleRate` 0.3 (the old rate's default: the twinkle's 3.7 / 7.1 / 11.3 Hz become 1.1-3.4 Hz), mod 1000 s.
  Paused = frozen (the clock does not move; the report view's Sun box changes 0.05 levels over 2 s, the same as with
  the warp off), reverse time runs it backwards. `PointDrawPC::noTwinkle` is 0 on the stars, 1 on the planets.
- **The per-star phase is hashed from the star's INDEX** (`gl_VertexIndex`, 2026-10-28, issue 20261028_220009). A hash
  of its moving ENU direction gave a new phase each frame (white-noise flicker at the frame rate).
- **Point sprites average their PSF over the pixel** (`pointPixelGauss`, point_style.glsl): a star drifting with the sky
  no longer pulses with its sub-pixel phase.
- **The warp** runs after the main pass, before the UI, inside the pre-UI hook (`recordPreUiCopy`, SatelliteSimVideo.cpp;
  the HQ photo's frames too): the swapchain image is copied to `turbFrameImg`, then a fullscreen draw in
  `renderPassLoad` resamples it (5-tap Catmull-Rom) and blurs it. The clean video copy / settle probe take the warped frame.
- **Amplitude** per pixel = the refraction table's aoa rms at its apparent elevation (64-entry table in the UBO; 2.91
  D^-1/3 int Cn^2 ds, D 1 cm) x sqrt(1 - exp(-d / aoaS)) (d = scene depth / the cloud composite's distance; aoaS the
  Cn^2-weighted mean distance along the ray) x a fade from 30 to 80 km of eye altitude; 0.9 / 1.2 horizontal /
  vertical near the horizon. Report view (sea, 14 m, Sun at the horizon): 7.8" rms (0.79 px at 9.8"/px); the table's
  aoaS is 4.7 km for the sea 0.2 deg down and ~95 km for a sky ray at the horizon (it grazes ~700 m up).
- **The eddy band**: a Kolmogorov tilt field has about equal variance per octave of eddy size from the aperture D (1 cm)
  to the outer scale L0 = 0.4 x the height of the air aoaS out along the ray, 1-50 m (site medians ~20 m), x cell
  size; seen from aoaS away that is the angular band D / aoaS .. L0 / aoaS. Six octaves of vector value noise on
  fixed absolute angular cells (1 urad x 2^k, finest >= 1.5 px, faded in over 1.5-3 px and the coarsest out over
  48-96 px) carry the band's variance above ~2 px as displacement, spread by each octave's overlap with the band (the
  coarsest stands for the band above it) and each **capped at 1/8 of its cell** (a field displaced by more than ~1/4
  of its cell folds: the 2026-10-28 cut grew one cell to 4x the displacement instead, the "big and chunky" look). The
  rest of the variance (sub-pixel eddies: the seeing disc) is a blur, a ring of six bilinear taps (3/7 r^2 per axis),
  at most 1.5 px. Measured at the report view (horizon line, per-2-column vertical shift vs strength 0): 0.32 px
  (3.2") rms, p99 0.5 px; it was 3.9 px (38") rms, p99 11-12 px with the old field at the user's 5.5x. Strength 5 /
  20: 0.65 / 2.25 px rms. The Moon at 2.3"/px, 16 deg: 0.32 px rms in all (no visible limb ripple, as in a real
  0.7" seeing).
- **The field is in the air**: octave k's lattice is in ECEF metres on a sphere L = 100 / 300 / 900 m (by k mod 3)
  around the eye, cell l_k = theta_k L; the CPU anchors the forward ray's point (eye / l + F / theta + drift) in double
  and wraps it mod 4096 before it reaches float. Turning the camera keeps the field on the scene (yaw 0.1 deg: the
  horizon's displacement profile correlates r 0.34-0.49 at the scene's 37-px shift, 0.7-0.9 for a repeat at one pose);
  a 1-m move of the eye redraws it (r 0.05-0.12 at any shift), as two eyes a metre apart (>> r0) see independent motion.
  Drift: each octave's lattice moves 0.8 cells / s x speed along a hashed 3D direction (across the view: frozen flow;
  along it: boiling); the physical crossing rates v / l are 10-100 Hz at these scales, above the frame rate (the blur
  stands for them). **Never make an octave's cell vary with the pixel or the elevation**: amplitudes may vary per pixel,
  coordinates may not (a varying scale shears a ~1e4-cell coordinate).
- **Twinkle** amplitude = the table's scintillation index (Rytov 2.25 k^(7/6) int Cn^2 s^(5/6) ds, saturated
  sqrt(1 - e^(-sigma_R^2)); 0.54 at 45 deg, 0.99 at 10 deg, 1 lower in the report place) / 0.55 x the sum of three
  sines (clamped at 0). Measured at shot 3's place, catalogue stars at a 10-deg field, linear flux std / mean over 40
  frames of running time: 0.37 at 60 deg, 0.63 at 16 deg, 0.71 at 6 deg; 0.003 at strength 0. Brightness only (no
  chromatic twinkle yet).
- **Runs only when it shows**: the largest 3-sigma displacement in view >= 0.3 px (`kMinRunPx`), so not at a normal
  field of view. Not on Potato. The Sun's and the Moon's disc jitter in sat_sky.frag is gone (they move through the warp).
- Measured (RTX 3070 Ti, 1920x1009, report view): the pass costs 0.27 ms GPU (in the `ui_overlay` timestamp bucket;
  frame GPU 16.17 / 16.18 ms on vs 16.01 off, 3 interleaved reps); the 2026-10-28 field was 0.36-0.43 ms. Harness
  `state` -> `turbulence` {active, max_rms_px, max_px_3sigma, finest_cell_px, strength, cell_size, speed, clock_s,
  twinkle_clock_s, frames_run}; `refraction.samples[].aoa_mean_dist_km`.
- Open: the Milky Way's sub-pixel star-cloud points do not twinkle; no chromatic scintillation; the warp's
  discontinuity at a near silhouette is not smoothed; the near sea (a few hundred metres) in the report view changes
  frame to frame while paused with or without the warp (not the turbulence; seen 2026-11-03, unexplained).

## Subsystem: The Moon as a body (2026-09-30)

`updatePositions` computes the Moon's geocentric ECI position (`satphot::moonGeoEciAt`: Meeus ch. 47's main
periodic terms — 14 in longitude, 7 in latitude, 10 in distance; ~0.05 deg, eclipse times within minutes) and
from it, per frame: the TOPOCENTRIC direction (`moonDirENU`, up to ~1 deg of parallax from the ground, far
more in deep space), the true angular radius x "Moon size (x real)" (`moonSizeScale`, slot 220, Photometry tab's
EXPOSURE AND MOON section since 2026-10-06, key
`clouds.moon_size`; the old disc was a fixed 3x at infinity), the Earth-centred position in the observer's ENU
(km) and the eclipse state — all into `cloud.moonCenter` / `cloud.moonMisc` (UBO 784 -> 816).
- **The disc is anti-aliased and stable under the sky TAA** (2026-10-28, issue 20261028_191038, "flickers" from 1538 km):
  full variants measure each channel's angle off the centre as the small offset vector (`mOff`; the ray-sphere test's
  bm^2 - (1 - R^2) cancelled in float to ~1 px of the radius: a jagged limb) and take its coverage over one input pixel
  (`hitC`); the normal is from the lateral offset. Moon pixels carry the TAA marker 0.975 (`moonCovTaa`) and
  sky_taa.comp skips its flash rule on them while still (it reset crater rims and the limb to each jittered sample).
  Still view: > 4 levels 1.0% -> 0.08% of the disc, max std 58 -> 19.
- **A real surface (2026-10-27): albedo + LOLA relief, the IAU rotation.** Binding 2 is `moon_maps.rgba16f`
  (`tools/make_moon_maps.py`: NASA SVS CGI Moon Kit, the LROC WAC colour mosaic + LOLA `ldem_16`, cached in
  build/moon_cache/), an RGBA16F equirect mip chain 2048 x 1024 in selenographic lon / lat (RGB albedo scaled so the
  near side's face-on mean = the old photo's, A the height in km); `createMoonMap` uploads it (no decoding), and
  without the file spreads full_moon.png over the near hemisphere. The body frame is the IAU WGCCRE 2009 model
  (updatePositions, double, ICRF taken as the sim's ECI) -> `cloud.moonPole` / `moonPrime` (UBO 12224 -> 12256, w =
  "Moon relief" / "Moon shadows", Photometry EXPOSURE AND MOON, slots 355-356, `clouds.moon_relief` / `moon_shadows`):
  real optical libration and the pole near the ECLIPTIC pole (the old face-on photo was turned by the celestial pole,
  without libration, its far side the photo mirrored and greyed). sat_sky.frag: the level from the pixel footprint
  (`textureLod`, the longitude seam breaks derivatives), the normal tilted by the height gradient at that level (faded
  in at 8-30 px of disc radius), Lommel-Seeliger with the tilted normal (the full Moon stays flat) scaled by Allen's
  law at THIS view's phase angle (moonDirENU.w is geocentric: wrong from beyond the Moon), and near the terminator
  (Sun < ~17 deg over the smooth sphere, disc > 20 px) `moonTerrainSunVis`: a 14-step march toward the Sun over the
  heightfield, the share of the Sun's disc over the terrain (crater floors, mountain shadows, lit peaks past the
  terminator). SKY_LITE: the map with its implicit level, no relief (its size budget); SKY_ENV: the map, no relief.
  **Potato's sat_sky_minimal.frag still reads the old face-on photo at binding 2**: `updateMoonDescriptor` (top of
  recordCompute) rebinds binding 2 when the minimal sky is in use. Cost within noise (RTX 3070 Ti, 1600x900, a quarter
  Moon filling the frame: GPU 7.7-8.1 ms old vs 7.6-7.8 new). Harness `state` -> `moon.libration_lon_deg / _lat_deg /
  pole_pa_deg / sublunar_lat_deg / _lon_deg` (an observer beyond the Moon: `observer alt=6e8` off its axis; `harness_runs/moon/`).
- **Its light follows the PHASE LAW (review 16):** `moonDirENU.w` is the Moon's light relative to full (Allen:
  V = V0 + 0.026 a + 4e-9 a^4, a = the phase angle; quarter 0.09, a 32% crescent 0.04), read by every moonlit term
  (clouds, ground, sky, halo, star/satellite dimming). It was the lit FRACTION, linear: a crescent lit the clouds and
  the sky at a third of full. The disc's own shading recomputes the fraction; harness `state` gives both (`illum`,
  `brightness_vs_full`). "Moon size (x real)" defaults to 1 (2026-10-27, the user: true size for conjunctions,
  occultations and the other sky events; review 16 had made it 2.5, "the true 0.5 deg is a few pixels at a wide FOV",
  and the slider keeps 0.25-4); within 1.5-4 deg of the Sun the drawn disc eases back to its TRUE size (`moonAngR`),
  so a solar eclipse's contacts, totality and annularity are the real ones at any setting. The occultation search
  (`sky::SearchCtx::moonSizeScale`) and the Moon-base glints' place on the disc follow the setting.
- **The eye stops at 100,000 km** above sea level (`kMaxObsHeightM`: Q/E, the altitude scroll, the follow offset;
  review 16 — boost reached the Moon).
- **Lunar eclipses** per surface point: the Earth's disc (enlarged 1/85 for the atmosphere, Danjon) against the
  Sun's (seen from that point, with the Sun's 0.15-deg parallax from the Moon), penumbra -> umbra, and the
  umbra's red refracted light.
- **The Sun's angular radius is its TRUE one** (`sunAngRTrue` from `satphot::sunDistAuAt`, 0.2624 deg in July ..
  0.2711 in January; `cloud.taaJitter.w`, `kSunAngR` in sat_sky.frag): it decides total vs annular. It was a
  fixed 0.2667 deg.
- **Solar eclipses**: near the Moon the Sun disc takes its TRUE radius (it is drawn ~2x large for the glare
  otherwise) and is hidden where the Moon is; glare, halo and the sky dim with the Sun the observer sees
  (`moonMisc.x`, CPU `discOverlapFrac`); a corona at totality; the Moon's shadow on the ground per pixel
  (`moonSunVisible` on `sunDiscVis` for land, `directSun` for the sea) and uniformly on the clouds' key light
  (per-sample would risk the v2 march's register cliff). **Everything sunlit must take the eclipse factor**
  (2026-10-03, found at the real 2037-07-13 totality, where the sky stayed bright): the land's direct sun (it
  reached only the sea), the terrain's sky light (`skyAmbientTerrain`), the clouds' sky light (the march's zenith
  cache), the fog/dust march's `sunF`/`skyF` (the desert dust stayed sunlit), the Sun's bloom seed
  (`FlareSourcePC::sunRefIntensity` only — `SatFlarePC`'s is every satellite's soft ceiling, 0 would zero them)
  and the Sun's lens flare. An annular eclipse falls out of the geometry.
- **The Moon's shadow per point (2026-10-03, `include/eclipse.glsl`):** `eclipseSunVis(pKm, sunD, blurKm)` = the
  Sun's visible share from any point (equal-disc lens area between the umbra edge and first contact; CPU mirror
  `eclipseSunVisCpu`), and `eclipseSkyLight` = that blurred over `kEclipseSkyBlurKm` (180) x the colour of light that
  crossed `kEclipseSideKm` (80) of air sideways at the point's height + the ozone shell at a grazing secant (~20): the
  sky light inside the shadow, deep orange low, blue high. Used by: the sky loop per sample (direct + second scattering,
  `kEclipseMultiScatter` 2 in sat_sky.frag; also the sea's reflection march), land direct sun and sky light, the cloud
  march's key light per sample and its zenith cache, the fog/dust march, the far cloud layer. Everything is gated on
  `cloud.moonMisc.y` (uniform), so outside an eclipse it is a branch; the cloud march stays at 128 registers. The
  orange horizon of totality comes out of this (lit air beyond the umbra seen through the low air, plus the reddened
  sideways light) — no colour grading. `skyExposure()` adapts to the observer's sky light (`moonEclipseSkyObs`, dayness
  x it), so totality is shown at about twilight's exposure (stars come out). Harness `state` moon block:
  `sky_light_frac`, `sky_exposure`. `scripts/eclipse_visuals.satcmd`.
- **The corona** (`solarCoronaAt`, sat_sky.frag, only near totality within `kCoronaMaxR` 9 R of the Sun): fibres along
  dipole field lines to a source surface at 2.5 R (radial beyond), each point mapped to its line's footpoint and the
  legacy cirrus volume (`cloudNoiseTex`) read on that circle — fine across the lines, stretched along them; footpoints
  mirrored about the solar equator (taken per hemisphere they jumped there: a seam), where a streamer stalk is drawn;
  coarse reads for streamers; Baumbach's radial profile; H-alpha pink chromosphere and prominences at the limb. The
  solar axis is the ecliptic pole. `kCoronaGain` 0.4. **Only at totality, about the Sun as seen** (2026-10-27, issue
  20261027_212202 "Refraction silliness during eclipses", 160 km over the Arctic, a partial eclipse at the limb, FOV 2 deg):
  `tot` was (1 - the share seen)^6, so with a third of the Sun left the corona drew at 8% — its pink chromosphere a ring —
  and it was centred on the TRUE Sun while the disc and the Moon are refracted (lifted ~0.3 deg there): a phantom circle
  under the crescent. Now `tot` = (1 - smoothstep(0, 0.02, seen))^2 and the corona reads the disc's bent ray (`dTg`) in
  the refraction block (SKY_LITE keeps the old pair: its size budget). **The eclipse's per-step term** in the sky loop,
  the AP volume and the sea's reflection march is `eclipseLightK` (eclipse.glsl: one geometry for the direct and the
  blurred visibility, exactly 1 outside the penumbra); the loop evaluated the pair twice a step. The report's view (the
  whole frame in the horizon band, the loop per pixel): GPU 63.5-63.9 -> 57.1 ms, the same view a day later (no eclipse)
  51.7-52.1 ms; sky pass 40.1-40.3 -> 34.3 (30.2-30.4 without). The rest is that zoomed limb view's own cost.
- **The dynamic corona** (2026-10-28, issue 20261028_220320 "Sun corona work"; `solarCoronaDyn` in sat_sky.frag, full
  variants only — SKY_LITE keeps `solarCoronaAt`; `SatelliteSimCorona.cpp` `fillCorona`, called after `fillSunspots`;
  data in `GpuRefraction::corInfo / corInfo2 / corStr[24] / corCme[12]`, the refraction block's tail). A pure function
  of sim time: activity = SpaceWeather's cycle envelope (`solarCycleEnvelope`, 0 minimum .. 1 maximum). The field
  lines are a dipole about each LIMB'S NEUTRAL LINE (the current sheet `sheetLat(L)`: a ring tilted 5 -> 67 deg with
  activity, warped near maximum, read where it crosses the east / west limb as the Sun turns, 27.2753 d), the warp
  fading to the poles (no seam there); fibres and coarse bundles read at each line's own footpoint (north and south of
  the sheet apart, cross-faded over +-0.05 rad at it: no mirror). Near minimum: an equatorial belt (Ludendorff
  flattening, `outerK`) and dark polar holes with plumes (`holeK`); near maximum round, a lower source surface. Up to
  12 helmet streamers (11 slots living 2-5 rotations, on the sheet or anywhere by activity, + the largest sunspot
  groups near the limbs), projected from their heliographic place (rise at the east limb, fade toward the central
  meridian). CMEs (<= 6): Earth-directed = `spaceWeatherCmeArrivals` (the storms' own hashes) as a halo ring leaving a
  transit time before arrival (the Sun's Fury: 17.5 h, at its flare); limb CMEs per 3-h bin at 0.5 + 4 act + 3
  smoothstep(Kp 4, 7) a day (loop, cavity, core). The fine structure flows outward (`corInfo.y` = fract(0.07 days)).
  **Coronagraph**: with the inertial lock's occulter (`corInfo.w`) the corona draws without an eclipse (`totC`), not
  dimmed by `sunCloudT` (the occulter forces it 0), lifted by r^`photometry.corona_display_lift` (1.2, no UI) and faded
  from 2.7 R. Harness `corona` (state) / `corona cme [halo=1] [r=4] [days=60]` (jump to a CME's front at r solar radii).
  **Continuity and shape (2026-10-30, issue 20261030_141422 "Corona graphics"):** the fibres are a hand-made mip chain
  (`corFib`: 2.6 / 2^L and 2.6 / 2^(L+1) of the noise cross-faded, L from the texels per pixel; the 6.3x filaments only
  where resolved and inside ~8 R): the volume has no mips, and at ~4 texels a pixel every sky-TAA jitter phase sampled
  other fibres, which in time warp (moving TAA weight) read as rays jumping frame to frame. The phase is sent mod 4 (the
  0.25x / 0.5x reads jumped every 14 days). Out to `kCoronaDynMaxR` 24 R, fading over its outer half; past ~3 R the smooth
  background falls an extra r^-2 and the rays (coarse bundles, 4.5x) and streamers carry the outer corona: beams, not a
  disc. Streamers are sent as position angles (base, stalk target; no trig in the loop), the stalk does not bend near the
  central meridian (it switched limb sheets there), sunspot streamers fade in by size x envelope, and the 12-streamer and
  6-CME lists are capped softly (`softCap`). CMEs: a limb CME's existence uses its own bin's Kp (the current Kp popped
  CMEs in flight), it fades in over its first hour and out between 12 and 24 R, its texture is fixed on the bubble
  (relative radius, angle) so it grows instead of boiling, and it PUSHES the corona (`corCme[2c+1].w`: the structure is
  read at a warped place, swept to its flanks and stretched outward behind the front; a halo stretches it radially);
  streamers a limb CME crosses are blown out to 25% and reform over ~a day (CPU). With the Sun hidden (occulter or
  totality) the zodiacal light no longer stops at 5-10 deg: it brightens inward as the F-corona under the corona
  (`zodHid`). Cost (RTX 3070 Ti, 1920x1009, interleaved): a coronagraph view filling the screen (FOV 6) sky pass 3.9 ->
  6.1 ms (the corona now covers the whole frame; ~1.6 ms per megapixel of corona), the 2037 totality at FOV 12 2.4 ->
  2.65, shot-2 view (FOV 35) equal; 168 registers, TAA binary 960 -> 966 KB, SKY_LITE unchanged.
  Cost (RTX 3070 Ti, 1920x1009, old vs new sky SPVs interleaved, a GPU shared with other apps): sky pass within the
  run-to-run noise at the report's totality (old 4.77 / 4.02 / 3.88, new 5.03 / 2.81 / 2.99 ms) and in a coronagraph
  from 900 km (old, drawing no corona, 4.55 / 4.46; new 3.99 / 3.91); 168 registers both, TAA binary 942 -> 951 KB,
  SKY_LITE unchanged (230272 B); CPU `corona` ~0.01 ms. Captures `harness_live/cor/` (untracked).
- **The Moon writes its distance into the unified depth**, so stars and planets behind it are depth-occluded
  and satellites (nearer) draw in front; star_point.vert's cull takes the real radius from
  `PointDrawPC::moonDirENU.w`.
- **Real eclipses at their real UTC times** (2026-10-03): the 2037-01-31 total lunar eclipse greatest at 13:59:48
  (real ~14:01), the 2037-07-13 total solar eclipse greatest at 02:42:56 at 21.75 S 138.31 E (Moon/Sun 1.043; real
  magnitude ~1.04). Harness `eclipse solar|lunar` finds the next one after the current time
  (`SatelliteSim::findEclipse`, hourly scan + golden section): solar when a partial is seen anywhere on Earth
  (separation < the Moon's parallax + both radii - the Sun's parallax), lunar when umbral; it used to need 0.55 /
  0.6 deg and skipped the July 2037 eclipse (0.68 deg). It places the observer under the Moon (lunar) or where the
  shadow axis meets the Earth (solar; under its nearest point for a partial-only one). `state` reports the moon's
  `sep_from_sun_deg`, `sun_seen_frac`, `drawn_radius_deg`, `sun_radius_deg`, `dist_km`.
  `scripts/eclipses_2037.satcmd` (the two 2037 eclipses from Alice Springs, Charleville, Sydney), `scripts/moon.satcmd`.
  Sun and Moon are of date; stars, the Milky Way and the planets are J2000 (~0.5 deg of precession apart in 2037). SKY_ENV probes use the main observer's
  `moonCenter`, so the Moon's face in reflections is approximate.

## Subsystem: Planets

See `PLANETS_PLAN.md` for the session log and forward-looking next-steps list (in-app QA still
outstanding, known simplifications, ring rendering, attribution follow-up) — this section is the
architecture/design writeup only.

Session 30. Mercury, Venus, Mars, Jupiter, Saturn, Uranus (`enum PlanetId`, `kPlanetCount = 6` —
Neptune excluded, never naked-eye at ~mag 7.8) with real Keplerian-approximation orbital positions,
rendered as clickable points of light. Deliberately **not** built on the satellite orbital-compute
pipeline (`GpuSatOrbit`/`sat_orbit.comp`) — that solves near-field Earth-relative geometry (shadow,
attitude, specular surfaces) planets don't have. Instead: closed-form CPU math in the sun/moon
pattern (`updatePositions()`), rendered through the star pipeline's shape (direction + magnitude-
driven brightness/size, no near-field 3D position needed at render time).

**Ephemeris** (`SatelliteSim.cpp`, top-of-file constants block, `keplerEclipticPos()`): low-precision
Keplerian elements + linear centurial rates (JPL/Standish, valid 1800-2050 —
https://ssd.jpl.nasa.gov/planets/approx_pos.html), one `KeplerElements` row each for Earth
(`kEarthElements`, the table's EM Bary row) and the six planets (`kPlanetElements[]`). Computed every
frame in `updatePositions()`, right after the Sun/Moon block, using the same `Tcent` (Julian
centuries since J2000, derived from the same `dJ2000` the Sun calc already computes) and the same
`epsR` obliquity rotation — no separate time base. Standard Newton-Raphson Kepler-equation solve +
3-1-3 (ω,i,Ω) orbital-plane-to-ecliptic rotation; geocentric vector = `helio_planet - helio_earth`.
Results land in `planetStates[kPlanetCount]` (`PlanetState`: `eciDir`/`distanceAU`/`sunDistAU`/
`phaseAngleDeg`), a plain ephemeris record — distinct from the render-ready `GpuSatVisible` entries
`updatePlanets()` derives from it each frame.

**The Moon** is not on this Keplerian path: since 2026-10-03 it is `satphot::moonGeoEciAt` (Meeus ch. 47's
periodic series — see "The Moon as a body"). Its earlier two-body fit (and before that a circular orbit
with a hand-calibrated phase) put eclipses hours late.

**Brightness** (`updatePlanets()`, mirrors `updateStars()`): apparent magnitude via
`planetApparentMagnitude()` — Paul Schlyter's standard formulas
(stjarnhimlen.se/comp/ppcomp.html), `V = V0 + 5*log10(r*Δ) + phase-angle polynomial`. Saturn's ring
brightness is deliberately omitted (needs Saturnicentric ring-plane geometry, not just phase angle —
accepted simplification, dims slightly near ring-plane-open oppositions). Converted to
`rawIntensity = 10^(-V/2.5)` — **the same convention `initStars()` uses** — so a planet's brightness
runs through the exact same suppression chain stars already have (day/moon/pollution-dome/
extinction), hand-duplicated into `updatePlanets()` per this codebase's established per-consumer-
duplication convention for that formula (see "Subsystem: Light Pollution Dome" above). Its point
is drawn by the shared point-source model (see "Point-source appearance" under Photometry), so a
planet reads at the same visual weight as an equally bright star or satellite. Color
(`planetPointColor(i)`, SatelliteSimPlanets.cpp, 2026-10-28, issue 20261028_222744): ONE true mean colour per planet
(`BodyConst::col`, linear) for the point AND its resolved disc — the disc's albedo is col / the map's own mean
(`kMapMean`, measured from planet_maps.rgba8) x the map, so its mean is col, and the hand-off point the disc shader
draws takes the same colour (`PlanetDisc.pad`). Mars: (1, 0.62, 0.32), butterscotch, from a full-disc reflectance
spectrum under sunlight (B-V ~1.33-1.36); it was a hand-picked (1, 0.2, 0.2) to tell it from the stars, and its map
(as baked) averages a far redder (1, 0.23, 0.11). Jupiter / Saturn: their maps' means, which agree with their
spectra (B-V 0.87 / 1.04) within a few percent. Mercury, Venus, Uranus: the disc's flat colours (the old point colours
were within a few percent of them). `star_point.frag`'s desaturation (bright = full tint, faint = fades
toward white, keyed on the drawn peak) applies to planets unchanged.

**Rendering**: a second tiny host-mapped `planetBuf` (`GpuSatVisible`-shaped, 6 entries) + a second
descriptor set (`planetDescSet`, reusing `starDescLayout`/`starDescPool`'s shape via its own tiny
`planetDescPool` — `starDescPool` itself is sized `maxSets=1`) — but the **same** `starPipeline`/
`starPipeLayout`/shaders (`star_point.vert`/`.frag`), just a second `vkCmdDraw` in `recordDraw()`
right after the star draw. Reusing the pipeline object means `onResize()`'s existing
`createStarPipeline()` recreation covers planets for free — no separate resize handling needed.
One real shader difference was necessary: `star_point.vert`'s atmospheric scintillation/twinkle is
physically wrong for planets (small resolved discs, not point sources) — gated behind a new
`noTwinkle` field, set to 1 only on the planet draw's own copy of the push constant. (Originally
`SatDrawPC.noTwinkle` at offset 164; now `PointDrawPC.noTwinkle` at offset 76 after the
128-byte push-constant split — see the **PointDrawPC** entry under "Subsystem: GPU Orbital
Pipeline".)
The pre-existing Moon-disc occlusion cull in the same shader stays active for planets unchanged
(correct — a planet behind the Moon's disc should still be culled).

**Picking**: `pickPlanetAt()` is cheaper than `pickSatelliteAt()` — `planetBuf` is already
HOST_VISIBLE/COHERENT (`updatePlanets()` writes it directly from the CPU), so unlike satellites'
device-local `satVisibleBuf` there's no staging-buffer copy at all, just a loop over 6 entries using
the existing `projectSkyDirToScreen()`. `selectedPlanetIndex` (mutually exclusive with
`selectedSatIndex` — selecting one clears the other) is tried first (planet priority on an exact
overlap) at both click sites (`SatelliteSimUI.cpp`'s mouse-click handler and `KB_SELECT_SAT`'s
center-screen equivalent). `formatSelectedPlanetInfo()` fills `planetInfoLine[]` (name/magnitude/
distance/phase) — unlike `formatSelectedSatInfo()` (called only when the selection changes, since a
satellite's orbital elements are static), this is re-called **every frame** the selection is active
(right after `updatePlanets()` in `recordCompute()`), since a planet's distance/phase/magnitude
changes continuously. `buildSelectedSatPanel()` branches on `isPlanet` to pick its data source
(`planetBuf`'s own mapped memory directly for planets — never stale, vs. satellites' one-frame-stale
`lastPickedSkyDir` GPU round-trip) and info-line array, then shares the rest of the panel/reticule
rendering unchanged.

**Settings**: `showPlanets` (global) + `planetEnabled[kPlanetCount]` (per-planet), UI in
`buildSettingsConstellationsTab()` reusing the exact ON/OFF row pattern the constellation list
already uses. Persisted as `j["planets"]` (`{show_planets, list:[{name,enabled}]}`) in
`saveSettings()`/`loadSettings()`, same shape and same ungated (non-schema-versioned) treatment as
`j["constellations"]` — not a graphics-tuning value a schema mismatch needs to guard against.

### Resolved discs and the Galilean moons (2026-10-27)

`SatelliteSimPlanets.cpp`, `shaders/planet_disc.vert/.frag`, `include/planet_disc.glsl` (mirror: `GpuPlanetDiscUBO` in
SatelliteSim.h, std140 vec4s), `tools/make_planet_maps.py` -> `assets/textures/planet_maps.rgba8`. At the 0.5-deg field the
camera reaches, Jupiter is ~24 px across, Venus near inferior conjunction ~28, Saturn's rings ~25, Mars at opposition ~9.
- **Its own small pipeline, not sat_sky.frag** (at the 16 sampled-image floor, and SKY_LITE at its budget): one quad per
  resolved planet drawn right after the planet points (`recordPlanetDiscs`), additive, at kDepthFar with the depth test
  (terrain, opaque cloud and the Moon hide it), the cloud composite dimming it as star_point.frag does. Set: 0 the disc UBO,
  1-2 the cloud targets, 3 the scene depth (manual test below render scale 1), 4 the maps (a 2D array: Mars, Jupiter,
  Saturn, Saturn's ring profile). `writePlanetDiscTargets` follows the cloud/depth recreation in
  `recreateComputeScaledTargets`; the pipeline is rebuilt in onResize. Nothing resolved = no draw (~0 cost).
- **Share**: `smoothstep(1, 2.5 px)` of the equatorial radius on screen; a resolved planet's planetBuf record gets
  `angularSize < 0` (star_point.vert skips it; picking and the panel keep it) and the disc shader draws the point's
  remaining (1 - share) itself.
- **Light**: the planet's magnitude (Saturn: Meeus with the rings' term -2.60 sin|B| + 1.25 sin^2 B, now also the
  point's; the ball's normalisation uses B = 0) spread by its law (Lommel-Seeliger: Mercury, Mars; Minnaert k 0.8-0.9:
  Venus, Jupiter, Saturn, Uranus) on the oblate spheroid; the law's disc mean is a CPU integral (32 x 32) so the disc's
  total is the magnitude. Units pi L / E x 1.35 (the sim's Moon disc is ~1.35x physical) x `planets.disc_gain` (1.5), the
  sky exposure and the sky's tonemap (AgX with the tone settings). NOT x the point's suppression (day, Moon, light
  pollution are the point model's stand-in for sky contrast; a disc is added to the real sky, like the Moon: Venus shows
  in daylight). Extinction as the point's (reddened 0.75 / 1 / 1.4). Rings and Venus's rim are in I/F x 1 / r^2.
- **Hand-off**: the point style is not linear (zoomed points are 6-px-sigma saturated blobs: up to +7 mag of zoom
  aperture), so at 2.5 px the disc's light is scaled (`handScale`, bisected on a grey AgX) to the point's displayed
  energy, clamped to +-3 stops, relaxing to physical by 5 px.
- **Orientation**: IAU WGCCRE 2009 poles and W (light-time corrected; `x0` = longitude 0 in the disc's local frame: x the
  view's right, y up toward the zenith, z toward us); Jupiter: Meeus's System II central meridian, the map's Great Red
  Spot (u 0.3682) put at System II 55 deg + 18 deg / yr from 2024.5 (an extrapolation). Maps: u = 0.5 + east lon / 360
  (checked: Syrtis Major); stored sqrt(ratio to the map's mean / 8) so the disc's mean stays the magnitude's.
- **Saturn's rings**: ray / plane, the profile layer (C, B, Cassini division, A, Encke / Keeler gaps; optical depth +
  albedo, made by the bake script), lit or unlit face, the ball's shadow on them, their shadow on the ball, the ball seen
  through them. **Venus**: a forward-scattering rim round the limb past ~140 deg of phase, a full ring near inferior
  conjunction. **Jupiter**: the moons' shadows (soft by the Sun's disc).
- **Galilean moons** = planetBuf records kPlanetCount.. (`kPlanetBodyCount` 10; `planetBodyName`), points of the star
  pipeline, selectable (`select planet Io`) — by a click only when clear of Jupiter on screen (> 8 px from its centre or
  4 px outside its disc, 2026-10-28, issue 20261028_190641: at a wide field a click on Jupiter picked a moon 2 px off). Meeus ch. 44 low accuracy (light time in it), placed in Jupiter's frame (IAU
  pole, the Earth's direction): eclipsed by Jupiter's shadow (soft), hidden behind the disc, x 0.05 in transit over a
  resolved disc; flags transit / occulted / eclipsed / shadow on Jupiter. **Checked against JPL Horizons** (2036-11-22
  07:00 UT, offsets from Jupiter): Io 86.8" E (Horizons 86.2), Europa 206.3" W (205.7), Ganymede 353.1" W (352.6),
  Callisto 399.4" E (398.7); north offsets within ~2". An Io transit predicted by a Python replica (11:30 UT) shows Io
  and its shadow on the disc in the app.
- **Panel**: angular diameter (and px), phase angle; Jupiter's moon line (east to west, `[J]`, flags), Saturn's ring
  opening, Mars's central meridian; a moon: offset in Jupiter radii and arcsec, its state. The reticle surrounds the disc.
  Harness: `state` -> `planets` {bodies[], discs_drawn, galilean[]}, `selection.planet_lines`; `camera look <planet>`
  aims at the REFRACTED direction now (near the horizon a 0.5-deg field is narrower than the refraction).
- Settings: `planets.resolved_discs` (on; Constellations tab "Resolved planet discs"), `planets.disc_gain` (1.5, no UI).
- **Cost** (RTX 3070 Ti, 1600x900, Jupiter at 0.5 deg, interleaved on/off x2): GPU within noise (5.51 / 5.54 vs 5.54 /
  5.50 ms), CPU update_planets 0.009 -> 0.052 ms with a disc resolved. Maps 11.2 MB (1024 x 512 x 4 layers, mips).
- **Texture vs procedural**: Mars needs its albedo map (no procedural stand-in reads as Mars) and Jupiter's belts and GRS
  read far better from the Cassini map than from noise; at <= ~25 px a texture fetch is cheaper than banded noise.
  Saturn's ball is a map too (free in the array); its rings are a 1D profile; Mercury, Venus and Uranus are flat colours.
- Open: no differential refraction (the disc stays round near the horizon); seeing is only a mip bias (the refraction
  table's aoa) and a whole-disc jitter; the GRS longitude is an extrapolation; Saturn's ring brightness against the ball
  unjudged at 5 px; Neptune not modelled; the trails skip a resolved planet.

---

## Subsystem: Stars (2026-10-27)

`SatelliteSimStars.cpp` (catalogue, picking, panel, the supernova, harness `stars`), `updateStars` / `initStars` /
`createStarEnvBuffer` (SatelliteSim.cpp), `tools/make_star_catalog.py`.
- **The catalogue is `assets/stars/star_catalog.bin`** (`tools/make_star_catalog.py`: the HYG database v4.1, CC BY-SA 4.0,
  downloaded into `build/star_cache/`): every star to V 8.5 (61,816; 417 with IAU proper names), **sorted brightest
  first**, 48-byte records (RA / Dec J2000, V, B-V, distance pc, HIP, HR, string offsets for the proper name,
  Bayer / Flamsteed designation, spectral type and constellation name, the IAU abbreviation with a variable flag) + a
  string blob. Overrides: Betelgeuse at 168 pc (Joyce et al. 2020), Capella's spectral type. Without the file: the
  compiled-in BSC5 (`star_catalog.h`, 8404 to 6.5, no names). `starRecords` / `starInfo_` / `starRaDec_` share the index.
  The env renderer's star grid (SKY_ENV, `STAR_ENV_COUNT` 8404) takes the brightest 8404: sat_sky.frag is unchanged.
- **updateStars draws a PREFIX.** Each frame it writes, uploads (`mirrorDirty` of the prefix only: the buffer is no
  longer re-sent whole) and draws (`starDrawCount`, both star draws) the stars brighter than the frame's cutoff: the
  point style's limit (`pointEffLimitMag`) and the darkest direction's naked-eye limit + 2 (`starLimitMagFrame`); none
  while the day gate is shut. Never index a star record past `starDrawCount` expecting this frame's values
  (`skyObjectEnu` handles it). Everything that depends only on a star's elevation (the line of sight's extinction, the
  refraction of the primary image, its jitter and twinkle) is ONE lookup in `starElTab_` (1024 knots over sin(true
  elevation) from 0.03 under the limb, quadratic toward it), rebuilt when the observer radius, `extinctionCoeff` or the
  refraction table change (`refrSerial_`, bumped in `refrDerive`); it replaced `refractEnu` + a Chapman table per star.
- **Faint stars only under dark skies** (`stars.sky_limit`, default on; `stars.limit_boost` 1.2 mag): each star is gated
  by the naked-eye limit of the sky in ITS direction, a Schaefer-style V_lim = 7.93 - 5 log10(1 + 10^(4.316 - B/5)) +
  boost (the boost a keen dark-adapted eye's, fading out as the sky brightens; + the zoom's optics gain), the sky B from
  darksky.glsl's linear model (pristine 22 + the city dome with its compression undone + twilight with its anisotropy +
  the Moon and the supernova as an 18 mag/arcsec^2 sky at the full Moon high up). Response 1 / (1 + (Ilim / I)^3),
  divided by a pristine sky's, so a pristine sky draws every star as the point style alone would. The old linear dims
  (beam dome, Moon) are kept on top; the city dome's only with `stars.sky_limit` off (since 2026-10-28 the city glow is
  the skyglow model, "Light Pollution Dome": NELM 7.8 pristine, 5.5-5.7 a B6 town, 3.2 downtown LA).
- **Selectable stars**: named or brighter than V 4.5, drawn this frame (`pickStarAt`). A selected star is
  `selectedStarIndex` (its own index, as the comets' `selectedCometIndex`; the first cut used `selectedPlanetIndex =
  kPlanetCount + i`, which the Galilean moons now occupy). Any other selection wins: `buildSelectedSatPanel` drops
  the star while a satellite, planet or comet is selected, and `skyJumpTo` (every Watch) clears it. Click / centre-select
  order: a comet, a planet, a star within 4 px, a satellite, a star within its hit radius. Panel (`starInfoLine`,
  `formatSelectedStarInfo` every frame): name, spectral type and constellation, V (variable), distance (ly, pc),
  designation / HR / HIP, colour (B-V). No Track / Info buttons. Its place: `starEnuNow(i)` (this frame's eci2enu, the
  drawn record's refraction when current).
- **The 2037 Betelgeuse supernova** (`supernovaMagAt`, a pure function of sim time; `stars.supernova`, default on):
  breakout 2037-12-25 03:00 UTC (`kSupernovaT0`), V to -6 in 20 min, the cooling envelope to -7 by 6 h, the rise to
  -11.2 at day 7 (M_V ~ -17.8 at 168 pc with A_V ~0.5: the bright end of II-P), the plateau to -10.2 by day 100, the
  drop of 3.2 mag over 25 days, the 56Co tail (0.98 mag / 100 d, + 0.4 / 100 d from day 400 as dust forms); B-V -0.35
  (breakout) .. 1.5 (tail). Below +0.5 again ~2 years on (`supernovaEndT`). Before it Betelgeuse varies about V 0.45
  (416 d and 2100 d, +-0.35; `stars.betelgeuse_variable`). Brighter than -1 it is a **kFireBuf point** (`supernovaFill`
  after `moonBasesFill`: point, bloom, glare, the Moon-base glints' path, range 9e8 m so the Moon hides it) and its star
  record is dark; by day it keeps 6.5 mag of contrast (visible in daylight near its peak). Its light dims the faint
  stars like moonlight, and **it lights the Earth as a second Moon: the NIGHT KEY light** (2026-10-28, issue
  20261028_222111; `nightKeyLightENU()`, SatelliteSimStars.cpp): the Moon and the supernova folded into ONE directional
  source — direction = their light-weighted mean (each weighted by its own horizon gate, so a set Moon does not tilt it
  down; exact when one dominates), w = their summed light vs the full Moon (`snLightK` = 10^(-0.4 (V + 12.7)) above
  the air: illuminance 2.54e-6 lux x 10^(-0.4 m), -11.2 = 0.077 lux against the full Moon's 0.31, i.e. 0.25 of it, a
  gibbous Moon's light from a point). With no supernova up it IS moonDirENU (nothing changes). The cloud passes take
  it as `CloudMarchPC::moonDirENU` (the v2 march's key light, phases, ice / rain optics; the far layer; the rain's key;
  the aurora's sky dimming); sat_sky.frag reads `CloudParams.nightKeyENU` (the old unread `auroraSky` vec4, renamed
  in place; w 0 = use the Moon) through `NK_DECL` / `NK` at every site the Moon LIGHTS: terrain direct and moonlit-sky
  terms, sea foam and the moon glint, the flat clouds, the moonlit sky colour, the Milky Way's / zodiacal light's
  moon dimming. The Moon's disc, corona and halo stay on moonDirENU. SKY_ENV (another ENU frame) and SKY_LITE (budget;
  the macros fold to moonDirENU, so its SPIR-V shrank 40 B) keep the Moon alone. It used to add 0.25 sin(el) to
  "Night sky light" (`snDiffuseMoonFrac`): an isotropic term on land and cloud ambient only, nothing on the sea or as a
  key light, invisible at the report's mid-Atlantic view. Cost: sky TAA binary +0.9 KB (942.1 -> 943.0 KB), 168
  registers both; the sky pass within noise (interleaved, a GPU shared with another app). Colour is the Moon's (the
  supernova's B-V ~0 blue-white is not tinted). Harness `stars` -> `betelgeuse.sn_light_vs_full_moon`, `night_key`,
  `moon_light_vs_full`. No shadows. Sky events kind `kSupernova` (`skySearchSupernova`: one record when
  its window meets the range; `skyWatchSupernova`: the peak, or now while under way, a dark site with Betelgeuse
  >= 20 deg up (Sun <= -15, -9, -4, else up), the camera on the Betelgeuse-Rigel midpoint at 42 deg, Betelgeuse
  selected). Kind appended after meteors and comets; icon `sky-supernova` (40).
- **Under a dark sky the faint stars are drawn as if the point limit were 1 mag fainter** (`stars.dark_sky_extra`,
  0-3): the point model subtracts its value at the limit, so at limit 8 a mag-7.5 star was ~0.004 of white and a
  pristine sky read nearly empty. Per star the intensity is remapped (I^gamma + w x the difference of the two limits'
  offsets) so pointPsf(limit) draws what pointPsf(limit + extra) would, w = how close ITS sky is to pristine (the gate's
  Ilim within 1.5 mag of a pristine sky's) x the night gate. Stars only: satellites and planets keep the shared limit.
  Measured (600x400 zenith crop, Atacama, moonless): pixels > 12 levels over the sky 1918 -> 2788; a Colorado suburb
  (40.25 N 105.0 W) 11 -> 15, a rural Colorado sky 17 -> 15: suburban skies are not busier.
- **Cost** (RTX 3070 Ti, 1600x900, harness `perf` cpu `update_stars`): pristine night (all 61,816 in the prefix, ~32k
  lit) 0.49 ms, LA at night 0.20, by day 0.02, supernova peak from a dark site 0.36; GPU frame within noise of before
  (6.0-6.5 ms in these views). The loop runs in 2048-star chunks on the storm catalog's pool
  (`stormCatalog->parallelFor`; single-threaded it was 2.7 ms), the naked-eye limit comes from a 25 x 33 grid over
  (elevation, bearing) built per frame, the dark-sky remap uses a polynomial log2 / exp2, and the elevation table is
  rebuilt only on a new refraction table (`refrSerial_`, bumped in updateRefraction when a table is taken: refrDerive
  runs every frame), a metre of eye height or a new k (rebuilt every frame it cost 0.18 ms). The old BSC5 loop was
  0.26-0.5 ms at night. A GPU pass is the next step for a much larger point set (the Milky Way's particles): the
  catalogue, the prefix draw and the per-star constants are ready for it.
- Harness: `stars [state] | find <name> | select <name>`, `select star <name>`, `camera look|track <star>`
  (docs/HARNESS.md). Scripts `harness_runs/stars_scripts/` (untracked).
