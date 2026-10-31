---
pages: [rendering/clouds/march.md]
code: [shaders/cloud_v2_march.comp, tools/make_mie_lut.py]
---
## Now
"Mie optics" (`mie_optics`, 0.33) modulates the tuned cloud single-scattering phase with the table's structure row
(the phase divided by its own 8-deg-smoothed self): `p_tuned x (1 + mie_optics x (structure - 1))`. That multiplier
is an amplitude only where the tuned phase's level is the table's, which is so at the cloudbow and the corona. Toward
the anti-solar point, past 154-160 deg from the Sun (a smoothstep on -cos, 0.90..0.94), it instead ADDS the table's
own sharp deviation from its smoothed level, `mie_optics x (p_table - p_table / structure)`, at the table's
amplitude: there the table's 4 pi p is 0.72 at 180 deg (10 um droplets) against a tuned back lobe of 1.43, so the
multiplier used to make a broad, smooth disc about 3.5x the table's phase. The cloudbow, the corona and the side
scatter are untouched, and the phase row is fetched once for both "Mie phase" and "Mie optics".

## History
2026-10-31, issue_20261022_175746 ("Mie optics visible from space"): from 223 km a small, intense spot with a bright
core sat on the deck and read as painted. Its pixel measures 179.92 deg from the Sun -- the anti-solar glory, not a
near-zero phase angle. The spot's peak-to-neighbourhood luminance ratio was 1.111 in the reporter's shot and 1.102 in
the old shader (reproduced in one app instance with `shaders reload march=`), 1.051 after; the anti-solar phase bump
against the deck 8-22 deg off it went 1.79x -> 1.19x, with the tuned phase alone at 1.115x. A control view at a 90 deg
phase angle is a no-op (imgtools diff mean 0.21 levels against the same-shader repeat's 0.16). The table is correct
and was not regenerated.
