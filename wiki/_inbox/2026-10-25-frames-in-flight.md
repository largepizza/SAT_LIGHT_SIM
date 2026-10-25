---
pages: [development/gpu-conventions.md, development/architecture.md, development/profiling.md, development/harness.md]
code: [src/VulkanContext.h, src/VulkanContext.cpp, src/App.cpp, src/simulations/SatelliteSim.cpp, tools/harness/frame_bench.py]
---
## Now
The app keeps two frames in flight: the CPU records frame f while the GPU runs frame f-1, so a frame costs
max(CPU, GPU) rather than CPU + GPU. `SATLIGHTSIM_FRAMES_IN_FLIGHT` (1 or 2, default 2) selects the mode at launch, so
one build can A/B both. The GPU still runs frames one after another: each frame's GPU work starts with a full memory
barrier, so device-local resources need nothing. Only memory the host touches is handled differently:
- buffers the CPU rewrites are mirrored: a CPU shadow is copied in by a per-slot upload command buffer, either whole or
  by the dirty ranges the code marks with `mirrorDirty`;
- staging, the UI's vertex buffers and host readback targets are slotted (one copy per frame slot);
- GPU results the CPU reads come from per-slot HOST_CACHED readback copies.

Readbacks (picking, peak magnitude, the exposure meter, the beam list) are two frames old instead of one. One-shot
readbacks (radar, harness probe, photometric check, screenshots) wait until `completedFrame()` reaches their frame.
Descriptor writes and resource recreation at runtime drain the frames in flight. A steady frame does neither: the
harness `perf` -> `frames_in_flight` block shows ~400 KB uploaded, 0 drains and 0 descriptor writes per frame.
`tools/harness/frame_bench.py` measures wall-clock frame time in the matrix suite's scenes (uncapped present) for
stills, time running and player-speed motion paths.

## History
This replaced a single command buffer with a fence wait at the start of every frame, which left the GPU idle while the
CPU recorded. Measured on an RTX 3070 Ti at 1920x1009 (two runs a side, `harness_runs/fb/`), wall-clock frame time:
- summed over the stills: 839 -> 767 ms (-8.6%);
- summed over the motion paths: 1404 -> 1330 ms (-5.3%);
- orbit -7..-16%, launch -13..-15%, 1 h/s -10..-15%, ground views -2.5..-6%.

The new build in single-frame mode matches the old build (within 1%). The GPU's own time rises ~2% with two frames in
flight (it never idles, so it boosts less). Matrix suite, one frame vs two in one build: the image, motion and grain
metrics agree within 0.01-0.1 levels. A probe of a deferred split of `sat_sky.frag` (a surface pass and an
atmosphere pass) found both halves still at 168 registers. The split alone is about neutral (~2.6 ms of shared work at
3 km); the atmosphere loop (~4.4 ms on the ground) is the part worth moving into a volume.
