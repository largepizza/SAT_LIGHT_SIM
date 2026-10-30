---
pages: [simulation/rockets.md, simulation/launches.md, rendering/launch-visuals.md, development/harness.md]
code: [src/simulations/SatelliteSimLaunches.cpp, tools/harness/mission_ops.py]
---
## Now
At staging the upper stage starts where it sat on the stack: both stages' track points are their aft ends, and the
upper stage's aft end is placed along the stack's axis at the stack model's top less the upper model's top (read from
the rocket models when the launch state is made: Starship 72.32 m, on the hot-staging ring; Falcon 9 37.52 m). The
stages then part with a smooth push, 40 m/s reached over 5 s (peak 12 m/s^2); the whole offset fades out with the upper
stage's blend into its inertial plane and is zero at SECO. The booster's attitude after staging is a causal Hann window
16 s long over its raw pitch sampled on a fixed 0.5-s grid: the 180-degree flip after MECO is a slew peaking at
22.3 deg/s with an angular acceleration of at most ~5 deg/s^2 that starts and ends at zero, and the booster turns about
its centre of mass (0.3 of its length up), not its aft end (the shift fades out over 40 s after the flip).
`launch trace` meco_staging reports `stack_offset_m`, `stage_gap_m`, `sep_axis_clearance_min_m`, `sep_gap_5s_m`,
`booster_flip_rate_max_deg_s`, `booster_flip_accel_max_deg_s2` (frame-rate sampled); the mission-ops suite checks them.

## History
Issue 20261030_134600 (2026-10-30): the ship appeared inside the booster at separation (both drawn from the same aft
point), and the booster's flip was choppy: a 10-s box average over 21 samples taken at tau - 0.5 k moved one sample past
the raw pitch's 180-degree step every 0.5 s, so the flip came as 9-degree jumps twice a second. Measured after: clearance
between the stages' axes >= 5.3 m from 1 s after staging, gap at +5 s 170-270 m, mission ops 1286 PASS / 0 FAIL.
