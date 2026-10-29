# v1.2 tour: shot list (draft for approval, 2026-10-29)

The cinematic is `tour_v1_2.json` (copy it to `<user data>/cinematics/` to open it in the Cinematics window). It was
built in a live harness app by `tour_v1_2_build.py` (each shot placed by a Watch, a bookmark, an issue-report view or a
plain observer / time, then keyed), and checked with low-resolution previews only (`cine preview`, docs/HARNESS.md).
**No HQ render has been made.** Preview mp4s and a contact sheet: `screenshots/tour_v1_2/`.

Rules the cut follows: slow, readable camera moves (pans of 10-40 degrees over 12 s, gentle zooms); every shot at 1x
except two gentle time-lapses that never cross day / night (the typhoon from orbit at x60, the twilight launch at x15);
no lightning-heavy night storms, no strobing; each shot keeps its own look (settings + cloud drift are stored with it).

Total ~5 min (22 shots).

| # | Chapter | Shot | What it shows | Dur | Rate | Placement | Why |
|---|---|---|---|---|---|---|---|
| 1 | Opening | halo | Night side over Europe from 420 km: city lights glittering below, the sunlit Starmind shells arcing over the dark Earth | 12 s | 1x | observer 48N 12E 420 km, 2036-11-22 21:00 UTC | The 2036 sky in one image: a living night Earth and a million satellites |
| 2 | A living Earth | coast | Low over the Pacific off Big Sur at sunset: the physical Sun glitter path on the waves | 12 s | 1x | 36.05N 121.75W, 40 m, Sun 4 deg setting | The new sea |
| 3 | | alps | Morning over the Bernese Oberland from 5.7 km: eroded 3D terrain, a sea of cloud in the valleys | 12 s | 1x | 46.55N 7.98E, 1500 m AGL, Sun 7 deg rising | Terrain detail + clouds v2 |
| 4 | | city | Los Angeles at night from 2.5 km, drifting east: street grids, lamp posts, real roads | 12 s | 1x | 34.05N 118.45W, Sun -25 deg | Procedural cities |
| 5 | Weather | anvil | A cumulonimbus anvil lit gold at sunset from 6.6 km, mid-Pacific | 12 s | 1x | issue 20261023_003611 shot 1 (default settings) | Storm towers and anvils |
| 6 | | mammatus | Under a storm's golden base over Venezuelan farmland (the default "Rainbow" bookmark), tilting up | 12 s | 1x | bookmark 5 | Storm bases, sunset light, farmland |
| 7 | | haboob | A haboob's dust wall advancing over the Kalahari | 12 s | 1x | `weather event haboob` (the Weather window's Watch) | Weather events |
| 8 | | fireline | A wildfire's flame front at night, smoke over it (Queensland) | 10 s | 1x | issue 20261028_191813 shot 1 | Wildfires |
| 9 | | cyclone | Super Typhoon over Luzon from 885 km, slow push-in | 12 s | x60 | bookmark 10 | Tropical cyclones (arms turn; daytime only) |
| 10 | | eyewall | Inside a hurricane's eye at 7.5 km: the eyewall and its cirrus outflow | 12 s | 1x | issue 20261027_213254 shot 3 | The cyclone up close |
| 11 | Launches | liftoff | A Starship lifting off from a Gulf sea platform by day, 5 km away, the camera following it up | 24 s | 1x | `launch next when=day vehicle=starship pad_dist_m=5000`, keys from the rocket's az/el every 2.5 s | Launches |
| 12 | | jellyfish | The twilight launch's exhaust bells catching the Sun high over a dark Gulf (default bookmark) | 14 s | x15 | bookmark 8, from 150 s before it | The signature twilight plume |
| 13 | | droneship | A Falcon 9 booster's landing burn onto A Shortfall of Gravitas | 16 s | 1x | `launch droneship day=1 lead=14 dist_m=2500 h=15`, tracked | Recovery |
| 14 | | catch | A Starship ship's flip, landing burn and catch at a sea platform tower | 24 s | 1x | `missions catch`, tracked | Full reuse |
| 15 | Satellites | iss | Following the ISS over a sunlit Earth, the camera drifting round it | 14 s | 1x | follow shot (ISS), 2036-11-22 03:16 UTC | Satellites as 3D models |
| 16 | | rings | Inside the Starmind sun-synchronous shell: a formation ring's neighbours as meshes, the shell as a field of light | 14 s | 1x | follow shot (bookmark 6, sat 434774) | Starmind formations |
| 17 | The sky | aurora_orbit | A strong aurora from orbit (the "Solar Storm" bookmark), curtains with red tops | 12 s | 1x | bookmark 3 | Space-weather aurora |
| 18 | | comet | The Great Comet of 2037 at its closest approach (mag -6), tilting up its tail | 12 s | 1x | 2037-03-17 01:00 UTC, `comets watch great` | Comets |
| 19 | | eclipse | 13 Jul 2037 total solar eclipse over Australia: wide orange-horizon sky pushing in to the corona | 14 s | 1x | bookmark 1, 20 s before greatest, clouds off | Real eclipses |
| 20 | | supernova | Betelgeuse's 2037 supernova (mag -11) over the Great Plains | 12 s | 1x | `skyevents watch supernova` | Sky events |
| 21 | The Sun's Fury | fury | The Carrington-class storm night: red aurora overhead from the US Southwest | 12 s | 1x | bookmark 7 | The scripted storm |
| 22 | | dead | A dead Starmind satellite tumbling a day after the storm, the shells and Earth behind | 14 s | 1x | follow shot, 2037-07-16 06:00, `kessler select dead formation=1` | The Kessler cascade begins |

Not included, and why: tornadoes (every Watch / report view this session framed rain or a dark base: none read as a
funnel), meteors (sparse at physical rates), the Moon-base glints and Jupiter (sub-pixel at a tour's field of view),
the blood moon (overcast at the bookmark; a dim red dot when cleared), Starmind deployment (from the ground it is a dot
441 km away; a ride-along needs a cinematic follow of a rocket transient, which follow shots cannot do), re-entry
fireballs (a soft moving dot from the Watch's 120 km).

Open questions for the user are in the hand-off report.
