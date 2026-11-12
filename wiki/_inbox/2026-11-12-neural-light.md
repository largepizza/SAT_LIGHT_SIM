---
pages: [new: rendering/neural-light-sandbox.md, rendering/clouds.md]
code: [shaders/cloud_v2_march.comp, src/simulations/SatelliteSimHarness.cpp, tools/neural/]
---
## Now
A neural cloud-lighting sandbox lives in `tools/neural/` (branch `neural-light`, not merged). The cloud march
has a `-DCV2_NDUMP` build variant (never shipped) that records 56 floats a half-res grid pixel into march
binding 29: the converged key / ambient / rest lighting summed over the frames of a dump, and the primary
ray's features at its interaction sample (optical depths, heights, sigma, the march's own single-sample key
and ambient terms). Harness `ndump on|off|write <name> frames=N` drives it. `collect.py` builds a dataset from
the user's report shots and perf snapshots (1665 views, ~10 GB), `train.py` trains a residual U-Net
(8.25 M params, width 32, depth 4) that predicts the key and ambient lighting from the features, and
`train.py bench` times it.

## History
Motivated by Leonard & Westermann, arXiv 2611.12103 (U-Net inference of volumetric light transport from one
light march per pixel). Result 2026-11-12: the net reaches 3.7 / 5.8 / 6.0 display levels mean |diff| on
cloud pixels by day (ground / aircraft / orbit; floor ~2), 1.7 at twilight, ~0.2 at night; data scaling
620 -> 1126 views bought 0.5 levels. Inference costs 24 ms at the user's half-res grid in eager PyTorch,
the size of the 34 / 23 / 15 ms lighting loop it would replace. Neither gate met; no in-engine path built.
