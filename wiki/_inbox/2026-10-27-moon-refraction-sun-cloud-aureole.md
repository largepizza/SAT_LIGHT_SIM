---
pages: [rendering/refraction.md, rendering/moon-and-eclipses.md, rendering/atmosphere-and-sky.md]
code: [src/simulations/Refraction.cpp, src/simulations/SatelliteSimRefraction.cpp, src/simulations/SatelliteSim.cpp, shaders/sat_sky.frag]
---
## Now
The refraction table's duct test counts the ray's path IN the air (4000 km), not from the eye. From orbit the straight
leg to the top of the air no longer counts against it, so limb-grazing rays are traced through and the Sun (and Moon)
are refracted and flattened at the limb instead of cut off above it.

The Moon is drawn through the same refraction table as the Sun (full sky variants): each pixel's apparent elevation,
jittered by the same angle-of-arrival turbulence, maps per colour channel to the direction the light came from; a
channel whose ray ends on the ground sees nothing; the extinction is the bent path's air columns plus the haze near
clouds toward the Moon. The Moon's glow is centred on the Moon as seen, and the stars hidden behind the Moon use its
refracted position. SKY_LITE and the environment probes keep the straight ray.

Cloud in front of the Sun: the disc is dimmed by the cloud's transmittance and fades out as the stored transmittance
falls from 0.1 to 0.02 (below that the march has stopped: optically thick). The cloud between the eye and the Sun adds
its forward diffraction glow (the aureole): pi L / E = 0.25 / sigma^2 x tau e^(-tau) x a Gaussian of the angle from the
Sun (sigma^2 = 0.012^2 + half the drawn disc radius squared), times the Sun's air transmittance. A thin cloud across the
Sun glows white round it with the disc showing through.

## History
2026-10-27, issue 20261027_185311 "Sun refraction at intense and far angles": from 790 km the Sun was cut flat 0.8 deg
above the drawn limb. The duct test counted from the eye; every ray grazing the limb read as ducted (408 of 512 table
entries "hit", apparent horizon -26.34 deg against a geometric dip of -27.17).

2026-10-27, issue 20261027_184332 "Moon Refraction": the Moon was a round, unrefracted disc (only the Sun and stars went
through the table).

2026-10-27, issue 20261027_182140 "Sun occlusion on far clouds": the disc was hidden only by a binary >= 90% opacity
flag per half-res texel (speckle along a thick cloud's edge) and drawn at full brightness through anything thinner,
with no forward glow on the cloud, so thin cloud crossing the Sun read as behind it.
