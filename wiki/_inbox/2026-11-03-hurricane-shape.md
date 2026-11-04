---
pages: [rendering/clouds/storms.md, simulation/weather.md]
code: [src/simulations/Weather.cpp, src/simulations/SatelliteSimWeather.cpp, shaders/cloud_v2_weather.comp, shaders/include/cv2_storms.glsl]
---
## Now
A tropical cyclone's shape is controlled by `wx::CycloneShape` (Settings > Weather > Storm objects, slots 455-461,
`clouds_v2.storm_cyc_*`): main arms (default 2; 0 = by intensity, 2 -> 5.4), arm tightness (x the pitch's cotangent),
arm speed (x bandRot), minor swirls (2n + 1 finer bands at 0.8 the pitch, broken), deck spiral (0.8), outflow cirrus
(x strength, capped at 0.4, and reach) and outflow streaks (0.6). `wx::applyCycloneShape` applies count / pitch / speed
to the cyclone states before the band towers are placed, so towers, the weather bake and the upper deck agree.
In the weather bake an arm keeps the angular width it had at ~4 arms (b^p, p = (4/n)^2) and its gaps narrow it to
nothing along its own length; every angle term is a whole multiple of theta (no seam at atan's cut) and no smoothstep
has edge0 > edge1 (NVIDIA inverts it: straight-edged filled wedges). The CDO's low deck reaches out along the arms (radius x (1 + 0.8 k (0.5 - band))). The
upper deck is the CDO shield (outer part notched between the arms) plus ONE cirrus-deck disc per cyclone whose shader
(`cv2CycSpiral`, applied to the strongest shield of a sample only) draws the arms' cirrus on the low arms' own log
spirals, the minor swirls, and outflow cirrus in 3n + 2 anticyclonically curving streaks. The spiral is packed into
the shield record (c.w < 0, d.xyz). `clouds_v2.storm_cyc_spiral false` restores the old deck (A/B only).

## History
Until 2026-11-03 (issue 20261030_205220) the arms were cut by a multiplier of (ln r, theta) waves whose zero lines
crossed each arm at an angle (blunt, square arm ends: "chopped in rectangular ways"), the count rose with intensity to
~5 thin uniform arms, the CDO and its deck were round, and the upper deck was an outflow disc plus 4 straight capsules
per arm (a polygon of bars stepping in brightness at every joint). Shot 2 of the issue, azimuthal luminance round the
eye: before, no consistent harmonic inside the CDO (h2 0.010-0.019 of the mean); after, h2 dominates from 120 to 280 px
with its phase drifting -169 -> +36 deg outward (a log spiral). March registers unchanged at 128.
