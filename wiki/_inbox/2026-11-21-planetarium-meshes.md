---
pages: [rendering/hardware-tiers.md, rendering/satellite-meshes.md, using/graphics-settings.md, development/harness.md]
code: [shaders/sat_sky.frag, src/simulations/SatelliteSim.h, src/simulations/SatelliteSim.cpp, src/simulations/SatelliteSimLaunches.cpp, CMakeLists.txt]
---
## Now
Planetarium draws the 3D models: any satellite, station or rocket whose model spans a few pixels is drawn as its
mesh, with the same hand-off from its point, the same shadows, bloom and glare, and follow mode shows the model up
close, as on Low and above. Potato keeps every satellite a point (its sky shader has no mesh composite). In the
tier table, "satellite meshes" is yes for Planetarium and no for Potato; `kNoMeshBits` is the `sat_meshes` knockout
(2097152) and Potato's bit (262144).

What Planetarium leaves out of the models: the environment probes and the sharp mirror reflections (`kNoEnvBits`,
which adds SKY_LITE's bit 524288: both are renders of the full sky shader from the satellite, so a model is lit by
the Sun, the Moon, nearby engine and pad lights and an analytic Earth, and a mirror shows only its Sun glint); the
launch pads' towers and tanks (the tier draws no relief and no pad ground, so they would hang in the air); the Sun's
veiling glare over a backlit model.

The SKY_LITE variant of `sat_sky.frag` is 229,584 bytes of its 230,400-byte budget. The mesh composite added 768
bytes; they were paid for by declaring the twelve variables that only the terrain march writes as `const` in
SKY_LITE (`MARCH_OUT`), which lets glslang drop every branch on them at parse time (1,548 bytes of code that could
never run there). The other variants of the shader compile to the same bytes as before.

Cost on an RTX 3070 Ti at 1600 x 900: with no model on screen the GPU frame is unchanged (4.32 against 4.40 ms with the
mesh path knocked out, six interleaved pairs, at a view from 1,024 km with 405,000 satellite points); following the
ISS from 62 m, with the station across the frame, +0.28 ms (3.82 against 3.55 ms, seven pairs): the mesh pass itself
is about 0.8 ms, and the pixels the station covers save some sky. Not measured on an integrated GPU.

Harness: `state` -> `satellites.meshes_drawn` is the number of models the scene pass drew last frame.

## History
Until 2026-11-21 SKY_LITE compiled the mesh composite out and `kNoMeshBits` held both light tiers' bits, so
following the ISS on Planetarium showed a point and its glare (issue_20261121_160405: "3D models are a necessary
include to this mode"). The SKY_LITE SPIR-V stood 36 bytes under its budget before the change.
