---
pages: [development/kessler.md]
code: [src/simulations/SatelliteSimKessler.cpp, shaders/cloud_march.comp]
---
## Now

A resolved dust piece keeps its surface brightness. `kesslerFillDust`'s tube drew a piece's light in the
point-source units (`toSky`, a display gain of ~10 magnitudes over a surface's physical brightness, 7e4 at the
report's view) while it was a few pixels across and, since d62306d (2026-10-30), at a constant `pi` once it
resolved - and `cloud_march.comp` divides that light by the Gaussian's volume, so a resolved piece's display
peak fell by `toSky/pi` (2.3e4): a station breakup's cloud at 1626-1731 km was 1.4e7 summed light at 20 min
(about 1e-5 of white, under 3 display levels in the reporter's crop) against 3.2e11 while unresolved, and the
same cloud was visible again at 40 min once it had receded enough to be a point.

The piece's light now stays in the point units while it is a few pixels across and becomes its surface
brightness once resolved, which the Gaussian's own `1/((2 pi)^1.5 sR sS sW)` already does, so a piece dims as
it spreads. Only the LEVEL is capped: at the total that renders a dust cloud's brightest possible face, a
semi-infinite layer of its own albedo (`cap = 0.25`, a quarter of a sunlit white face, the same 0.25 albedo as
the brightness estimate). The cap is computed in display units from the shader's own quantities (`q = dir' M
dir`, the along-ray integral `sqrt(pi/2q)`, the phase, `skyExposure()`), fades in with the resolved blend
`resK = smoothstep(1.5, 8, smallest sigma in half-res px)`, and never lifts the light above the point total, so
a piece under the crossover is unchanged. d62306d's intent (a resolved cloud must not pass white) is kept; its
error (capping the light at a constant instead of the display) is what made clouds vanish.

Measured at issue_20261019_231126's view (perf-frames fd5a036 + this change): `kessler state` dust.light_sum is
flat within +-7% through the crossover (3.23e11 at 0.4 min -> 2.78e11 at 20 min -> 2.79e11 at 30 min) where it
plunged 3.2e11 -> 1.4e7 before; the reporter's crop goes from 0.0000 mean / 0 px over 0.5 to 0.0136 / 970 px
over 0.5 at the report's age and 0.0926 / 16 467 px at 18 min (the reporter's own pre-928b44f crop: 0.3258,
29.26%); young clouds (0.5-12 min) are identical to the digit. c16's band clumps (`cwD = light * toSky`, no cap)
and the collision path are untouched.

## Still off

The fresh cloud right after it clears the Earth's limb (16-17 min at that view) is a bright compact blob at
`kessler.dust_gain` 1000: the dust adds 1.06% of the frame's pixels over 0.9. It is the sum of ~15 overlapping
pieces, so the per-piece cap does not bound it (lowering the cap 4x moved it 1.18% -> 1.06%). A breakup seen
from inside the cloud (790 m, issue_20261029_213225 shot 2) was not re-measured.
