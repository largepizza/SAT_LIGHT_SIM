---
pages: ["new: simulation/launches.md", modding/satellite-models.md, modding/index.md, development/harness.md, development/tools.md]
code: [src/simulations/SatelliteSimLaunches.cpp, src/simulations/Launches.cpp, data/launches.json, tools/make_rocket_models.py, src/simulations/SatModel.cpp, shaders/sat_mesh.vert, src/simulations/SatPhotometry.h]
---
## Now
Every vehicle with a payload fairing drops it once its upper stage passes `fairing_sep_km` (launches.json, 110 km;
crewed shrouds 85-95 km): two halves (`rocket_<vehicle>_fairing.json`, one half modelled at its place, the other
turned 180 deg) leave at 1.8 m/s, tip outward at 9-13 deg/s and fall under gravity and drag (ballistic coefficient
60 kg/m^2) until 60 km on the way down; they are selectable transients and a faint sunlit point. The stage then
shows `rocket_<vehicle>_upper_bare.json` (its payload adapter, whose top is `payload_base_m`; a crewed stage
carries its spacecraft until it leaves, then `_upper_empty`).

A program's `"stack"` (launches.json) says how its satellites ride: `pez` (Starship: out of a door on the leeward
side, 31.1 m up, one at a time, flat across the ship), `flat` (nadir face down, one or two a tier, released forward
top tier first: Starlink V1.5 on Falcon 9 two by sixteen, Qianfan two by nine) or `column` (around a dispenser
column, nadir face out, long axis along the rocket, released radially: Guowang two tiers of five). Satellites are
drawn stowed on the bare stage until each one's release, leave from their own slot in the stack's attitude with a
0.2-0.8 deg/s tip-off, and slew to their attitude law 1-2 min later (3 min slew), before unfolding.
`tools/make_rocket_models.py` measures each model stowed, derives the stack numbers (pitch, offset, member centre,
column radius), checks the stack fits every fairing the program flies in (and Starship's 8.2 x 22 m bay), writes the
dispensers (`rocket_dispenser_<name>.json`) and with `--check` fails when launches.json disagrees.

Model JSON gained `"fold"` on a component (point, axis, angle_deg, phase: turned about a hinge line after its stow
scaling; mesh only, photometry ignores it) and `"sweep_deg"` on a profile (a partial body of revolution). Starmind's
10 x 3 m bus folds in half (stowed 6.2 x 5.0 x 2.1 m): eight to a Starship. Payloads per launch: Starmind 8,
Starlink (Gen2 + Gen3 Broadband) 30, direct-to-cell 36, Starlink Gen1 on Falcon 9 31, Guowang 10 (5.2 m fairings
only: Long March 5B / 8A, Zhuque-3), Qianfan 18. Unfolding (stow + fold) added to Guowang, Qianfan, OneWeb, Amazon
Leo, Starlink V1.5, V3 (array packs folded over the zenith face) and DTC (the 5 m antenna folded under the bus).

## History
User reports 20261019_223922 ("can Starmind actually fit in the Starship?" - no: the 10 m bus alone needs a 10.4 m
circle against an 8 m bay; it carried 40) and 20261019_225243 (Guowang "birthed out of the launch vehicle", the
fairing never opened). Amazon Leo (a 2 x 2 x 1.2 m bus with a 3 m panel: 45 do not fit any fairing) and OneWeb (36
on a column need ~12 m of a 5 m fairing) have no stack yet: they still appear at the dispenser point, stowed.
Launch CPU cost at the default Starship bookmark: 1.0 ms -> 2.1 ms a frame (5300 live batches vs 1300: Starmind's
launch rate is 5x), after the batches were walked by pointer (2.4 ms before).
