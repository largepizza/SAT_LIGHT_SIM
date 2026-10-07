---
pages: [using/features.md, using/getting-started.md, development/harness.md, rendering/clouds/weather.md, sound/music.md]
code: [src/simulations/SatelliteSimBookmarks.cpp, src/simulations/SatelliteSimHarness.cpp, data/bookmarks/bookmarks.json, tools/harness/scripts/default_bookmarks.satcmd, src/simulations/SatelliteSim.h, src/AudioSystem.h]
---
## Now
**Nine bookmarks ship with the sim** (`data/bookmarks/`, copied next to the exe as `default_bookmarks/` and packaged):
Venezuela Rainbow (2036-11-24 10:44 UTC, 9.97 N 68.27 W, sunrise behind a storm), Great Australian Eclipse
(2037-07-13 02:42:57 UTC at greatest eclipse, 21.75 S 138.31 E: the corona at a 15-degree field), Blood Moon over Alice
Springs (2037-01-31 14:01 UTC), Aurora from Orbit (a Kp 6.8 storm, 2037-01-12 18:00 UTC, 420 km over Siberia), Reflect
Orbital over Anchorage (2036-11-22 02:40 UTC: the line of mirrors above the city), AI Satellites at Twilight (the
intro's Big Sur vantage), Los Angeles by Night (10 km), Starmind Formation Rings (beside an 8-satellite ring, 565 km)
and Sunset from Orbit. A first run (no `bookmarks.json` yet) starts with them; the Bookmarks window's **Restore
defaults** re-adds any that are missing without touching the user's own. A bookmark may carry an optional `look`
(`time_scale_idx`, `paused`) that sets the clock on Go: the event bookmarks start at 1x, the rainbow and the Starmind
ring start paused. Rebuilt by `tools/harness/scripts/default_bookmarks.satcmd`; harness `bookmark add id= scale=
paused=`, `bookmark restore`, and the new `drift [default] [phase=] [rate=]` command.

**v1.2.0 defaults are the author's tuned settings** (to slider precision): master / music / ambience volume 0.4 / 0.7
/ 0.7; rain 0.71, drops at the eye 2.63, drop reach 33 m, rain fall speed 3.0, rain wind 0.4, storm wind 11 m/s, gusts
1.38; lightning storms 0.31 /min per 20 km, Cb towers 0.4 /min, bolts 1.25, tendrils 1.23, sprites 0.01; brightness
1.01, highlight flare 0.01; mirror max slew rate 0.001 deg/s; HQ photo 1x window; automatic render scale on.

## History
The rainbow was the user's snapshot taken at rain 3.0; at the shipped rain amount (0.71) the bow barely shows (it
needs ~1.5). The storm there also changes within ~10 minutes of sim time, so the bookmark is paused.
