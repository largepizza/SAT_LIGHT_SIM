---
pages: [using/weather.md, rendering/clouds/storms.md, development/harness.md]
code: [shaders/cloud_v2_lightning.comp, src/simulations/SatelliteSimLightning.cpp, src/simulations/SatelliteSimWeatherWindow.cpp, src/simulations/SatelliteSimHarness.cpp, src/simulations/SatelliteSim.h]
---
## Now
The Weather window's radar marks the lightning of the last 5 minutes of sim time: a yellow cross where a ground
stroke struck, a small blue dot where any other flash (in cloud, spider, sprite) started, fading with age (alpha 1 ->
0.25), and a line under the radar: "lightning, last 5 min: ground N / in cloud M". The "Strikes" button (on by default)
hides them. Works at 50, 150 and 400 km; not on the loop's past frames.

The marks come from a CPU ring (`strikeRing_`, 4096 marks, always kept, nothing on the GPU): each flash is marked once,
by id, when the lightning pass's list first shows it (when its tree is built), at its Earth-fixed position at its start
(a storm drifts away from its old strikes, as on a real strike map) and in the weather map's frame. The list only
holds what the lightning pass walks (out to the cloud tops' horizon, clouds on), so the map holds what is in reach.
At 10x time every flash is caught; at higher time scales the short ones can fall between frames (the flashes stay
deterministic in sim time; the list only samples them).

Harness: `lightning map [minutes=5] [range=<km>] [rows=] [open=1] [strikes=0|1]` gives counts by kind, by distance
from the nearest tower's axis, and by the radar's rain under each point (none / light / moderate / heavy), with the
share of ground strokes on light or no rain (`cg_dry_share`), and opens the radar for a capture.

Anvil ground strokes: a share of a dominant tower's ground strokes (air-mass 0.15, multicell 0.25, supercell 0.45,
squall line and MCS 0.4) start in the tower's anvil downshear (its shear bearing +-26 deg, 0.25-0.6 of its reach out,
at least 6 km) and strike round there: a supercell's forward flank, the trailing stratiform region of a line or an
MCS. Where no anvil cloud stands there, the stroke comes from the tower as before, so the rates, the share to ground
and every other flash are unchanged. In this model the forward flank under the anvil rains (the storm's stratiform
deck), so these strokes land in 30-45 dBZ rather than the core's 60-66, not in dry air.

No flash comes from outside a storm object: the cumulus field's deep rain bands (the Amazon at night, 30-40 dBZ out
to 400 km) flash 0 times.

## History
2026-11-22 (wave 20261122 c16, the user's request for better tools to check where lightning spawns): added.
