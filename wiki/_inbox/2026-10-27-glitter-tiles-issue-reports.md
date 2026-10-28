---
pages: [rendering/sea.md, rendering/launch-visuals.md, development/issue-reports.md]
code: [shaders/sat_sky.frag, shaders/sat_mesh.frag, src/simulations/SatelliteSimReportsWindow.cpp, src/simulations/SatelliteSim.h]
---
## Now
The Sun's glitter on the sea is made of facets fixed on the water. A lattice on the wave field's own coordinates,
turned to the view's azimuth in 22.5-degree steps (two neighbouring orientations cross-faded), has cells of
0.25 m x 2^k about two pixel footprints along and across the view (two sizes blended). Each cell is one facet whose
unresolved slope (Gaussian, the glint lobe's own slope variance) turns continuously at its own rate. The cell flashes
while that slope mirrors the Sun to the eye within a radius chosen so the chance of a flash equals the share of the
surface the mirroring facets cover times the facets the cell stands for, and then carries the mean glint divided by
that chance. 0.4 of the light stays the smooth glitter path. The lattice is read at the unjittered ray's point on the
sea, so the sky TAA's jitter does not move a sparkle between cells. A sparkle stays on its patch of water as the camera
moves; from high up each cell holds many facets and the sparkle becomes the smooth mean.

Starship's pale replacement heat-shield tiles are a lighter grey tile (1.6x the tiles' level, grey 0.5), darker than
the felt in the gaps, so each keeps its outline.

The Issue reports window saves the text box being edited (title, description, a shot's note) when it leaves the
screen: Back, opening another issue, Export, Refresh, Go, Delete or closing the window write it first, and an export
zips the text just typed. Only thumbnails inside the window's scroll view hold one of the 96 atlas cells; one that
finds no free cell waits for one.

## History
2026-10-27 (issues 20261027_180609, _184725, _184917). The glitter sparkles were a hash of the screen pixel (2 x 2
blocks) and a 6-per-second time bin: they did not move with the water and showed at every altitude. The pale tiles
were 2.5x in the felt's grey, filling the hexagon in about the gaps' tone: they read as missing tiles. A committed text
box waited to be drawn again to apply its commit, so Back / Close / Export lost the text typed last (and exported zips
missed it); with more than 96 issues every atlas cell counted as drawn (every card is laid out, scrolled to or not),
so each further card, a new issue first, was marked "No image" until the window reloaded.
