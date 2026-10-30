---
pages: [rendering/moon-and-eclipses.md, rendering/atmosphere-and-sky.md]
code: [shaders/sat_sky.frag, src/simulations/SatelliteSimCorona.cpp]
---
## Now
The dynamic corona's fibres are a hand-made mip chain (the noise volume has no mips): the 2.6x octave where resolved,
else two coarser octaves cross-faded by the texels per pixel, the 6.3x filaments only where resolved and inside ~8 R, so
the rays flow smoothly in time warp instead of jumping between frames. The corona reaches 24 solar radii and fades over
its outer half; past ~3 R the smooth background falls away faster than the ray bundles and helmet streamers, so the outer
corona is beams. CMEs fade in over their first hour and out between 12 and 24 R, their texture grows with the bubble,
they push the corona aside (rays swept to the flanks, stretched outward behind the front; halos push radially) and blow
out the streamers they cross, which reform over about a day. A limb CME's existence uses its own launch bin's Kp. With
the Sun hidden (coronagraph or totality) the zodiacal light brightens inward as the F-corona instead of stopping 5-10 deg
from the Sun.

## History
Issue 20261030_141422: rays "extremely choppy" (sub-pixel fibres sampled differently by each sky-TAA jitter phase, the
TAA at its moving weight in time warp), the corona ending sharply at 9 R and too round, a black hole in the zodiacal
light round an occulted Sun (its inner fade was meant to hand over to the glare), CMEs snapping in and out (they read the
current Kp; amplitude ramps of minutes at warp). Cost: a coronagraph view filling the screen +2.2 ms of sky pass (the
corona now covers the whole frame), the 2037 totality at FOV 12 +0.25 ms.
