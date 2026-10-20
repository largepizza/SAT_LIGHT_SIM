---
pages: [simulation/launches.md, rendering/clouds/lighting.md, rendering/satellite-meshes.md]
code: [shaders/cloud_march.comp, shaders/lightning.frag, shaders/include/cloud_occlusion.glsl, shaders/sat_sky.frag, shaders/include/terrain_detail.glsl, src/simulations/SatelliteSimLaunches.cpp]
---
## Now
**Following a rocket at night.** A rocket's exhaust is lit by its engines once: in the cloud pass, where each
exhaust Gaussian's engine light is self-shadowed by its own mass (the side of a thick column facing the flame
glows, the rest takes about a quarter), and a Gaussian the camera is inside thins out (from 3 to 1.5 sigma, down
to 10%). The engine-light pass that lights clouds, fog and haze skips the exhaust: the cloud composite marks a
texel whose distance is a plume's or flame's rather than a cloud's (the half float's lowest mantissa bit). It
lights a cloud at a smoothed distance (a 3x3 tent of the half-res texels, weighted by opacity), so a strongly lit
deck has a smooth outline. A burning stage's hull and its exhaust are composited with a soft depth test.

The eye's night adaptation to the engine lights snaps on a cut: a sim-time jump of more than 2 s, the camera
moving more than max(300 m, a fifth of the distance to the nearest light) in one frame, and through a Go-to flight.

**The Info window's 3D view near the ground.** Its background (the environment renderer from the view's camera)
takes the city lights' close-up mean (0.12 of the night map until the pixel footprint reaches the street layout's
range), as the main view's procedural cities average, and ignores the data that is in the main observer's frame
(the exact DEM near the observer, the launch sites' coast patches and pad grading).

## History
2026-10-20 (QA sweep follow-up). Following a Starship 300 m off at night showed a bright ball ~300 m across: the
exhaust was lit twice, once in the cloud pass and again as if it were a cloud at its own distance in the
engine-light pass. The same double lighting made far horizon clouds read as darker half-res blocks inside the glow
(their texels kept their own, far distance). The first ~0.5 s after a jump beside a stage was a white-out (the
adaptation eased from the dark). The Info view of a rocket on the pad at night was a flat yellow-beige: the ground
under it took the 5-km night map's light around the Cape at full strength. Measured cost (RTX 3070 Ti, 1600x900):
cloud pass +0.05 to +0.25 ms.
