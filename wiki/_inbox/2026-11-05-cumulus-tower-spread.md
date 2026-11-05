---
pages: [rendering/clouds/field.md]
code: [shaders/include/clouds_v2.glsl, src/simulations/SatelliteSimCloudsV2.cpp]
---
## Now
A cumulus' envelope is a 2-D strength against one vertical profile whose top T comes from the cell-scale strength.
"Cumulus tower spread" (`clouds_v2.cumulus_tower_spread`, default 0.75, range 0-0.9, Settings > Clouds) adds k x (1.8 s - 1)
to each cell's height factor, s a km-scale field (the cell volume's Perlin, 1-8 km octaves, plus the cells), and the top
may now reach 1.7 x "Tower top" of the span (was 1 x), capped at the type's span. Inside one cloud mass towers several km tall stand beside shoulders a few hundred metres
high, so a big cumulus reads as a skyline of separate heads rather than one dome. Bases and footprints are unchanged.
0 = one top per cloud. Not in SKY_LITE. Cost (RTX 3070 Ti, contended): cloud march +0.2 to +0.35 ms at Amazon and Darwin
views, ~0 on trade cumulus; march stays at 128 registers.

## History
Two rounds of shape work (turrets, thermal bubbles, bubble hierarchy, heavy-tailed cells) changed only the surface; the
silhouette was still one dome per mass ("gumdrops, still 2D", the user, 2026-11-05). Tried and dropped the same night: a
height-proportional lean of the cell lookup (sheared tall masses into smooth slanted planes), much stronger bubble relief
(smooth glassy ovals), a mid-height pinch for stacked masses (invisible, 250 registers).
