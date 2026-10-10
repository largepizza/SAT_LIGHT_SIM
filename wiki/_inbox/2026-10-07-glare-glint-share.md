---
pages: [rendering/points-bloom-glare.md, development/harness.md, using/settings.md]
code: [shaders/sat_flare.comp, shaders/sat_point.frag, shaders/include/point_style.glsl, shaders/sat_orbit.comp, src/simulations/SatelliteSim.cpp, src/simulations/SatelliteSimHarness.cpp]
---
## Now
**One displayed brightness per satellite.** sat_flare.comp writes a DISPLAYED effectFlare into the visible record:
physical x 10^(0.4 satGainMag) (exposure gain minus the satellite adaptation) x the resolved-object factor x the
formation merge. Point, bloom and glare all read it, so glare and bloom now follow the exposure (they ignored it
before). sat_point.frag adds satGainMag back to the magnitude, because the point-style UBO's reference magnitude
already carries the shift (the same shift dims stars and planets).

**Resolved satellites** ("Resolved satellites", `photometry.sat_resolved`, default 1). A model satellite's sprite
fades from the point-style brightness into its model's own mean surface brightness through the sky exposure and
tonemap: 1 - exp(satPhysK x raw flux / area px), with satPhysK = pi x skyExposure / (kFluxToFlare x brightnessScale x
pixel solid angle). The fade runs over an effective diameter meshPx x sqrt(fill) of 0.8 -> 2.5 px. Fill is a type's
mean projected area / bounding disc (surface/4; StarMind 0.10, Reflect 0.32), carried in the colour's alpha. The
sprite spreads over that size with its drawn flux kept. The point style is white at ~mag 1.5 and a model's pixel at
~mag -7.5, so before this the hand-off window drew a blazing point beside a grey model.

**Adaptation** ("Satellite adaptation", `photometry.sat_adaptation`, default 1). All points lose
max(2.5 log10(1 + S/0.003), 2.5 log10(1 + M/0.01)) magnitudes, eased 0.25 s up and 2.5 s down, capped at 8.
- S = the drawn meshes' light per frame pixel, in sky-exposure units.
- M = the exposure meter's displayed frame mean. It applies above the air only (40-100 km ramp, auto exposure on);
  the ground keeps its own suppression.
- Bright resolved satellites (S 0.02-0.2) also let the auto exposure stop down at night.

**Formation merge** ("Merge formations", `photometry.formation_merge`, default 1). While a cluster is 3 -> 1 px wide,
its slot-0 member draws N x its light and the others fade (sat_orbit.comp; `constIdx` bits 8-15 = cluster size,
16-23 = slot). This affects the drawn point, bloom and glare only; the sky glow and ocean glints keep each member's
own light. The selection panel and the info window (BRIGHTNESS > Formation) show the formation's combined magnitude.

**Glare hand-off.** While a meshed satellite is small, its sprite's glare is kept only for its SUN-LIKE share
(`glintShare`: each lobe's sunlight x smoothstep(150, 1500, pi x mean radiance), mesh_bloom.frag's test), which keeps
the 12 -> 36 px fade. Diffuse, glossy and earthshine light stops glaring by the mesh's fade-in (1.5 -> 3 px). A
Reflect mirror's glint share is 1.

**Harness.**
- New command: `select at <x> <y>` (the click pick at a frame pixel).
- New `state` fields: `selection.photometry` (CPU photometry from the camera with a per-lobe breakdown, plus
  `formation`), `selection.mesh.glint_share`, `satellites.mesh_fade_px` / `mesh_candidates`, and
  `exposure.sat_adapt_mag` / `sat_adapt_lum` / `sat_adapt_frame_mag` / `sat_gain_mag`.

## History
Measured on the user's StarMind snapshots (profile_log 2026-10-07 records 4-8, from 583-1000 km; before/after in
harness_runs/flare_snaps/final_before_after.png):
- **Backlit cluster at 4-5 km** (phase ~173 deg): lit by earthshine (I_sun ~7e-8, I_earth ~0.02, mag -4.2 to -5.5).
  Before, it drew as full starbursts x7 by the user's glare near gain; after, dark silhouettes.
- **Earthlit cluster at 11 km:** before, bright dots; after, faint grey.
- **Lit Antarctic limb view:** before, adaptation was 0; after, it is 2.7 mag and the mag-5 background fades.
- **Ground night view (Glasgow):** resolved factor and adaptation change nothing (noise level).
- **Formation merge's first cut** scaled the physical flux. The glow bins take a MAX per bin, so leaders at 8x
  brightened the whole night sky from the ground (mean +1.7 levels). The merge was moved to the drawn point only.
- **A/B in one harness launch:** a `set` resets the cloud history, so knock out the volumetric clouds first.
- **Mesh glint normalisation:** the kept share was first packed as a half and underflowed for a sun-bright mirror
  (~1e-9). It is now packed as log2.
