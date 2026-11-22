---
pages: [rendering/hardware-tiers.md, using/graphics-settings.md, using/features.md, simulation/kessler.md]
code: [src/simulations/SatelliteSimSkyEvents.cpp, src/simulations/SatelliteSimSpaceWeather.cpp, src/simulations/SatelliteSimBookmarks.cpp, src/simulations/SatelliteSim.h]
---
## Now
The graphics preset changes what is drawn, never what is simulated. The Sun's Fury (the July 2037 solar storm and
the Kessler cascade it starts) runs the same on Potato, Planetarium and the full presets: the same shipped prebake,
the same collisions, debris and re-entries at the same seconds, and switching presets in the middle of it restarts
nothing. Its Watch is offered on every preset, in the Sky events window (the Sun's Fury card and the storm row of
the space-weather tab) and as the default bookmark "The Sun's Fury": on Planetarium and Potato the storm night shows
the dying satellites, the collisions' flashes and debris and the re-entries as points (the dust clouds too on
Planetarium), without the aurora. What stays off on those two presets, with the reason as the button's tooltip
("Off in this graphics mode: needs the aurora (Low or higher)"), is what would land on the aurora alone: the
ordinary geomagnetic storms' Watch, "Watch the aurora", and the Solar Storm bookmark.

After a jump past the storm date (a time set, a bookmark, the Betelgeuse Watch) sim time holds while the cascade's
history up to that date is replayed from the prebake, with the banner "Computing the debris cascade: day N of M".
That wait is the same on every preset: about 2.5 s to two months after the storm, 25 to 30 s more to the Betelgeuse
Watch at the turn of 2038 (RTX 3070 Ti box, 16 threads, a loaded machine).

## History
Until 2026-11-21 the Sun's Fury's Watch and bookmark were disabled on Planetarium and Potato because "the storm
night is the aurora" (the 2026-10-29 tier walk). The cascade itself was never tier-gated, but with its Watch greyed
out the storm read as switched off, and the replay's banner after a jump past it as the game computing a history
without it (issue_20261121_160405, shot 6). Measured on that day's build before the change: the jump from
2037-07-14 to 2037-09-10 gives the same `kessler fingerprint` on Planetarium and Medium (451,277 records, 139,280
events, cache source prebake), and Medium -> Planetarium -> Medium leaves it unchanged (Potato too, measured after
the change).
