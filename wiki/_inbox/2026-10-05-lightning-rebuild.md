---
pages: [rendering/clouds/weather.md, sound/ambience.md]
code: [shaders/cloud_v2_lightning.comp, src/simulations/SatelliteSimLightning.cpp, shaders/lightning.vert, shaders/lightning.frag, src/AmbientSynth.cpp]
---
## Now
Whole storms produce lightning. The GPU schedules flashes from two sources:

- the Cb towers ("Lightning (flashes/min/tower)");
- a lattice of ~20 km storm cells over every deep-convective, raining region of the weather map ("Storm lightning
  (flashes/min/cell)", `clouds_v2.lightning_storm_rate`, default 2).

Flashes come in four kinds: cloud-to-ground (about 30% where the storm is strong), in-cloud, spider lightning just
under the cloud base, and red sprites. The schedule is deterministic in sim time: a 50 ms stepped leader, then 1–4
return strokes.

The host builds each flash's channel once as a fractal tree from its seed. A ground stroke leaves the cloud's flank
and comes down slanted to a point 1.5–9 km aside, with branches, twigs and many dim tendrils. A spider is a web of
near-level arms 6–30 km long. A sprite has 20–45 forking tendrils. "Lightning tendrils"
(`clouds_v2.lightning_tendrils`, default 1) scales the branching.

The channels are drawn after the sky TAA at full resolution, at their true luminous width (3 m for the main channel).
A channel thinner than a pixel is drawn one pixel wide with its energy kept, so it gets dimmer, never fatter. Each
channel has a halo of light scattered by the rain and air around it. Channels and glow are screen-blended, so
overlapping light saturates instead of clipping. Cloud and rain in front hide a channel.

The cloud lights up around each flash with a ~4 km diffusion profile; from orbit a flash is a soft 10–20 km patch.
The air between the eye and the flash is counted as an air column, not a fixed haze distance. A close strike also
lights the raindrops at the eye.

Thunder comes from the channel geometry. The sound of each segment arrives at its distance ÷ 343 m/s, and the binned
arrivals form the roll's envelope. The sound is low-passed by how far it has travelled, and a strike closer than
~3 km opens with a crack. Thunder is heard within 35 km.

The lightning draw costs about 0.3 ms.

## History
This replaced `cloud_march.comp`'s `lightningCS`, which had three problems:

- Only the dominant anvil-reaching towers flashed, so a map-typed storm without such towers had none (user snapshot 43).
- The channel was 14 vertices with a 3 m minimum width, drawn inside the half-res composite and smeared by the sky
  TAA: pudgy, straight down.
- The glow and bolt saturated.

Thunder used to be a synth roll with random peals.

A harness command, `lightning spawn`, places flashes by hand for review.
