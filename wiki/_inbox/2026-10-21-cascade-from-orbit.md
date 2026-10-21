---
pages: [rendering/clouds/march.md, rendering/terrain.md]
code: [shaders/cloud_v2_lightning.comp, shaders/include/cloud_lightning.glsl, src/simulations/SatelliteSimCloudsV2.cpp, shaders/sat_sky.frag]
---
## Now
The Sun shadow cascade works from orbit as near the ground. Its lattice axes across the Sun are written every frame by
the lightning pass. Its rays ride with the clouds across the Sun (a, b integrated) AND along it: the CPU integrates the
eye's motion along the Sun and passes, per ray half, how far the knots moved since that half was baked
(`cv2.sunCasc8.xy`); the lookup reads each ray at the knots it was baked for, and a bake taken while the eye moves replaces
the ray's running mean. The ground's light bounced from the albedo (`bounceK`) applies to all land, inside and past the
terrain march's range ("Terrain fade end").

## History
Issue 20261021_002345 ("Odd shadows from space", 333 km over Antarctica): a sharp-edged black trapezoid of ray-streaked
cloud under the eye. The axes were written only with the eye inside the cloud shell, so from orbit the lookup used the
axes of the last place the eye had been low (after a fresh load at altitude: zero, and the cascade silently did nothing).
With the axes fixed, fast flight from orbit drew a dark rectangle (the knots, fixed relative to the eye, were read several
9-km knots off). The faint circle in the same shot was the march range: only terrain hits took the bounce (~15% brighter
snow inside). Cost unchanged (128 registers; ground storm view 38.4-38.8 ms old and new).
