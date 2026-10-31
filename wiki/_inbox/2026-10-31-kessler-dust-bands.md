---
pages: [simulation/kessler.md, simulation/index.md]
code: [src/simulations/SatelliteSimKessler.cpp, src/simulations/SatelliteSim.h]
---
## Now
A breakup's dust no longer stays a round cloud: from half an hour to four hours old each event is drawn as a
BAND along its parent's orbit (`kesslerFillDust`'s ring pass; issue_20261030_175418, "Dust Clouds from kessler").
The band is a chain of Gaussian clumps (a clump's along-track sigma 400 km, spacing 1.5 sigma, at most 26 per
band) spanning the fragments' along-track 3 sigma - the Clohessy-Wiltshire term `sv |4 sin(nt)/n - 3t|`, so
2 900 km after 30 min and a full ring past ~2 h - each with the fragments' radial / cross-track spread (20 km
floor, 250 km cap: a band, not a fog). The event's own light is spread over the clumps (the tube's `Amp`) x
`kRingGain` 1.0 in the point sources' units, i.e. the same "Dust cloud brightness" gain as the young cloud: at a
physical surface brightness the resolved band would be some six orders of magnitude below the night sky.
At the reporter's view (day 60.94, 170 km, gain x1000) the widest drawn band covers 135 deg of its orbit (37.5 %
of an annulus), five bands 544 deg in all; before the fix no band was drawn at all, and the dust was 8 410 pixels
over 2 display levels against 63 428 now (the faint band 0.29 % -> 3.13 % of the frame). One event seen from
30 000 km grows 14.9 deg (31 min) -> 74.3 deg (2 h). The bands take at most 144 of the 384-Gaussian dust budget
(the tube keeps the rest), so the cloud march's cost is unchanged (+0.3 ms at the report's view); the CPU kessler
block grows ~2.9 ms at day 60. The debris points are untouched (bright specks -1.5 %, no new white). Harness
`kessler state` -> `dust.rings` {pieces, arc_deg_max, arc_deg_sum, width_km_max, bands} lists each drawn band.

## History
The tube along the orbit (2026-10-20) drew each breakup as a chain of Gaussians centred on the parent's position
at its age, ranked by surface brightness; past ~20 min the pieces are hundreds of km long and a millionth of a
fresh blob's brightness, so in a late cascade only clouds seconds to minutes old were ever drawn and the debris
read as glints and round blobs. The bands were added on 2026-10-31 (qa/w6-kessler-dust).
