---
pages: [rendering/clouds/march.md]
code: [shaders/cloud_v2_march.comp (the first long stride, after the faint stride), docs/subsystems/clouds.md "A ray's first long stride is hashed"]
---
## Now
The march's start offset spans two steps (`t += 2 x step x jitter`). A sample standing for more than one step (rain
alone below a base: four steps) strides a fixed multiple of the step, so a ray whose FIRST sample is such a sample
strides 0.5-1.5 x it, drawn from the stride hash (`cv2StrideU(jitter, 1)`): offset U[0,2] + stride U[2,6] is uniform
modulo 4, so after one stride the lattice's phase is uniform and the cloud base is met at every depth equally.
The weight takes the stride the sample stands for. Only the first stride: hashing every long stride gives the same
image for about twice the cost (rays in rain fall out of step).

## History
2026-11-14 (cluster c03 "zenith rings", the user: "a concentric ring lighting glitch right above you at 67, 67 on
harness starts"). The default start (-67, -67) sits in light rain under an overcast. A ray from the eye strode the rain
four steps at a time from an offset spread over two, met the base at half the lattice's phases, and the bias changed
with the base's distance: bright/dark rings concentric round the zenith at sunset, twilight and midday (white-noise
jitter: the same rings; a constant jitter: sharp contour rings; spacing followed `step_base_m`). Ring amplitude at three
times 0.35 / 0.78 / 0.67 -> 0.18 / 0.20 / 0.14 levels (a 20 m-step reference 0.26 / 0.26 / 0.27, the scene's own
structure); ~+0.2 ms GPU at that view. A deterministic first stride from `jitter` left fine rings (the step grows and is
capped, so the offset is not exactly two of the first sample's steps).
