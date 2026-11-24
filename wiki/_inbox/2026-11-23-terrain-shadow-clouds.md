---
pages: [rendering/clouds/march.md, rendering/terrain.md]
code: [shaders/cloud_v2_march.comp, shaders/include/cloud_lightning.glsl]
---
## Now
The terrain shadows the volumetric clouds' sunlight: the Sun's key light at a cloud sample is multiplied by the terrain
Sun-horizon map (cv2TerrSunT, the same map the air, fog and god rays read) when the Sun is below 20 degrees at the
sample. On only while the map is (Sun up at the eye, below 60 km, terrain in reach). Moonlight is not shadowed by
terrain.

## History
Added 2026-11-23 (lighting audit step 5). Before, cloud behind a ridge at sunrise caught sunlight the mountains
blocked (bright glow and rays toward the Sun in the Himalaya). Ungated it cost +0.6 ms at noon over the Alps; the
gate leaves only low-Sun views paying (+0.6-0.9 ms GPU, RTX 3070 Ti, 1600x900).
