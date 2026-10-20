---
pages: [simulation/launches.md, rendering/terrain.md, rendering/satellites.md, development/profiling.md]
code: [tools/make_pad_coasts.py, data/launch_coasts.json, shaders/include/terrain_detail.glsl, shaders/include/cloud_params.glsl, shaders/sat_sky.frag, shaders/sat_mesh.frag, src/simulations/SatelliteSimLaunches.cpp, src/simulations/SatelliteSim.cpp, src/simulations/SatelliteSim.h]
---
## Now
**Coasts around launch sites.** The coastal launch sites (Starbase, the Cape, Louisiana, Wallops, Kourou, Wenchang,
Sriharikota, Tanegashima, Mahia, SaxaVord, Vandenberg) stand on land with the real coastline and lagoons nearby.
`tools/make_pad_coasts.py` bakes the Natural Earth 10 m coastline around each group of coastal sites into a small
signed distance grid (48 x 48 cells over 12-20 km, 250-420 m a cell, against the global water map's ~5 km), with
every pad's graded disc forced to land, into `data/launch_coasts.json`. Inside a patch the terrain, the sea, the
beaches and the depth passes all take the shore from the grid; its land is never lower than 2.5 m (land at 0 m was
drawn like the sea), and where the 5-km day map shows water on it, it takes the patch's ground colour (sand flats at
Boca Chica, scrub at the Cape, marsh in Louisiana). The nearest four patches within 400 km are in the frame's uniform
block. Off with "Launch site graphics".

**Open lattices converge to their mean.** A truss whose members are smaller than a pixel (a catch tower from ~1 km,
grid fins) is drawn with a per-pixel threshold that changes every frame, so the sky's temporal anti-aliasing averages it
to its coverage: a fine translucent lattice instead of a fixed speckle that swam as the view moved. Without the TAA (or
in the 3D view's MSAA) the threshold stays fixed.

**Floodlit apron.** At night a pad's concrete is lit by its floods: brightest toward the mount and under the masts'
aim, falling off to the apron's corners; each 10-m slab has its own shade and a weathered mottle.

**Engine bells at night** keep their colour (orange at the lip to yellow-white at the throat) whatever the exposure;
they used to saturate to a white ring at night.

**Environment probes after a time jump.** A probe re-renders whole after a jump in sim time of more than
max(30 min, 10 wall seconds of the current warp); a pad's tanks lit by a daytime probe read pale blue-white at night.
Probes inside the atmosphere take the observer's light-pollution dome for the Milky Way and zodiacal light.

**Launch CPU.** At the Twilight Starship Launch bookmark (~5300 live batches) the `launches` CPU bucket fell from
~2.1 ms to ~1.25 ms a frame (1x), with identical launch results: the window walk is reused while sim time stays within
5 minutes of it, freeing and record writes skip batches whose state cannot change before a known time, the visuals
loop over the batches with something to draw, and the plume Gaussians are cheaper. Harness `launch state` ->
`cpu_split_ms` breaks the bucket down.

## Was
The pads sat on the 5-km water map and DEM: Starbase's foreground and the Cape's pads read as sea and the pads as
islands of apron; the tower lattice dithered from ~1 km; the floodlit apron was a uniform pale slab; launch CPU ~2.1 ms.
