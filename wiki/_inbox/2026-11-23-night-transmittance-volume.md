---
pages: [rendering/clouds/march.md, rendering/atmosphere-and-sky.md, development/harness.md]
code: [src/simulations/SatelliteSimCloudsV2.cpp, shaders/cloud_v2_lightning.comp, shaders/cloud_v2_march.comp, shaders/cloud_march.comp, shaders/sat_sky.frag, shaders/include/clouds_v2.glsl]
---
## Now
The cloud transmittance volume (the long-range shadow store) holds the Sun's light while the Sun is above -5.7 deg at
the eye, and otherwise the night key's: the brighter of the Moon and the 2037 supernova (each by its light times its
horizon gate), never their averaged direction. Below 1% of the full Moon there is no night volume. Moonlit clouds are
shaded by cloud beyond their own ~2 km light march, the clouds' shadow falls on moonlit ground (only where the Sun is
below that ground's horizon), and moonlit faint haze and rain take the same no-light-march paths as sunlit ones. The
Sun's consumers (god rays, fog, sea reflection, plumes) still read only the Sun's flag, so nothing Sun-lit takes a
Moon shadow. Setting `clouds_v2.night_volume` (default 1, no slider); harness `state` -> `clouds_v2.sun_volume.key`
(0 Sun, 1 Moon, 2 supernova). The deep-light shortcut now runs for any key light.

## History
Until 2026-11-23 the volume was off at night and the Moon saw only its light march (no far shadows, no ground shadows,
no shortcuts: moonlit samples were the most expensive). Measured on an RTX 3070 Ti at 1600x900: under a moonlit deck
from 1.3 km 22.4 -> 20.3 ms GPU, 3780 km over the night side 37.9 -> 35.7 ms, daytime unchanged. Gating deep light on
the night volume pushed the cloud march from 128 to 255 registers, so it is ungated.
