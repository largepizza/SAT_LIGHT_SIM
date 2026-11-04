---
pages: [rendering/clouds/weather.md, rendering/clouds/field.md]
code: [shaders/include/clouds_v2.glsl, shaders/cloud_v2_lightning.comp, shaders/rain_particles.vert]
---
## Now
Rain shafts in the low cloud field fall only where the cloud body is drawn over the column: the weather map's rain is
scaled by smoothstep(0, 0.08, mBase), mBase the body's margin at its base before the lobes (cv2FieldLow's rain branch;
the convective profile's base requirement req0 against the cell-capped strength, a deck's raw strength). Sub-pixel
footprints (300-2000 m) keep the mean. The tower rain cores are unchanged. Drops at the eye, the radar and the
curtains all read this one field (the eye rain map is the field at the eye's height), so they agree.

## History
Until 2026-11-04 the map's rain keyed on the 2D cloud strength alone and hung in the gaps between a storm's cells:
a grey curtain with a flat top under open sky, with drops at the eye in it (issue 20261104_020642).
