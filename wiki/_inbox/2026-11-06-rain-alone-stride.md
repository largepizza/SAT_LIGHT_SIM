---
pages: [rendering/clouds.md]
code: [shaders/cloud_v2_march.comp]
---
## Now
The cloud march shades a coarse sample that holds rain alone as one sample of four steps, and "rain alone" means below the
cloud base: a sample with no cloud body under it (`f.rain > 0.9 && f.hf <= 0`). The 400-m rain floor over a raining base
(in cloud and in the clear air above a thin cloud) is marched like cloud, so a thin cloud margin over rain is never
stepped over from above.

## History
2026-11-06, issue 20261105_180442 ("Cloud base edge contour"): every raining cloud field seen from above showed a thin
see-through line just inside the edge of each clear gap. A ray from above met the pure rain of the floor above a thin
margin first, strode four steps and skipped the cloud. Found by knockouts (only removing the rain stride fixed it); the
fixed frame is much closer to a 15 m-step reference (mean |diff| 12.9 -> 5.4 levels), the motion score improves
(walk 4.43 -> 3.97, pitch 1.81 -> 1.50), cost within noise, 128 registers.
