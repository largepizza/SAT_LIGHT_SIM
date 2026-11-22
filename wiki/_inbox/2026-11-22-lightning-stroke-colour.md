---
pages: [rendering/clouds/weather.md, development/harness.md]
code: [shaders/include/lightning_draw.glsl, shaders/lightning.vert, shaders/lightning.frag, src/simulations/SatelliteSimLightning.cpp]
---
## Now
A lightning stroke is drawn in the colours a photograph shows, each from its physical source. The core is the return
stroke's ~30,000 K plasma (singly ionised nitrogen's blue-violet lines over a continuum, H-alpha), hue (0.68, 0.62,
1.0): the tonemap works per channel, so a near trunk clips to white and a thin or far piece keeps its hue. A violet
corona sheath (N2 / N2+ bands, hue (0.55, 0.18, 1.0)), a Gaussian 6 m wide (x 0.7 per branch order), takes 5% of a
main channel's light, x 2.2 per branch order, and 60% of a stepped leader's: branches and leaders are purple. The
light scattered by the air replaces the old Gaussian halo: each piece of channel adds the integral along itself of
1 / distance^2, which summed along the channel falls as 1 / distance; indigo in clear air (0.30, 0.13, 1.0), the
stroke's own colour with twice the energy and a three times wider core where rain or cloud stands in front of it
(read from the cloud composite per pixel). The air between the stroke and the eye takes the blue first (the grey
transmittance to the powers 0.58 / 0.99 / 2.0 per channel), on the channel and on the flash-lit cloud, whose light
is (0.68, 0.64, 1.0). Every hue is given the luminance of the colour it replaced, and the glow the old halo's energy.
One setting mixes all of it: Settings > Weather > Lightning > "Lightning glow colour" (`clouds_v2.lightning_colour`,
0-2, default 1; 0 = the earlier near-white stroke exactly, 2 = more saturated). The hues, shares and radii are live
in the harness: `lightning color [gain=] [core=r,g,b] [sheath=r,g,b] [sheath_share=] [sheath_order=]
[sheath_leader=] [sheath_m=] [sheath_taper=] [glow=r,g,b] [glow_share=] [wet=r,g,b] [wet_share=] [wet_wide=]
[wet_floor=] [wet_gain=] [air=r,g,b] [cloud=r,g,b] [reset=1]`. Sprites keep their own colours.

Measured (RTX 3070 Ti, 1600x900, a ground stroke held at its first stroke on a clear night): 5 km off, the ring 2-6
core widths from the clipped centre is (174, 144, 218), saturation 0.34 (before: (220, 222, 235), 0.07); branch
pixels' saturation 0.27 (0.09); blue / red of the unclipped glow 1.57 at 5 km, 1.27 at 15 km, 0.96 at 45 km (1.18
at every distance before); the stroke's box keeps 97.5-105% of its mean luminance and the flash-lit cloud 99.8%. No
measurable cost (one stroke held: 18.04 +- 0.14 -> 18.06 +- 0.08 ms GPU, the setting at 0 / 1 interleaved).

## History
Until 2026-11-22 the core was (0.90, 0.93, 1.0), the halo a Gaussian of (0.66, 0.70, 1.0) and the cloud's light
(0.80, 0.86, 1.0): after the tonemap a stroke and its halo read white (issue_20261121_163317's follow-up: "a dull
indigo lighting around lightning bolts" in photographs). Not modelled: the pinker channel of a stroke in heavy
rain; the glow reaches only as far as the old halo's quads (175 m round a main channel).
