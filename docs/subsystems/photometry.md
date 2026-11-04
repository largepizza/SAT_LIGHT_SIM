# Photometry, point sources, bloom and glare, sky glow

Flare/magnitude conventions, the point-source model, bloom and glare, the sky glow SSBO.

Part of the subsystem notes (`docs/subsystems/`, map in `AGENTS.md` §4). Moved verbatim from
`CLAUDE.md` on 2026-11-03. Do not read this file whole: `python tools/outline.py docs/subsystems/photometry.md`
lists its sections and topics with line numbers. New notes go next to the section they belong to.

## Subsystem: Photometry / Shader Constants

Photometry values are **runtime members** on `SatelliteSim`, synced each frame to `SatFlarePC` —
except `brightnessScale`/`mirrorBoost`, which the reflectance model in `sat_orbit.comp` reads from
`GpuSatTypeHeader` at the front of `satTypeBuf`. They are persisted in `settings.json` and adjustable in the settings window.

| Member | Default | Description |
|--------|---------|-------------|
| `brightnessScale` | 1.0 | global flux multiplier |
| `daySuppression` | 500.0 | sky background suppression ratio (sun) |
| `mirrorBoost` | 1000.0 | mirror peak multiplier (MIRROR_BOOST) |
| `visThresh` | 0.0 | visibility cull threshold |
| `highlightFlare` | 0.05 | fixed flare for highlight/census mode |
| `moonSuppression` | 4.0 | sky background suppression ratio (moon) |
| `skyglowGain` | 1.0 | x the calibrated skyglow model (`photometry.skyglow_gain`) — see "Subsystem: Light Pollution Dome" |
| `extinctionCoeff` | 0.18 | sea-level zenith extinction, magnitudes — see "Subsystem: Atmospheric Extinction" |

`effectFlare = flare / (1 + (dayBright × daySuppression + moonBright × moonSuppression) × atmFrac)`,
then `×= extinction` (line of sight), then `×= (1 − domeVal × 0.85)` (light pollution)
`magnitude = kMagRef - 2.5 × log10(effectFlare / kMagRefFlare)` where `kMagRef=6.0`, `kMagRefFlare=0.008`

**Point-source appearance (2026-09-23) — one model for satellites, stars and planets.**
`shaders/include/point_style.glsl` (`pointPsf(mag)`, CPU mirror `SatelliteSim::pointPsf`) maps an
apparent magnitude — AFTER every suppression and extinction term, i.e. as drawn — to a Gaussian
PSF: displayed flux `D = 10^(-0.4·γ·(m − refMag))` minus its value at `limitMag` (so a point fades
to exactly nothing there), drawn with sigma `sigmaPx` and peak D while D ≤ 1; brighter points keep
peak 1 and widen (`σ = sigmaPx·√D`, flux-conserving) up to `sigmaMaxPx`. Sprite edge =
`2·(3σ + 1)` px (≥ ~5 px, which also keeps the S2a sub-pixel flicker fix). Each shader converts its
own record units to a magnitude first — satellites keep `effectFlare` (`satFlareToMag`: the
flare_source bloom, picking and the UI read it), stars/planets `10^(-0.4 m)` (`relFluxToMag`).
Users: `sat_point.frag` + `sat_flare.comp` (sprite size, and a zero record past `limitMag` so faint
satellites cost no fill; highlight mode too), `star_point.frag` + `updateStars()`/`updatePlanets()`
(size every frame). Parameters: the five "Point ..." Photometry sliders (`pointRefMag` 1.5,
`pointGamma` 0.8, `pointLimitMag` 8.0, `pointSigmaPx` 0.45, `pointSigmaMaxPx` 6.0, persisted under
`photometry.point_*`) → `GpuPointStyle` → `pointStyleBuf`, a host-coherent UBO rewritten every frame,
bound as `descSet` binding 11 and `starDescLayout` binding 5. Until this, satellites (sigmoid core,
log-sized sprite with a range term) and stars (sqrt core, sqrt-sized sprite) had separately tuned
curves: a mag-3 satellite drew about as bright as a mag −2 star (Jupiter), a mag-5 satellite ~13× a
mag-5 star. Not unified yet: the bloom/corona (`flare_source`) exists for satellites and the Sun only.

