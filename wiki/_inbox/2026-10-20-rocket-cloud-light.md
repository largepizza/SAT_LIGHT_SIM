---
pages: [simulation/launches.md, rendering/clouds/lighting.md]
code: [shaders/lightning.frag, shaders/include/lightning_draw.glsl, shaders/cloud_march.comp, src/simulations/SatelliteSimLaunches.cpp, src/simulations/SatelliteSimLightning.cpp, src/simulations/SatelliteSimKessler.cpp]
---
## Now
**Rocket light on clouds, haze, exhaust and the ground.** At night a burning rocket engine lights everything
around it: cloud bases and fog orange, the haze along each line of sight, its own exhaust trail, and the ground
and sea near the pad. Every burn counts (ascent, boostback, entry and landing burns), as does a returning ship's
entry glow (faint: it is 45-100 km up) and each launch pad's floodlights (on low cloud and fog only).

Each source is a point light. Its intensity comes from the flame's magnitude seen from 100 km
(`flame1_mag_100km` / `flame2_mag_100km` in launches.json; landing burns +1.5 mag, boostback and entry +0.7):
a Super Heavy (-6) is about 6e6 cd, which gives a cloud base 3 km up about 0.7 lux, roughly three full Moons.
At night the light is raised by the same factor the Moon's light is (moonGain / the full Moon's physical
irradiance, about 2600x), because the night exposure is not physical. By day it is physical, so nothing shows.
A pad's floodlights count as 0.5 (Starship pads 0.8) Sun-irradiance at 1 m, about 5e4 cd going upward.

What each light does:
- clouds and fog (the half-res cloud composite): albedo 0.85, falling as 1 / (r^2 + r0^2) with r0 about half the
  flame's length; 0.35 of that when the light is on the far side of the cloud (diffuse transmission through it);
  less on thin edges (x opacity^1.5);
- the ground and sea (the half-res scene depth): albedo 0.15 x the cosine to the light, behind the clouds'
  transmittance, never on the rocket's own hull (pad floodlights skip the ground, which the pad pattern lights);
- haze along the ray: single scattering by air and low haze (scale heights 8 km and 1.5 km) at the light's height,
  integrated in closed form;
- the exhaust plumes (in the cloud composite): each parcel scatters the light by its albedo.

The light's colour is the flame's pushed toward deep orange (sooty fuels deeper). Each light fades out over the
outer 40% of its reach (where a lit white cloud falls to 2e-3 of white on screen). At most 32 lights a frame, those
whose lit region is nearest the eye first.

Setting: Settings > Constellations > LAUNCHES > "Rocket light on clouds" (Off / x0.3 / x1 / x3,
`launches.engine_light_gain`, default x1).

Measured (RTX 3070 Ti, 1600x900, Starship from SLC-37 at T+15 s under broken cumulus, 6 km from the pad): the
light pass +0.26 ms, the plume lighting within noise, GPU frame 12.2 ms on vs 12.8 off (noise).

Harness: `launch state` -> `launches.engine_lights` (distance, height, intensity, reach of each light) and
`engine_light_gain`. Scenes: Starship night launch from SLC-37 with `drift phase=2.86` (broken cumulus) or
`5.2` (overcast) and `clouds_v2.coverage 1.6`; `launch landing booster=1` for a night booster catch.

Not modelled: the light does not know the cloud surface's orientation (the screen derivatives of the half-res
distance drew a crosshatch); a light inside a cloud does not diffuse through it as lightning does; terrain does not
shadow it; the lit cloud's outline can show the half-res texels where the light is strong.
