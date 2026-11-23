---
pages: [rendering/clouds/march.md, rendering/clouds/storms.md]
code: [shaders/cloud_v2_lightning.comp, shaders/cloud_v2_march.comp]
---
## Now
The lightning pass (cloud_v2_lightning.comp, workgroup 0) scans the storm-bin table once a frame into a bit mask of the
6 x 64 x 64 storm regions (each listed bin marks its region and the regions one bin away; face-border regions are always
walked) and skips a region with no listed storm before drawing its candidate flashes. A candidate needs a listed tower
to flash, so the flash list is exactly the same. From 3780 km the pass costs 4.9 ms instead of 8.5 (RTX 3070 Ti,
1920x1009); the rest there is the live candidates' column probes.

The cloud march's batch puts moonlit samples (Sun 0.06 below the sample's horizon) in the lit front with the non-faint
ones: under the Moon every sample runs its light march, and at the back they ran out of step with their neighbours.
Same image (A-vs-A floor), 128 registers; 49.6 -> 47.1 ms at a moonlit night-side view from 3780 km.

## History
2026-11-22 (c19 orbitperf, issues 20261122_190752, _213304, _213341): the 3780 km night view was 49.7 ms (lightning pass
8.5, march 22 with the 3x3 orbit block); 43.6 after both. 540 km day view 36.5 -> 34.4; 1517 km 37.7 -> 37.2 (noise).