**Satellite display (2026-10-07) — the record holds a DISPLAYED effectFlare.** `sat_flare.comp` writes physical
effectFlare x 10^(0.4 `satGainMag`) (exposure gain minus the satellite adaptation; `GpuPointStyle`'s former pads)
x the resolved-object factor x the formation merge, so the point, bloom, glare (which ignored the exposure before)
and the HUD's peak magnitude (corrected back by the gain) follow one value; `sat_point.frag` adds `satGainMag` back
to the magnitude (the UBO's reference magnitude already carries the shift for stars, planets and city lights).
- **Resolved satellites** ("Resolved satellites" `photometry.sat_resolved` 1): a model satellite's sprite fades
  from the point into its model's mean surface brightness (`resolvedSatFlare`, point_style.glsl: 1 - exp(-satPhysK
  x raw flux x extinction / its area in px), `satPhysK` = pi x skyExposure / (kFluxToFlare x brightnessScale x
  Omega_pix), exactly a model's mean pixel) over an effective diameter meshPx x sqrt(fill) of 0.8 -> 2.5 px, spread
  over that size (sigma from the sprite size; drawn flux kept). The point style is white at ~mag 1.5, a model's
  pixel at ~mag -7.5: a StarMind 2-10 px wide was a blazing point handing over to a grey model. Never brighter
  than the point.
- **Adaptation** ("Satellite adaptation" `photometry.sat_adaptation` 1): points (satellites, stars, planets) lose
  max(2.5 log10(1 + S / 0.003), above the air 2.5 log10(1 + M / 0.01)) magnitudes, eased 0.25 s up / 2.5 s down,
  capped at 8: S = the drawn meshes' light on screen per frame pixel in the sky exposure's units, M = the exposure
  meter's displayed mean (auto exposure on). The ground keeps its day/moon/pollution suppression (the M term is
  gated above 40-100 km). User snapshot (Antarctic limb from 583 km) 2.7 mag; a Reflect mirror reflecting the Sun
  from 4 km, the cap. S past 0.02-0.2 also lets the auto exposure stop down at night (only as clipping asks).
- **Formation merge** ("Merge formations" `photometry.formation_merge` 1, sat_orbit.comp): over a cluster 3 -> 1
  px wide its slot-0 member draws N x its light and the others fade; the point's display response (flux^0.8) read
  eight overlapping members ~0.45 mag too bright and none glared. Drawn point, bloom and glare only: the sky glow
  bins and ocean glints keep the members' own light (a merged leader's 8x in a per-bin MAX brightened the whole
  night sky from the ground). Selection panel / info window: the formation's combined magnitude.
- The mesh candidates carry the displayed flux and log2 of the share the resolved factor kept; the mesh's bloom
  follows the displayed sprite, its glints' glare (glareNorm) the unresolved one (a sun glint is not the mean
  surface brightness), normalised so bloomScale x glareNorm is unchanged.

**Bloom and glare (2026-09-24).** The bloom (`flare_source` → `flare_blur` → `flare_composite`) is a
quarter-resolution BROAD glow: a narrow separable Gaussian plus a wide one (σ 10 texels, "Flare
streak" scales it), exactly round, final result back in `flareSourceImg` (four dispatches — see
`flare_blur.comp`'s header). Its last pass was a six-spoke, then a 16-direction streak: a bright point
came out as a dotted starfish that stopped dead where the taps ran out. The composite's ceiling is a
soft knee on luminance (unchanged below 0.45, exponential approach to 0.8); it was a hard per-channel
`min(c, 0.8)`, which flattened overlapping glows into white plateaus with sharp edges.
The sharp part is the glare (`include/glare.glsl`): a full-resolution point sprite past
`glareThreshold` in the bloom's log response — a glint core, a tight halo, and `glareSpikes` spikes of
their own length and brightness, each a line of constant pixel width (anti-aliased at any radius)
fading along its length as (1 − r/len)^`glareFalloff`, windowed smoothly to zero at the sprite edge so
a crowd of glares sums softly. The first cut used the sun corona's angular noise (hundreds of rays,
an aliased centre) and a blue anamorphic streak the sun doesn't have; both are gone.
- Satellites: `glare.vert/.frag`, one sprite per listed satellite, flare source descriptor set,
  occlusion tested at the source's screen position.
