---
pages: [new: rendering/meteors.md, using/sky-events.md, rendering/atmosphere-and-sky.md, development/harness.md]
code: [src/simulations/Meteors.h, src/simulations/Meteors.cpp, src/simulations/SatelliteSimMeteors.cpp, src/simulations/SatelliteSimSkyEvents.cpp (skySearchMeteorShowers), tools/make_icons.py (sky-meteors)]
---
## Now
Meteors fall all the time: the sporadic background (ZHR 10, radiants drawn per meteor from the apex, helion and
antihelion sources, so mornings are richer) and the major annual showers (Quadrantids, Lyrids, Eta Aquariids,
Southern Delta Aquariids, Perseids, Draconids, Orionids, Southern / Northern Taurids, Leonids, Geminids, Ursids) with
the IMO working list's peak solar longitude, ZHR, activity slopes (ZHR x 10^(-B |dLam|)), population index, entry
speed (20-71 km/s) and radiant with its daily drift. The flux a ZHR stands for is Koschack & Rendtel's
ZHR (13.1 r - 16.5)(r - 1.3)^0.748 / 37200 meteoroids per km^2 per hour (brighter than absolute +6.5), x sin of the
radiant's elevation; magnitudes follow r below mag 2 and a shallower bright-end index above (fireball-rich Taurids).
Every meteor is a pure function of sim time (hashed per shower, magnitude band, 1-degree Earth-fixed cell and time
bin), so warp, reverse and bookmarks agree. A meteor begins at 80 + 0.55 v km, ends at 65 + 0.35 v + 2.5 M km, flies
straight from the radiant for 0.2-1 s, brightens to a peak at 2/3 of its path, flares (M <= -1), fragments
(fireballs, M <= -4), ends in a terminal burst (bolides, M <= -8) and leaves a persistent train (M <= -5.5: green,
then dim orange, drifting and twisting in the upper-atmosphere wind for seconds to a minute). Fast bright meteors are
white-green, slow ones orange-yellow. Its brightness goes through the stars' dimming (night, light-pollution dome,
the Moon) and the point model's limit, so a city or a bright Moon removes the faint ones. Drawn as a point (bloom,
glare) and its streak over the eye's 0.1 s persistence (emissive Gaussians); not drawn above 2x time warp.
Settings `meteors.enabled` (on), `meteors.rate` (1), `meteors.fireball_rate` (1).
The Sky events window lists each shower's peak (ZHR, the Moon's lit fraction and a quality note); its Watch goes to
a dark, clear site near the peak with the radiant >= 25 deg up, at 1x, looking 45 deg from the radiant (FOV 75).
Harness `meteors [state]|count [minutes=] [lm=]|force [kind=meteor|fireball|bolide] ...|go [shower=]`.

## History
New on 2026-10-27 (branch feat/meteor-showers). Measured at the 2037 Perseid peak (RTX 3070 Ti, 1600x900): from a dark
site 1784 meteors / h brighter than mag 6.5 all sky (322 brighter than 4, ~56 / h round a 75-deg view), from Manhattan
190 / h; the fill costs ~0.07 ms a frame on the CPU, GPU within noise. A first cut divided the Koschack-Rendtel flux by
1000 (the IMO's per-1000-km^2 figure): 2 meteors an hour.
