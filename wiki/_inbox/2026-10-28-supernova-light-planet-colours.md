---
pages: [simulation/sun-moon-planets.md, rendering/moon-and-eclipses.md, rendering/atmosphere-and-sky.md]
code: [src/simulations/SatelliteSimStars.cpp, src/simulations/SatelliteSim.cpp, shaders/sat_sky.frag, shaders/include/cloud_params.glsl, src/simulations/SatelliteSimPlanets.cpp, shaders/planet_disc.frag]
---
## Now
The 2037 Betelgeuse supernova lights the Earth as a second Moon. The Moon and the supernova are folded into one
directional "night key" light: its direction is their light-weighted mean (a Moon below the horizon does not pull it
down), its strength their summed light relative to the full Moon. At its peak of V -11.2 the supernova gives 0.077 lux
against the full Moon's 0.31 (mag 0 = 2.54e-6 lux): a quarter of the full Moon, about a gibbous Moon's light from a
point. Everything the Moon lights takes it: the ground (direct and the moonlit sky's share), the sea's glint path and
foam, the clouds' key light (with their phase, halos and bows), the moonlit sky's colour and the dimming of the Milky Way
and zodiacal light. The Moon's own disc, corona and halo stay on the Moon. With no supernova up nothing changes. The
Planetarium tier and the reflection probes keep the Moon alone. Harness `stars` gives `sn_light_vs_full_moon` and
`night_key`.

Each planet has one true mean colour, used for its point and as the mean of its resolved disc, so the hand-off from point
to disc keeps its colour. Mars is butterscotch, linear (1, 0.62, 0.32), from its full-disc reflectance spectrum under
sunlight (B-V ~1.35); its surface map's own colour is divided out and only its detail kept. Jupiter and Saturn take
their maps' mean colours (which agree with their spectra, B-V 0.87 / 1.04); Mercury, Venus and Uranus flat colours.

## History
2026-10-28 (issue 20261028_222111): the supernova's light had been an isotropic addition to "Night sky light" on land
and cloud ambient only: nothing on the sea or as a key light, invisible from the reporter's mid-Atlantic view.
2026-10-28 (issue 20261028_222744): Mars's point had been a hand-picked vivid red (1, 0.2, 0.2) to tell it from the
stars, and its disc map (as baked) averages a far redder (1, 0.23, 0.11) than the planet; the other planets' point
colours were hand-picked near their disc colours.
