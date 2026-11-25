---
pages: [rendering/moon-and-eclipses.md]
code: [shaders/cloud_v2_march.comp, shaders/include/eclipse.glsl]
---
## Now
In a solar eclipse every sunlit term of the cloud march takes the Moon's shadow per sample (eclipseSunVis): the key
light, the zenith sky light, and the diffuse sunlight a cloud deck lets through to the clouds beneath it. At the 2037
totality under a rain overcast the deck goes dark blue-grey (~110 levels at the eclipse exposure), below the clear
totality sky beside it (~175), over dark ground.

## History
2026-11-25 (release sweep): the deck's diffuse sunlight (added 2026-10-27 for anvil shading) was not gated, so a rainy
eclipse cell was lit like daylight at totality (197 levels).
