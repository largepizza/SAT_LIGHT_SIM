---
pages: [rendering/cities.md, rendering/clouds/weather.md, rendering/points-bloom-glare.md, rendering/satellite-meshes.md, rendering/atmosphere-and-sky.md, rendering/moon-and-eclipses.md, simulation/sun-moon-planets.md, using/graphics-settings.md, rendering/hardware-tiers.md, development/harness.md]
code: [shaders/sat_sky.frag, shaders/cloud_v2_march.comp, shaders/glare_find.comp, src/simulations/SatelliteSim.cpp, src/simulations/SatelliteSimUI.cpp, src/App.cpp, src/simulations/SatelliteSimHarness.cpp, tools/harness/glare_track.py]
---
## Now
- **City lights under cloud are not blurred.** The lights are attenuated by the cloud composite like everything
  behind it; the clouds carry the cities' glow themselves (city light on clouds). The "City light blur LOD" slider
  and `clouds.city_light_blur_lod` are gone (slot 62 free; the UBO field `cityLightBlurLod` is unread).
- **The air's city glow** (sat_sky.frag atmosphere loop) reads the night map at mip 4 from the ground and aircraft;
  from 800-3000 km up it blends to a mip that follows the sample's height (log2(3 h / 4.9 km), ~1.5 at 8 km), so
  from high orbit a town's glow is a halo the shape of the town, not a ~150-km disc.
- **Dust seen from above** fades to 8% of its density by 60 km of eye altitude (was 25% by 80 km). Aircraft
  heights unchanged.
- **Mesh glare position is sub-texel**: glare_find.comp fits a parabola through the log light of the peak texel and
  its neighbours on each axis (exact for a Gaussian lobe), blended toward the 9x9 near-peak centroid where the top
  is flat. The CPU sprite placement on the Sun's image (review 28) clamps to the lobe as a square along the group's
  in-plane body axes (was a circle of half the side).
- **Settings**: "Exposure (EV)", "Auto exposure (day)", "Highlight roll-off", "White balance" and "Moon size (x real)"
  are in the Photometry tab's EXPOSURE AND MOON section (keys and slots unchanged: `clouds_v2.exposure_ev` etc.,
  `clouds.moon_size`).
- **Startup chooser** uses the HUD palette (UIPalette.h): near-black cards with the faint border, the red accent
  for the selection, checkbox and Start; the amber note on the highlight row colour.
- **Harness**: `state` -> `selection.glints` lists the main view's mesh glints (px, effectFlare);
  `tools/harness/glare_track.py <captures> <prefix> [--plot]` compares them with the Sun's image
  (`selection.mesh.sprite_px`); `scripts/mirror_glare.satcmd` (release cinematic shot 6), `scripts/city_orbit_blobs.satcmd`.

## History
- The blur mixed the lights toward the night map at mip 8.1 (~1000 km/texel) by the cloud opacity in front, so from
  orbit every city under thin cloud was a grey wash with the cloud's outline (the user: "blobby" cities in the
  release cinematic's pull-out). Europe from 3000 km: mean |diff| 1.5 levels, the wash gone.
- The air glow's height LOD applied from 30 km up drew 10-20 km texels as squares around towns from 400 km
  (bilinear): restricted to 800-3000 km.
- Mirror glare: the glint list held quarter-res texel centres, so in the mirror shot (500 -> 160 m dolly) the glare
  held still, jumped 4 px, held again. After: steps 0.1-2 px and within ~1 px of the Sun's image as rendered
  (low-exposure captures). The CPU sun-image placement was 25 px off at 500 m (circle clamp), 11 px after (square).
- The chooser's first cut had its own soft blue theme (the user: "no soft blue UI windows").
