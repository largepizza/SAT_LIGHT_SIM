---
pages: [development/gpu-conventions.md]
code: [src/simulations/SatelliteSimUI.cpp, src/simulations/SatelliteSim.cpp]
---
## Now
Pipelines, render passes, framebuffers and images are not covered by `deferDestroy` (buffers only): code that
destroys one at runtime calls `ctx.drainFrames()` first, because the previous frame (two in flight) may still be
drawing with it. The low-res sky target of the plain render-scale path (`destroySkyLowResResources`) is the
example: `applyGraphicsPreset` and the Render scale stepper drain before recreating it. In a Release run, the
validation layer's `VK_KHRONOS_VALIDATION_LOG_FILENAME=<file>` environment variable writes its messages to a file
(next to `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`); "currently in use by VkCommandBuffer" on a destroy
names this bug class.

## History
Wave 20261122 c29 (2026-11-23): sky TAA off at a scaled preset, then TAA on plus `preset Planetarium` in one frame,
aborted the app silently right after "sky pipeline: LITE" (the preset destroyed the low-res pass under the frame in
flight). The same validation run showed glareAccumImg cleared without TRANSFER_DST usage at init; fixed alongside.
