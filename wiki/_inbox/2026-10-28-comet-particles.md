---
pages: ["new: simulation/comets.md", rendering/sky.md, using/sky-events.md, using/camera.md, development/harness.md]
code: [src/simulations/Comets.h, src/simulations/Comets.cpp, src/simulations/SatelliteSimComets.cpp, src/simulations/SatelliteSimInertial.cpp, shaders/comet_splat.vert, shaders/comet_splat.frag, shaders/comet_composite.vert, shaders/comet_composite.frag, shaders/include/comet_frame.glsl]
---
## Now
Comet tails are 3D particles in heliocentric space (comet::buildParticles). Dust grains are released on a fixed
absolute grid of emission times (slots uniform in asinh((te - tPeri) / tau0): fine round perihelion, coarse far from it;
8-128 grains a slot, ~50,000 for the Great Comet), each with a radiation-pressure ratio beta (stratified in ln beta,
0.002-0.95, cross-section weight beta^0.5 exp(-(beta/0.55)^2)) and an ejection velocity (~80% into the sunward
hemisphere, 0.5 km/s x sqrt(beta) x r^-0.5), flown under the Sun's gravity x (1 - beta) by the universal-variable Kepler
propagator. Syndynes, synchrones, the sungrazer's hooked perihelion synchrone and an anti-tail (the Earth crosses the
Great Comet's orbital plane at its closest approach, 2037-03-17) come out of the physics. Ion parcels (every 0.01 day,
12 a slot, up to 2.5 days old) are picked up at the nucleus and carried radially by the solar wind (accelerating to
~250 km/s over ~3.6 h) in five streamers whose kinks are frozen into the plasma, so the ion tail points anti-sunward,
aberrated by the comet's motion, and reorients round perihelion. A build is a pure function of its time on a grid
(step ~0.003 of the nucleus's dynamical time, a power of two in seconds, 4 s .. 2.3 h), made on a worker thread (on the
frame after a jump or with `comets.particles_sync`); the GPU carries each particle to the frame's time by a Taylor step,
so any time warp shows the same tail. The particles are splatted as Gaussians of their own spread into a half-res
RGBA16F target: dust light = weight x the Halley-Marcus phase at the grain's own scattering angle / (r^2 Delta^2), ions
blue and unphased; the composite adds an analytic Haser coma (1/rho x exp(-rho/rho_s), round, no edge, white inside,
C2 green outward by the gas share), then extinction, the dark-sky gate, cloud occlusion and a soft shoulder. Cost
(RTX 3070 Ti, 1600x900, the closest approach at a 120 deg field) ~0.5 ms GPU for the splats, a build ~15-20 ms on a
worker.
Inertial camera lock (Sky events window "Lock view to the stars", harness `inertial`): the view is held fixed relative
to the stars (ECI); from above 100 km the eye is held too, so the Earth turns beneath. "Occult the Sun" (while locked)
hides the Sun's disc, glare and bloom like a coronagraph and gives the night exposure from space. Stars seen from above
the air are no longer gated by the twilight sky.

## History
Replaced the dust syndyne/synchrone SHEET (a grid of sky directions): at the 2037 closest approach it drew hard
stair-step bands, a hard-edged fan and an oval green coma (issue 20261028_192308 "Comet Graphics"). Particles wider than
the splat cap first had their light squeezed into the capped size and drew as bright out-of-focus discs passing the
eye; their light is now faded instead. Sampled-range edges (the highest beta, the oldest emission) drew hard lines
until faded. The dust normalisation is 10x lower than the sheet's (dust flux 0.025 x Fhead0 x dust per rate-day).
