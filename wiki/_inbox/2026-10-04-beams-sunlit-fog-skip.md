---
pages: [simulation/reflectors.md, rendering/clouds/march.md, rendering/cities.md, rendering/hardware-tiers.md, rendering/clouds/field.md]
code: [shaders/sat_orbit.comp, shaders/cloud_v2_march.comp, shaders/sat_sky.frag]
---
## Now
A Reflect-Orbital mirror beams only while it is sunlit: the beam's intensity (1361 W/m^2 x mirror area x
F x cos incidence x beam gain) is multiplied by the satellite's Earth-shadow factor, the same soft
umbra/penumbra cone the satellite's own brightness uses. A mirror in the penumbra gives a dimmer beam; in the
umbra it gives none, so no ground spot, cloud light, shaft or beam sound comes from it.

The fog/dust/ice-fog march (20 steps after the clouds' march) is skipped for a ray whose eye is above the
fog band and whose cloud transmittance has fallen below 0.01: nothing beneath an opaque cloud shows from
above, and the dust above it is faint (x0.25 seen from above).

Rain-only samples (shafts below a base) each stand for four coarse steps. The procedural city lights are drawn
on every preset, Low and Planetarium included (they no longer depend on the terrain detail switch).
The city light sprites are off under Potato and sit at sea level when terrain is knocked out (Planetarium).

The low cloud's shape (lobe) volume is read in one of four fixed rotations of its tile, chosen per frame at the
observer so that no short lattice direction of the tile lies in the local horizontal plane (the original orientation
wherever it is clear enough).

## History
Until 2026-10-04 the beam ignored the Earth's shadow: mirrors on the night side of the terminator kept
beaming at full strength (the user's Antarctic winter snapshot, 2037-07-12, ~400 beams on a polar night).
The fog skip measured -2.8 ms of cloud march (38.6 -> 35.9 ms) in the user's orbit storm snapshot (388 km,
1920x1009, full-rate march, RTX 3070 Ti), image within run-to-run noise. The fog march had been documented
as costing ~0, which held only at the ground-level benchmark views.
Also 2026-10-04: the cirrus samples' bounce light reuses the flow the field evaluation computed (it recomputed
the eight-wave flow per sample): ~1-2 ms at 11 km inside the high layer, image identical. In that view (user
snapshot, Arizona, night, anvils on the horizon) the HIGH layer, not the anvils, is the cost: ~11 ms of 32 ms of
cloud march, from evaluating and shading cirrus along horizon rays that stay in its band for ~200 km
(its density does not change the cost). Flared Cb heads ~3 ms, tower columns ~2.4 ms.
Rain x4 steps: user's snowing-storm snapshot (Dushanbe, night, 2 km) cloud march 40.6 -> 27.2 ms; review 22's daytime
storm 8.3 -> 6.8 ms, a shaft edge ~3 levels brighter. City pattern on Low +0.7 ms, Planetarium +2.4 ms (LA, 1.5 km,
RTX 3070 Ti); before, those tiers showed the night map's 5-km blobs.
Shape-frame choice: the user's snapshot at 9.6 N 134.6 W showed rows of identical puffs converging to the horizon; the
unrotated tile's x axis lay in the horizontal plane there (|up . x| = 0.007 in the drifted frame). Per-sample warps
that would break the lattice cost +5-9 ms (NVIDIA register cliff), so the frame is chosen per frame on the CPU instead.
