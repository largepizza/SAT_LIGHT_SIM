---
pages: [rendering/clouds/march.md, simulation/reflectors.md, rendering/terrain.md, rendering/sea.md]
code: [shaders/cloud_v2_beamvol.comp, shaders/include/beam_vol.glsl, shaders/cloud_v2_march.comp, shaders/sat_sky.frag, src/simulations/SatelliteSim.cpp]
---
## Now
The Reflect-Orbital beams' light is baked once a frame into a light volume per busy site (up to 4: an 80 x 80 km box,
-2.5 to 15.5 km, fixed in the site's frame; round 2: +-200 km with asinh-spaced columns, 0.58 km at the site to ~10 km
at the walls, 281 m levels). Every converged beam on the site gets its own cloud
shadow: 20 x 20 rays along its own direction through a density bake of the box. The clouds read the volume once per
sample (plus three short reads up the beams for their own lobes), the air's shafts are a 160 x 90 x 64 screen-depth
volume integrated from it, and the ground and sea are lit from it as the Sun lights
them (albedo x irradiance x cosine, the sea's water light and a faint sky reflection), at the same scale as the clouds
("Beam light on cloud"). Mountain ranges shadow low beams (the rays stop in the terrain, ~2.7 km elevation pixels).
Each cell also holds the light toward the eye with every beam at its own scattering angle (the air's Mie lobe, the
clouds' two lobes), so the clouds toward the satellites rim-light and the air glows toward each mirror. Past a box each
beam's air continues from where it leaves the box. Shadows are soft: a site is lit by 100-200 mirrors from all over the
sky (half its light more than 20-35 degrees from the mean direction), so a cloud's shadows from the different beams land
kilometres apart; a single beam's shadow is distinct (harness `debugview beamground`, march debug view 8).
Setting `clouds_v2.beam_volume` (default 1; 0 = the old clustered lights and the grey spot).
Measured (RTX 3070 Ti, 1600x900): the bench report's views 33.5 -> 31.3 and 36.7 -> 34.2 ms, orbit over the sites
22.6 -> 19.9 ms, daytime unchanged.

## History
Until 2026-11-24 converged beams were merged into one light per site and 15-degree direction cell; each cloud sample
looped over its tile's lights, the shafts were a per-pixel ray-cylinder integral per light shadowed only by one
"blocked above altitude X" number per beam, and the ground got a flat grey added to its colour (no albedo, relief or
cloud-shadow pattern). Reusing the Moon's key-light volume was rejected (one global shadow direction; the Moon is the
key at a full Moon). Tried and not kept: reading the volume at the sample with one short read (a flat, over-bright deck)
and reading it 2 km up the beams (lit cloud bases beside slanted beams).
Round 2 (2026-11-24, issues 20261124_085028 / _085151): the first +-40 km boxes drew as rectangles in the sky and on the
deck (low beams leave them below 15.5 km), and one irradiance-weighted direction per cell spread a satellite's silver
lining over the whole deck. Cost now (1920x1009, knockout delta): 3.8-4.1 ms at the bench report, 1.3-2.1 ms at the two
new issues' views.
