---
pages: [simulation/launches.md, simulation/kessler.md, modding/index.md, using/bookmarks.md, development/harness.md]
code: [src/simulations/SatelliteSimLaunches.cpp, src/simulations/Launches.cpp, data/launches.json, shaders/sat_orbit.comp, src/simulations/SatelliteSimKessler.cpp, src/simulations/SatelliteSimMissions.cpp, tools/make_pad_coasts.py]
---
## Now
The constellations are built up between the late 2010s and 2036 ("Deployment era (2019-2036)", `launches.deployment_era`,
default on; off = the 2036 roster at every date). Each program's `deployment` block in launches.json (start, complete,
`present_2026` at 2026-10-20, `shell_stagger`, `early_vehicles`/`early_until`) fills its planes by first-fill launches along
a logistic S-curve; a slot is not there before its first fill arrives, and later refreshes keep the design-life cadence
(first refresh at least `min_first_life` 0.25 x life after the fill). `roster_history` gives stations' launch dates, the
2020 benchmark shells (gone by 2024-2025) and the Space Junk count (30k in 2026 -> 300k by 2036-11). Sites and vehicles have
`online` dates (`online_for` per vehicle); 27 Starship sites were added (Starbase Pad 2, KSC LC-49, and 25 fictional:
Louisiana F-J, Gulf, Boca Chica, Pacific and Cape sea platforms), 42 in all. Satellites that are not there yet are not drawn,
cannot be selected or exploded, and are left out of the Kessler world. The Missions window's Ongoing tab opens with
BUILD-OUT (deployed / target, launches a day). Harness: `deploy`, `deploy stats`, `launch padstats`.

## History
Numbers 2026-10-20 .. 2036-11-22: ~197k launches (Starmind 188.5k: 123.6k first fills at 8 per Starship), peak 107 a day
(Starship 104) in late 2032. Same-pad pairs within 90 min per week at the 2032 peak: 398 -> 89 after rotating the
deconfliction's alternative pads (they all started at the list's head) and adding pads. Launch CPU at 2036 unchanged
(0.97 vs 1.04 ms era off). The two launch bookmarks were re-shot (adding sites re-hashes every launch's pad).