- Mesh glints: `mesh_bloom.frag` writes each texel's mesh light in effectFlare units into the flare
  source's ALPHA (per-instance `GpuMeshInstance::glareNorm` = the sprite's effectFlare per unit of
  bloom seed; satellite sprites write alpha 0), capped at 1e4 per texel (glare saturates at 256; a
  Reflect mirror in its beam overflowed the RGBA16F target to inf, so its glint's position was NaN
  and the Sun in the mirror never glared). Which light may glare: only SUN-LIKE surface brightness
  (π·L/E 150 → 1500: the Sun in a mirror ~4e4, an OSR radiator ~2e3; a rough-metal edge, even grazing,
  < 50). While the mesh is still point-like (below `kGlarePointPx` = 12 px, gone by 36) its glare is
  the SPRITE's, at its centre: the keep list's z (glare keep) goes to `sat_flare.comp`, which writes
  effectFlare × it into the visible record's last field (the pre-photometry mesh size, free by then),
  and `glare.vert` glares on max(flareIntensity, that) (2026-09-25). Until then mesh_bloom counted all
  of a point-like mesh's light, so its one glare sat on whichever few texels of the small mesh were
  brightest and jumped about until the satellite was close. **Only the sun-like share of the light keeps that glare** (2026-10-07,
  `glintShare`: each lobe's sunlight x smoothstep(150, 1500, pi x its mean radiance), mesh_bloom's test): the rest
  (diffuse, glossy, earthshine) glares only while the mesh fades in (fadeIn -> fadeFull, 1.5 -> 3 px). Keeping all
  of it to 12-36 px drew a backlit, earthlit StarMind 17 px wide (mag -4.5 at 4.7 km, its mesh near black) as a
  full starburst, x7 by the near gain. A Reflect mirror's glint share is 1 (unchanged). The first cut let
  all of it glare: every edge and vertex of a close-up satellite flared (each a sliver of a very
  bright satellite's flux) and filled the 64 slots with ~400 px sprites, a large part of the close-up
  frame-rate drop. `glare_find.comp` lists every 5×5 local maximum past the threshold into `glintBuf`
  (`GpuGlintList`, `include/glint_list.glsl`, 64 max, its own indirect args); `glare_mesh.vert/.frag`
  draws them with the same profile. **Sub-texel position (2026-10-06):** a parabola through the log light of the
  peak texel and its axis neighbours (exact for a Gaussian lobe), blended toward the 9x9 near-peak centroid by how
  much light besides the peak's is in it (flat tops). At texel centres the glare moved in 4-px jumps with holds —
  the "wandering" glare of the release cinematic's mirror shot (`scripts/mirror_glare.satcmd`,
  `tools/harness/glare_track.py`: the glint list is in `state` -> `selection.glints`, host copy `glintReadBuf`).
