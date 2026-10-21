---
pages: [development/harness.md, development/profiling.md, simulation/orbits.md, "new: simulation/launches.md"]
code: [tools/harness/mission_ops.py, tools/harness/launch_bench.py, src/simulations/SatelliteSimLaunches.cpp (launchTraceJson, launchFormationJson, buildBatch, memberState), src/simulations/Launches.cpp (boosterTrack, upperTrack, rampPhase), data/launches.json]
---
## Now
`tools/harness/mission_ops.py run [--captures]` traces the next launch of every (vehicle, program) pair in
data/launches.json at three epochs (2036-11-22, 2031-06-01, 2026-11-05) through its phases (pad, climb, max-Q,
staging, booster recovery, fairing, insertion, coast, deployment, raising, arrival, retirees, upper-stage deorbit /
entry / ship catch, docking) using harness `launch trace`, judges each phase against one threshold table (PASS / FAIL /
WARN for the landing-burn physics / SKIP for docking, not modelled), checks the Starmind rings of 8 with `launch
formation`, and writes report.json / .md / .html with per-pair contact sheets. `launch_bench.py run|compare` times
fixed launch scenes (the Twilight Starship bookmark at 1x / 1 h/s / 1 d/s, a busy 2032 day, a night launch close up,
a droneship landing), each with launches on and off. A formation's members (Starmind: one ship = one ring) now share
their cluster leader's raise schedule, so after the two-day release offset fades the eight fly as a ring all the way up
(spread <= ~3 km, ring shape within metres) instead of spreading up to 10,000 km and meeting only at the arrival.
Dispenser launches release a satellite every 12 s (stacks: Starlink Gen1 / Qianfan 15 s) with a 0.04 m/s spread.

## History
Bugs the suite found (2026-10-20): the booster track lacked its MECO sample (everything after staging ran 0.5 s early:
an ~800 m jump at separation); the upper stage's ground-arc rate was its inertial speed (5-13% too fast, a 250-940 m/s
drop at SECO); fairings separated at MECO + 30 s (~60-100 km) because the search ran before the deorbit plan set
tStageGone; trains' climbs ended at Kepler's rate not the slot's (J2) mean motion; the phasing residual was a constant
drift that stopped dead at arrival (18 m/s for direct-to-cell); the booster milestones used the vehicle's droneship
track and label for RTLS / ground landings; trains left 2 s apart overlapped (~1 m) for minutes.
