---
pages: ["new: simulation/comets.md", rendering/sky.md]
code: [src/simulations/Comets.cpp, src/simulations/SatelliteSimComets.cpp, shaders/comet_splat.vert, shaders/comet_field.frag, shaders/comet_composite.frag]
---
## Now
The ion tail is CO+ deep violet-blue (linear (0.05, 0.17, 2.1)); its five streamers are each 0.008 of their distance
from the head wide, and each one's brightness varies along it with the emission time (knots and gaps ~0.3 day apart
that ride out with the plasma, 0.25-1.75 x, mean 1). The composite's shoulder on the brightest channel is
K (1 - (1 + x / (K p))^-p), K 0.92, p 0.5: slope 1 at 0, and a decade of brightness still shows near white. The dust
tail's zoom detail takes its level of detail from a second field target (RGBA16F): the light-weighted magnitudes of the
field's screen gradients per full-res pixel; each grain's Gaussian in the field atlas is at least
`comets.field_floor_px` (0.5, no UI) field pixels, and the detail never removes more light than the colour atlas holds.
The coma fades out over 5-12 scale lengths and turns faintly C2 green outward (0.3 x the gas share). The head and coma
count as below the horizon only under the Earth's limb seen from the eye (then the refraction table). The field pass
costs 0.4-1.1 ms in issue 20261030_140112's views (1920x1009, RTX 3070 Ti).

## History
Issue 20261030_140112 (views from 23,575 km): the ion tail was a pastel cornflower wash (colour (0.16, 0.42, 1.95),
streamers 0.025 wide merged into one band, the old x / (1 + x / 0.9) shoulder flattened it); the dust trail showed
blocky "pixel noise" and black notches near the head (the LOD came from dFdx of the bilinear field, constant per field
texel); the head and coma vanished from orbit and popped back green as the view turned (a fixed z < -0.3 horizon test;
the limb dips 77 deg at that altitude); a bright coma ended in a hard circle (a cut at x = 12). A second instanced field
pass for the gradients cost 2-3x the field pass, a second RGBA32F target ~2x; RGBA16F keeps it at ~+0.3 ms.
