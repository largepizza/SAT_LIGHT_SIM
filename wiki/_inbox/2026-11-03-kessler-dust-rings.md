---
pages: [simulation/kessler.md]
code: [src/simulations/SatelliteSimKessler.cpp, src/simulations/SatelliteSim.h, src/simulations/SatelliteSimUI.cpp]
---
## Now
A breakup's dust goes through three looks: the young cloud (a tube along the orbit, minutes to 3 h), a BAND along
the parent's orbit (0.5 h until the event's hour is 11-14 h old, a full ring once it has wrapped), and from then a
dust RING around the Earth for up to 30 days (`kesslerDustRings`, issue_20261030_175418). Rings are merged by orbit
plane (bins of 25 km in a, 1 deg in inclination, 4 deg in the node), their light is the breakups' dust x
exp(-age / 3 days) (knee'd at 2e4 per ring), and they widen across the track with age as the fragments' J2 node
rates differ (a 51.6-deg ISS ring: 95 -> 151 -> 302 km sigma at 1 / 3 / 7 days, capped at 1 500 km). In a late
cascade the rings of a shell overlap: from 170 km the dust is a haze over half the sky, from 12 000 km a halo around
the Earth. A lone satellite's ring is drawn but too faint to see at the default "Dust cloud brightness" (x100).
Hidden settings (no UI): `kessler.dust_ring_gain` (default 1, the rings' brightness), `kessler.dust_rings` (default
true; false = bands only, faded out by 4 h: the A/B). Cost: GPU +0.0-0.7 ms at the worst cascade view, +0.1 ms from
12 000 km, ~0 for one ring; CPU +0.3-0.4 ms a frame; ~0.3 MB of plane bins at day 61; the dust buffer grows from 384
to 640 Gaussians (+20 KB). Harness `kessler state` -> `dust.shells`.

## History
The bands of 2026-10-31 faded out by 4 h, so no dust was ever drawn past 4 h and a ring never persisted; a wrapped
band's 26 clumps were 1 660 km apart with 400-km sigmas (beads, not a ring). The "prior branch" the reporter
remembers is the pre-tube dust (b9e2b69 .. 928b44f, 2026-10-16..19): each cloud was up to 12 straight Gaussians whose
along-track sigma grew as sv x age x 3 (16 000 km at 3 h for a 500 m/s kick), so within its 3-h life a cloud spanned
more than an orbit: a polygonal ring around the Earth, which the tube (2026-10-20) replaced with a physically sized
cloud.
