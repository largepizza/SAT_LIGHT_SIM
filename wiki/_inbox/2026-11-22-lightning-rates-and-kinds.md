---
pages: [rendering/clouds/weather.md, development/harness.md]
code: [shaders/cloud_v2_lightning.comp, src/simulations/SatelliteSimWeather.cpp, src/simulations/SatelliteSimWeatherWindow.cpp, src/simulations/SatelliteSimLightning.cpp]
---
## Now
A storm flashes as often as a real storm of its kind. "Lightning rate: Cb towers" (`clouds_v2.lightning_rate`,
default 3.0, slider 0-30) is the flashes a minute of an ordinary cell's full-strength dominant tower; a dominant
tower flashes at that x smoothstep(0.35, 0.75, strength) x its storm's kind (air-mass 1, multicell 2, supercell 12,
squall line / MCS 3 per tower, a tropical cyclone's towers 0.35), up to 240 a minute. "Lightning rate: storms"
(`clouds_v2.lightning_storm_rate_per_area`, default 1.0, slider 0-10) is the flashes a minute per 20 x 20 km of a
storm's deep cloud away from its dominant towers (flanking towers, thick anvil near the core, a line's cells), kept
with the kind's share (air-mass 0.15, multicell 0.3, supercell / line systems 1). "Cloud-to-ground share"
(`clouds_v2.lightning_cg_share`, default 0.30, slider 0-1, Settings > Weather > Lightning) is the share of an
ordinary storm's flashes that reach the ground; a supercell's is 0.4 of it (12%). Of the rest 4 in 7 are spider
lightning under the base and 3 in 7 stay in the cloud.

Measured at five storms (2036-07-10 05:30 UTC), flashes a minute, before -> now: air-mass cell 1.0 -> 3.3,
multicell 1.8 -> 13.1, supercell 1.2 -> 37.6, squall line 3.8 -> 40.8, MCS 2.6 -> 22.9; to ground 0.2-1.5 -> 0.9 /
4.5 / 5.5 / 14.9 / 7.1 a minute (27-37% of an ordinary storm's, 15% of the supercell's). The whole catalog (~180
storms alive) makes 18.6 flashes a second (2.0 before) against the world's 44-46: it holds a tenth of the world's
storms. Seen ground strokes are still rare: from 20 km outside a supercell 1 of 12 strokes in 2.5 minutes is drawn
over 200 px (rain in front of the rest); from under a base in the rain, none.

The storm's kind reaches the lightning pass in the low three bits of each tower's reach (whole metres, a multiple of
8 + the kind). The Weather window's "flashes a minute" is the same model evaluated on the CPU (towers exactly, the
regions from a measured 1.9 cells of deep cloud per tower); harness `weather state` gives each row's two parts and
`world_flashes`, the whole catalog's total.

Harness: `lightning stats on|off|reset`, `lightning stats [name]` count every flash once while sim time runs (by
kind, by storm, the budgets' high-water marks; rows to `captures/<name>.boltstats.json`), and `lightning goto id=|t=
[at=]` sets sim time to one counted flash, so a held frame can be captured with and without the channel
(`clouds_v2.lightning_bolt 0`): the difference is what is drawn of the stroke through rain and cloud.

Sources: Williams et al. 1999 (ordinary storms peak at 1-10 flashes a minute, severe ones over 60); Williams 1985
and Price and Rind 1992 (flash rate ~ the fifth power of the cloud top's height); Christian et al. 2003 and Cecil et
al. 2014 (44-46 flashes a second worldwide); Boccippio et al. 2001 (IC : CG 2.64-2.94 over the United States, up to
8-9 over the Great Plains, no trend with latitude); Rakov and Uman 2003 (3-5 strokes a ground flash).

## History
Until 2026-11-22 every dominant tower flashed at one rate (1.1 a minute) whatever its storm: a supercell made one
flash a minute. The storm regions' rate (0.53) did nothing at all: its candidates were weighted by the weather
map's cloud type and rain, which the storm objects (2026-10-23) cap at congestus and clear inside a storm, so the
weight was ~0.03 and no region flashed (measured: 0 flashes at ten times the default rate at four storms). The
cloud-to-ground share was a fixed 0.3 x smoothstep(0.3, 0.6, strength). The user ran the sliders at 12.6 and 1.27
and still found ground strokes rare.
