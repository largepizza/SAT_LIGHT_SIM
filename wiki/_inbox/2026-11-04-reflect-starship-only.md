---
pages: [simulation/launches.md, simulation/reflectors.md, simulation/rockets.md]
code: [data/launches.json, tools/make_reflector_model.py, tools/make_rocket_models.py, data/satellite_models/reflect_orbital.json]
---
## Now
Reflect Orbital launches on Starship only, 25 mirrors a ship, released one at a time through the payload door
(`stack`: layout `pez`, pitch 0.87 m, `deploy_interval_s` 150, `sep_speed_mps` 1.0). The mirror is the 2036-era 55 m quad sail
(2,819 m2 of film, 38.7 m half-booms; `make_reflector_model.py` now defaults to `--side 55`; the 18 m published
diagram is the prototype and is not modelled). Stowed, the booms telescope to 6% of their length (~2.3 m nested
sections) and the film bundles to the same scale, so a stowed mirror is a 5.45 x 5.45 x 0.81 m cross: one a layer
across Starship's 8.2 m bay, int(22.04 m / 0.87 m) = 25 a ship. Mass is not the limit: ~300 kg a mirror (film
~10 g/m2 ~28 kg, booms ~0.4 kg/m ~62 kg, deployer ~30 kg, bus ~180 kg), 7.5 t a ship against ~100 t. Released
150 s apart at 1.0 m/s, every pair stays >= 150 m apart over the 6-h free flight, so the 78 m sails never unfold
near each other; the train leaves in 60 min (a longer span hits the drifting CW loop's self-crossing at ~0.72
orbit). Steady state: 5,000 mirrors / 25 / 5-year life = 40 launches a year; `batch_gap_days` 27 spreads each ring's
refresh over the whole life instead of one burst.

## History
2026-11-04: was 40 mirrors (170 kg) on a Falcon 9 with no stack (they appeared stowed at the dispenser point);
the user: "the Falcon 9 launching is a bit goofy and there is a clown car of satellites from one launch". The
model's film stow scale was 0.08 (a 6.1 m bundle, wider than the telescoped booms and too wide for the bay at 55 m).
`make_rocket_models.py`'s stowed measurement now reads polygon components and radial stows. The 18 m note
2026-11-04-reflector-quad-sail.md predates the user's 55 m decision (commit 5bbfa39).
