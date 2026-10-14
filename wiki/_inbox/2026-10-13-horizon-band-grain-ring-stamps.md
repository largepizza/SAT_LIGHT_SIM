---
pages: [rendering/sky.md, rendering/clouds/temporal.md, rendering/clouds/lighting.md]
code: [shaders/sat_sky.frag, shaders/cloud_v2_resolve.comp, shaders/include/cloud_lightning.glsl, shaders/cloud_v2_lightning.comp, src/simulations/SatelliteSimCloudsV2.cpp]
---
## Now
The sky pass's single-scattering loop finds the stretch of each view ray inside the Earth's shadow cylinder once per
pixel, and each step takes the sunlit share of its length (the shadowed share is lit by the twilight sky). The cloud
resolve, in motion, widens its colour clamp to the 5x5 new samples within +-25% of the pixel's distance (opacity keeps
the 3x3 clip), fading out over 1-3 px of parallax. The Sun-shadow shaft rings store each value with a stamp of where
the slot sat relative to the eye; a lookup ignores slots that changed hands as the eye moved and the next ring or the
cascade stands in; rings fade in over 16 frames, the cascade from frame 4.

2026-10-14 (6384515): the sky's other Earth-shadow integrals (zenith sky light, terrain sky light, flat-layer twilight,
the sea's sky reflection) take the same exact sunlit share. The cloud light march takes 7 EQUAL steps over its length,
each with its own jitter (it was a 1.6 geometric series). The resolve takes its history unclamped when the true parallax
is under ~0.1 px a frame, and its eye-parallax distance cap is max(50 km, 2 x the eye's height). The shadow rings'
stamps record which of 8 wraps a slot holds, keep a per-slot running mean, and bake the window's leading edge first while
moving; the far-shadow cascade stays on while the march draws clouds (no longer cut at 60 km).

## History
2026-10-13 (ea6ffa4). The midpoint shadow test drew a hard band above the sunset horizon (~4.3 deg at a -0.84 deg
Sun) and veiled the clouds in it. The 3x3 motion clamp collapsed on the light march's all-or-nothing samples (blocky
grain while moving; 13.1% -> 0.4% of pixels past 16 levels 1 s after stopping). Far shadows needed ~130 frames to fill
and broke for ~100 frames after a 6-km hop (15% -> 1% of pixels past 16 levels).
2026-10-14: from 20-160 km with time running, the clouds' drift had made the resolve treat the view as moving, and
sub-pixel puffs flickered (per-frame pixels past 8 levels 0.10% -> 0.0002% at 158 km, 1.14% -> 0.00002% at 20.8 km).
The geometric light march made clouds in another cloud's shadow 30-60 levels too bright under a low Sun (Jensen bias of
single far samples); equal steps leave +8-10 at the same cost.
