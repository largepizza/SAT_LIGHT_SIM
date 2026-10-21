---
pages: [simulation/launches.md, simulation/kessler.md, development/profiling.md, using/missions.md]
code: [src/simulations/SatelliteSimLaunches.cpp, src/simulations/Launches.cpp, data/launches.json, tools/make_crew_models.py, tools/make_rocket_models.py, src/simulations/SatelliteSimKessler.cpp]
---
## Now
Dream Chaser (Sierra Space) flies cargo to the ISS once a year on Vulcan Centaur VC4L from SLC-41. Canadarm2 berths it
at Harmony nadir like Cygnus and HTV-X; its visits come half a year after HTV-X's (`alternate_with`), so the two never
share the port. Coming home, its Shooting Star cargo module is jettisoned 15 minutes before the entry and burns up a
little behind it; the spaceplane flies the entry to 10 km, glides to the KSC Shuttle Landing Facility (the deorbit is
timed so the entry ends short of the runway, heading for it), touches down on the centreline at ~95 m/s and rolls
2.4 km to a stop, then stays on the runway 40 minutes. Selectable and followable throughout; milestones: module
jettison, entry interface, glide, final approach, touchdown, wheels stop. Harness `launch crew craft=dream
event=jettison|glide|final|touchdown|stop`, `launch trace` -> `docking.runway`.

Kessler at fast warps: deaths of records already on the GPU are sent in arrival order, at most ~24k and ~2 ms a frame
(no sort of the backlog); the roster's dirty list sorts only what is new; the buffers a grown pool replaced are freed on
a thread. A breakup clicked while a rebuild of the world runs is queued and applied when it is in (no wait on the
main thread); the history is the same.

## History
Until 2026-10-21c a second Explode during a rebuild waited for it on the main thread (171 ms), and at 1 yr/s after the
CME the GPU upload of record deaths (up to 131k a frame, the whole backlog sorted each frame) and the freeing of the
old pool buffers were the remaining hitches (hitch_check D 1 yr p99 62 ms, max 91 ms).
