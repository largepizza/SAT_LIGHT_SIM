---
pages: [rendering/satellite-meshes.md, rendering/hardware-tiers.md, accuracy/results.md, accuracy/satbench.md]
code: [shaders/sat_orbit.comp, shaders/sky_taa.comp, shaders/sat_sky.frag, shaders/sat_mesh.frag, src/simulations/SatelliteSim.cpp]
---
## Now
A satellite close to the camera is placed by the CPU in double precision. The orbit shader lists every satellite
within 20 km of the eye; the next frame the CPU evaluates the nearest 128 of them, and always the satellite the
camera rides with, relative to the same eye the 3D models are drawn from, and the shader uses that vector in place
of its own float subtraction (which carries the float32 floor, about 1 m median, whatever the range). A point and
its model therefore agree to well under a millimetre close in, and where a point is drawn with no model in its
place (the Potato preset, a satellite type with no model, a model still too small on screen) a satellite ridden at
11 m holds still: its point moves 0.00 px between frames where it used to jump 59 px mean / 149 px max. Each
dispatch of the orbit shader tags its header, so the GPU/CPU parity check compares the same frame (harness `state`:
`selection.gpu_dir_err_deg`, `satellites.near_listed` / `near_exact` / `near_radius_m`).

A 3D model holds still while the camera rides with it, at any render scale. Under temporal upscaling (render scale
below 100%) a model's samples are placed where they were read (the centre of the low-resolution pixel), not at the
frame's jittered position, and an open lattice (a truss, a grid fin) on a model that moves against the ground keeps
one fixed per-pixel coverage threshold instead of a new one every frame. The stepped threshold remains for
ground-fixed lattices (a catch tower), where temporal AA has a history to average it.

## History
Issues 20261121_154126 ("jitter on models, especially trusses") and 20261121_160405 shot 1 ("satellites jitter
around when you are up close", Planetarium, which drew points only when the report was taken). Measured riding 62 m
from the ISS at render scale 0.67: the truss image shifted 0.67 px mean / 1.07 px max between frames (0.06 px at
100%, 0.004 px with temporal AA off; now 0.003 / 0.005), the crop changed 9.4 levels mean a frame (now 0.36), and
its edges redrew every frame at 100% as well (the lattice threshold stepping with no history to average it: temporal AA
reprojects a static world, and a model the camera rides with moves 100+ m a frame against it). The camera itself
was steady against the satellite in both reports (3e-11 of the offset a frame). A first cut that placed only the
model's own samples at their centres, and left the background samples between a truss's members at their jittered
places, still shifted 0.37 px mean / 0.60 px max: whether a sample is model or background is decided at the centre
too, so every sample beside a model is placed there.
