---
pages: [rendering/aurora-airglow.md, rendering/atmosphere-and-sky.md, development/harness.md]
code: [shaders/cloud_march.comp, shaders/include/darksky.glsl, shaders/include/cloud_params.glsl, src/simulations/SpaceWeather.cpp, src/simulations/SatelliteSimHarness.cpp]
---
## Now
The aurora is gated against a physical sky: skyglow is light added to the natural sky, so the background is
pristine (22 mag/arcsec^2) x (1 + d / 0.0241), d = the light-pollution dome's linear city brightness (its
d / (d + 0.08) compression undone) x the usual elevation term. A downtown core reads ~18 (Bortle 8-9), rural
Arizona or Iowa ~21.3, Fairbanks ~20. The aurora's own brightness runs at the physical 2.5 mag per decade, so a
storm's bright arcs survive a suburban sky while faint diffuse parts fade. The Milky Way and zodiacal light keep
their artistic gate (`darkSkySkyMag`).

Above Kp ~5 (ramp Kp 5 -> 8.5) storms change look: a growing share of sheets are tall, red-topped and strongly
rayed (red pillars); bright sheets get a magenta lower border ~9 km lower; sunlit tops add N2+ blue; a patchy
630-nm red glow at 290-380 km reaches up to 7 deg equatorward of the green edge (a red northern sky from ~41 deg
magnetic latitude at Kp 9, a faint red skirt from orbit); rare giant rayed bands (a ~100-km stretch of a curtain whose rays
stand to 350-650 km) glow blue where sunlit and dim red otherwise; emission fades out below the march's top
(700 km in storms). Sheets come in groups of 5; ~70% of groups are arc systems bright over 1-2 long stretches of the oval
(bright parts ~6x the mean), and the diffuse volume follows them; a storm no longer fills the whole oval.

Settings (Atmosphere tab, Aurora): "Arc systems" `clouds.aurora_arc_systems` 1 (0 = all sheets alike), "Storm
features (red / magenta)" `clouds.aurora_storm_features` 1, "Storm red glow" `clouds.aurora_storm_red` 1, "Blue
rays (chance)" `clouds.aurora_blue_rays` 0.012. Cost within noise (Kp 9 at Coldfoot 13.79 vs 13.85 ms off).
Harness `state` has a `light_dome` block: the 16 sectors, `gained` and `dark_sky`.

## History
Issue 20261014_193231: a Kp 9 storm was invisible from an Iowa farm and from Arizona, and cut off along a hard
azimuth toward a town. The aurora read the Milky Way's dark-sky dome, a log ramp from 1e-6 toward an artistic
13 mag/arcsec^2: a raw glow of 0.01 was already 83% "city" (a 0-sector beside a 0.83 one was the hard edge) and
populated farmland read 1.0 in every sector. Its brightness slope was 0.85 mag/decade, so a whole display
vanished at once. From orbit the Kp 9 oval was ~50 even concentric rings (a "donut"). A first arc-system cut
(55% of groups, short stretches) left a Kp 9 sky over Coldfoot nearly empty; a first storm red at 7x today's
strength drew a solid red donut from orbit; the first giant rays were single ~10-km lines (magenta when
unlit) and read as isolated straight lines from orbit; tall red pillars ended on a hard ceiling at the 700-km
march top seen from 590 km.