- **Proximity (2026-09-26, `glareNearGain` / `glareNearRangeKm`):** sprite radius =
  `glareSizePx` × (0.6 + s) × `glareNearScale(range)`, which ramps 1 → `glareNearGain` over
  `glareNearRangeKm` → 0, smoothstepped — exactly 1.0 at or beyond that range, for a range of 0
  (unknown / none) and wherever `glareNearGain` ≤ 1 (the off switch). So a source hundreds of km off
  — any satellite seen as a point sprite from the ground — keeps the distance-tuned look, while the
  viewer's model and a mesh resolved in the main view, metres from the camera, spread their glare
  `glareNearGain` (3) times wider. The range comes from data the record already carries — no extra
  pass, buffer or RTT: a sprite's `GpuSatVisible::rangeM` (`glare.vert`), a glint's `glintPos.w`
  (`glare_find.comp` writes `SatelliteSim::meshGlareRangeM`, the nearest mesh it drew this frame;
  `viewer_glare_find.comp` writes `ViewerGlare::rangeM`, the viewer camera's own `viewerDist`), both
  read back through the one `glareNearScale` in `include/glare.glsl`. `GlarePC`'s old `pad0`/`pad1`
  became `nearGain`/`nearRangeM` (metres). (Since 2026-10-20 `glare.vert` has its own `glarePipeLayout` and
  GlarePC is 128 B: the cluster grid below.)
- **Glare clusters (2026-10-20, issue 20261019_222938 "Glares": "clusters have always had a spidery, sea-urchin-y
  look").** `glare_cluster.comp` (recordCompute, after glare_find; 256 workgroups striding over the visible list's
  vertex count, + the mesh glints when glare_find ran) sums every source that will glare into a screen grid
  (`glareGridBuf`, a tile ~1/68 of the view's height, 3 uints a tile: the weights and the weighted position inside the
  tile, fixed point), each weighted q = (flare / the threshold's)^2 x its visibility (glare.vert's own projection and
  occlusion tests). `glare.vert` and `glare_mesh_scene.vert` then give each source w = q / (q + the others' q within
  0.8 x its glare radius, smoothstep-windowed on each tile's centroid) and keep w^2 of its spikes (`glareSpikeWeight`,
  include/glare.glsl), with shorter rays and a slightly stronger halo as w falls: a lone source is unchanged (exact:
  measured <= 2 levels), the brightest of a group keeps most of its star, N equal neighbours each 1/N^2 (a soft glow).
  Core and halo untouched. "Glare cluster merge" (Photometry, `photometry.glare_cluster_merge`, 1; 0 = as before;
  slot 38, the photometry arrays are [39]). Bindings: descLayout's 5-7 (cloud targets, depth) are also VERTEX | COMPUTE;
  glareMeshDescLayout = 0 the glint list, 1 the grid (VERTEX | COMPUTE); glarePipeLayout / glareClusterPipeLayout =
  {descLayout, glareMeshDescLayout}. The viewer's glare (`glare_mesh.vert`, SatMeshRenderer) has no grid (gridW 0); the
  body is shared in include/glare_mesh_vert.glsl. Cost within noise (Reflect ring from 20 km: 9.7 vs 9.6 ms GPU).
Photometry sliders "Glare gain / size (px) / threshold / falloff / spikes" (`photometry.glare_*`),
plus "Glare near gain / near range (km)" (`photometry.glare_near_gain`, default 3.0, and
`glare_near_range_km`, default 400). The five glare defaults are now the values tuned on screen for
satellites seen from the ground at a distance — a release build's own `settings.json` (gain 0.684,
size 29.79 px, threshold 1.765, falloff 4.678, spikes 10), not the earlier untuned 1.0/48/0.3/2.5/10
— so a fresh install starts at the shape the proximity scaling must leave alone.
  **Measured 2026-09-26** (`harness_runs\nearcheck2`, the `viewer_glare.satcmd` g20/g21/g22 A/B,
  `imgtools` at threshold 2 on a 360×280 crop of the viewer — the only place this view draws a glint):
  gain 3 vs gain 1 changes 7.1 % of that crop (bounding box 363×378 px around the glint) against 1.5 %
  (162×150) for glare on-vs-off at gain 1, i.e. a ring ~4.6× the disc it surrounds → a radius ratio of
  ~2.4, and visibly a whole burst instead of a compact star. Over glare-off, a 120×120 window on the
  glint reads +17.3 mean luminance at gain 3, +3.8 at gain 1, +11.8 for the pre-proximity build
  (`viewer_glare_final2`, which has no `glareNearScale` at all) — so the near case restores something
  like the old on-screen size while the off case is the new release shape, ~1/3 of it. Nothing outside
  the glints moves: the g10 viewer crop and a 300×300 main-view sky crop are byte-identical
  (`mean_abs` 0.0) between the two builds with the low threshold both on and off. Two whole-frame diffs
  of the same pair are *not* comparable — the sun-pillar beam and the status bar's fps text change
  frame to frame — so A/B the crop.

