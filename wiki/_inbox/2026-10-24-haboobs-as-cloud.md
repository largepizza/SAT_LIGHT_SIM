---
pages: [simulation/weather.md, rendering/clouds.md]
code: [src/simulations/SatelliteSimHaboob.cpp, shaders/include/clouds_v2.glsl, shaders/include/cv2_storms.glsl, shaders/cloud_v2_march.comp, src/simulations/SatelliteSimWeatherFeatured.cpp]
---
## Now
A haboob's dust wall is drawn by the volumetric clouds: the CPU rasterises each storm's gust front into a 512 x 512 grid
round the observer (wall strength, top height, the clear air ahead; 400-m cells near the ground), and the clouds v2 field
adds a dust term shaped like cumulus (the low layer's 3D margin with the cumulus lobe noise, its nose ahead of its foot,
a row of heads of different heights along the front), lit by the march like any cloud and tinted beige (0.95, 0.70, 0.45;
the sky light twice). Its lobe noise moves with the wall and rises ~2 m/s. Ahead of the front the low cloud and rain clear
for ~12 km. The front leads the storm's motion over the ground (map motion + drift). Beyond the grid (from orbit) the old
plume Gaussians remain. Setting "Haboobs as cloud" (`clouds_v2.haboob_field`, default on; Weather window > Controls).
The Weather window's Haboob card (and harness `haboob [aloft=1]`) picks an inland storm whose wall is front-lit, clear of
its anvil, Sun 18 deg up or more (25-60 preferred), and stands 9 km ahead of the wall's dustiest stretch, on the ground
or 2.5 km up. Cost (Medium, 1600x900): the wall itself 0.1-0.4 ms of cloud march; clearing the air ahead reveals the
storm behind (+~4-6 ms in those views); CPU 1.3-1.9 ms per grid rebuild (every 2 s at 1x).

## History
The first cut (2026-10-23) drew walls as plume Gaussians: 4-20 km flat slabs with an Earth-fixed billow noise the wall
slid through at the storm's speed. Its Go to stood a fixed distance from the storm's centre (40-80 km from the drawn
wall), the front was placed ahead of the storm's map-frame motion (so it often moved away from the viewer), and the
weather map's rain and the line's shelf deck hid the wall from where it put you.
