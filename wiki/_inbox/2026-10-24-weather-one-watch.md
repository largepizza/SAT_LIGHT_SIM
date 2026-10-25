---
pages: [using/weather.md]
code: [src/simulations/SatelliteSimWeatherFeatured.cpp, src/simulations/SatelliteSimWeatherWindow.cpp, src/simulations/SatelliteSimWeatherCyclones.cpp]
---
## Now
Every entry in the Weather window (each EVENTS card, each NEAR YOU row, the radar's selected storm, each cyclone row,
season row and milestone) has one button, Watch. It sets the clock so the event is already happening (forward to it if it
is still to come, never to before it starts), runs time at 1x and puts it in frame; the message says what you see.
Tornado / waterspout: the funnel on the ground (now, or 30% into its life), from ~3 km on the ground, 50-degree view,
then moved out of the rain by one radar probe. A storm: its tornado if one is down now, else the storm at its height,
from the ground ~30-35 km upwind, then the radar probe. Hurricane / cyclone row: from orbit now, moved forward to its next
daylight if it is night over the storm; a storm not alive now at its peak. Haboob: 6 km in front of the wall. Wildfire:
4 km beside the burning front, 150 m up. A landfall milestone: at the landfall, from just off the coast.

## History
The cards had up to four buttons (Go to, Watch, Mature, Aloft, Eye, Orbit, Landfall, Next major, Tornado, Passage) and
rows had Go to / Watch / Replay / Tornado / Passage; "Watch" often meant "a minute before touchdown" or "20 min before
the line arrives". Playtesters could not tell them apart and landed where nothing was happening yet (user, 2026-10-24).
The other framings remain in the harness (`weather event <kind> act=1..3`, `weather go`, `weather tornado|passage`).