`dayBright`/`moonBright` are elevation-ramp scalars (squared linear, sun/moon dot observer-zenith)
computed once per frame — **uniform across the sky, not per-satellite-direction**. This is an
accepted simplification for both (unlike light pollution below, neither has been made directional).
`moonBright` additionally omits the near-moon sky-brightening halo (real moonlight scatters more
strongly close to the moon's disc) — not built, no current plan to.

Stars (`SatelliteSim::updateStars`, CPU-side) apply the same three suppression sources
independently, with their own fixed (non-slider) response caps: `kStarPollutionMaxDim=0.85`,
`kStarMoonMaxDim=0.9`. Day suppression for stars is `nightFactorEff` (sun-elevation ramp), not
`dayBright`/`daySuppression` — a separate, older formula; the two were never unified.

## Subsystem: GpuSatInput (deleted 2026-09-22)

The 80-byte `sat_orbit.comp` → `sat_flare.comp` hand-off record no longer exists — see "Lighting
overhaul Phase 1" under "Subsystem: GPU Orbital Pipeline". Its job is done by the pre-photometry
`GpuSatVisible` encoding. Below-horizon and disabled satellites are simply never appended to the
compact list (Phase 1b); `sat_orbit.comp` also dropped the per-satellite `asin` that existed only
to feed `sat_flare.comp`'s duplicate horizon cull.

---

## Subsystem: Sky Glow SSBO

**Air toward the satellite (review 6):** `sat_sky.frag`'s glow loop weights each bin by the smaller of
the air along the pixel's ray and the air between the eye and the bin's direction (from the eye's height,
or the tangent height of a path below the local horizontal; Chapman airmass capped at ~35). From 40+ km
the pixel-ray weight alone lit the whole limb around every glaring satellite. Unchanged from the ground.

`sat_flare.comp` writes a spatial histogram + per-satellite flare list each frame → `sat_sky.frag` reads them.

**The bins SUM, since 2026-10-16** (issue 20261016_162337, the sky flickering with Kessler debris): each bin is the
summed displayed effectFlare (uint, `atomicAdd`, each flare capped at 1e6) of the flares past `GLOW_THRESH` 20
(~mag -0.8), and the glow's response is log2(sum / 300) / 4, clamped 0..1.5 (`kGlowRef`). It held the bin's brightest
satellite with log2(max) / 4, full at a single mag -3.5, so one tumbling -4 piece of debris lit a 51-degree patch of
sky as much as a whole Reflect site. A Reflect site (~150 mirrors at ~-3 in a bin, summed ~6000) gives ~1.1; a single
-4 glint 0. `GpuGlowBuf::peak` (floatBitsToUint max) feeds the HUD's peak magnitude. Harness `state` ->
`satellites.peak_magnitude / sky_glow_max_bin_flux / sky_glow_bins_lit`.

### GpuGlowBuf layout (std430)
```cpp
static constexpr int kGlowBins  = 64;   // 8 azimuth × 8 elevation cells (45° × 11.25°)
static constexpr int kMaxFlares = 8;    // per-satellite lens-flare slots

struct GpuGlowBuf {
    uint32_t bins[kGlowBins];           // atomicMax(floatBitsToUint(effectFlare)) per bin — wide Gaussian glow
    uint32_t flareCount;                // number of entries claimed (capped at kMaxFlares)
    uint32_t flarePad[3];
    glm::vec4 flareEntries[kMaxFlares]; // xyz=ENU dir, w=effectFlare — spiky corona + lens artifacts
};
// sizeof = kGlowBins*4 + 16 + kMaxFlares*16
```
`static_assert(sizeof(GpuGlowBuf) == kGlowBins * 4 + 16 + kMaxFlares * 16)`

`kGlowBins` and `kMaxFlares` must match constants in `sat_sky.frag`. `glowBuf` must be zeroed with `vkCmdFillBuffer` before each `sat_flare.comp` dispatch (floatBitsToUint(0.0) == 0u, so fill value 0 correctly marks bins empty).

---
