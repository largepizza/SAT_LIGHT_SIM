---
pages: [rendering/sky.md, simulation/space-weather.md]
code: [shaders/sat_sky.frag, src/simulations/SatelliteSimCorona.cpp, src/simulations/SpaceWeather.cpp, src/simulations/SatelliteSim.h]
---
## Now
The solar corona (at totality, and with the inertial lock's "Occult the Sun" as a coronagraph) is built each frame from
the Sun's state, a pure function of sim time. The solar cycle sets its shape: near minimum the outer corona lies in a
belt along a nearly flat current sheet (the flattened corona of a quiet Sun) and the polar coronal holes are dark and
full of fine plumes; near maximum the sheet is warped up to ~70 degrees and the corona is round, with streamers at every
latitude. The fibres follow dipole field lines about each limb's neutral line (where the warped sheet crosses that limb)
and are read at each line's own footpoint, so north and south differ. Helmet streamers (a broad closed base, a cusp, a
thin stalk bending toward the sheet) live a few rotations each, on the sheet near minimum and anywhere near maximum, and
over the largest sunspot groups near the limbs; each sits at its heliographic place, so it rises at the east limb and
returns on the west limb half a rotation later. The fine structure flows slowly outward and the prominences change
over days. CMEs: every Earth-directed CME of the space-weather model leaves the Sun as a growing halo a transit time
before its storm arrives (the Sun's Fury: at its flare, 17.5 h before); limb CMEs (a bright loop, a dark cavity, a
bright core) come at a rate following the cycle and the geomagnetic activity and cross the field in 1-6 hours. In the
coronagraph view the corona is lifted by r^1.2 (`photometry.corona_display_lift`), as coronagraph images are shown.
Planetarium (SKY_LITE) keeps the older, symmetric corona.

## History
2026-10-28 (issue 20261028_220320 "Sun corona work"). The corona's footpoints were mirrored about the solar equator
and its streamers were a fixed noise pattern: the same symmetric picture at every eclipse, whatever the cycle, and it
drew only near totality (not with the occulter).
