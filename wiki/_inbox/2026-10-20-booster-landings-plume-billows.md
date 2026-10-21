---
pages: [new: simulation/launches.md, rendering/sea.md]
code: [src/simulations/Launches.cpp, src/simulations/SatelliteSimLaunches.cpp, tools/make_rocket_models.py, shaders/cloud_march.comp, shaders/sat_sky.frag]
---
## Now
A returning booster falls with a ballistic coefficient of 2200 kg/m^2 (terminal ~190 m/s at sea level) and, after its
entry burn, steers its horizontal speed with aerodynamic lift (up to 0.6 x the drag) toward what reaches the landing
point in the time left. It comes over the ship / landing zone / tower nearly vertical: lean from the vertical at 1 km
at most 3.7 deg (ship), 1.6 (landing zone), 0.2 (tower); at touchdown <= 1 deg. The landing burn (constant deceleration,
from ~1.3-2.3 km) ends at 1.5 m/s straight down. Boosters with legs (Falcon 9, New Glenn, LM-12, Zhuque-3, Tianlong-3,
Pallas-1) fly with them folded and deploy them over 2.5 s from 7 s before touchdown. `launch check` reports
`booster_lean_deg_by_alt`.

The plumes' low-altitude billow field (below 3-12 km) is Earth-fixed: read at the point's ECEF offset from the terrain
detail's integer-anchored cell, 2048/3 m and 2048/9 m cells. The sea's screen-space reflection drops texels round an
engine light's place on screen when the reflected ray misses the light's real position.

## History
Issue 20261020_213739 "Rockets land tilted": drag was half the real value and there was no lift, so boosters reached
8 km crossing the ground at 650-1000 m/s and their landing burns leaned 35-50 deg (Falcon 9 to its ship: 37 deg from
8 km to touchdown). Issue 20261020_214200: the billow field was read in the eye's ENU axes, which turn with the eye's
position, so it slid with the camera (2 km/s at the reporter's speed). Issue 20261020_181339 shot 2: a flame in front
of horizon cloud sits in that cloud's (unflagged) texel, so the earlier emissive-texel test missed it and the flame was
mirrored as a glitter column from the horizon down.
