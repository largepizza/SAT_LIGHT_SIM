# A haboob's dust behind the front settles: the pall is no longer a second wall (2026-10-31)

`src/simulations/SatelliteSimHaboob.cpp`, `SatelliteSim::weatherFillDustGrid` — the CPU rasterisation of the
storms' gust fronts into the dust grid that `clouds_v2.glsl`/`cv2_storms.glsl` march as beige cloud.

**What it did.** Per cell, behind the front's foot:

- strength `e = m.e0 * (0.55 + 0.45*hn) * core * pall * wAl` with `pall = (1 - 0.5*smoothstep(-3, -12 km)) * …`:
  ~0.9 of the wall's strength at 5 km behind the front, 0.5 only past 12 km;
- top `m.H0 * (0.65 + 0.55*hn) * (1 - 0.6*smoothstep(-2.5, -12 km))`: still ~0.9 of the wall's own top at 5 km
  behind the front.

So the dust behind the front was a second wall 26 km deep. A viewer inside the outflow saw a featureless tan sheet
up to 60 degrees elevation with no sky over it (issue_20261026_192211: "pretty huge and continuous, I'd expect
them to just be an arc front"). The Gaussian plume path this grid replaced drew the pall at sigma 1.5 against the
wall's 25 (~6%); that ratio had been lost.

**What it does now.** `pall = (1 - 0.9*smoothstep(-1.5, -6 km)) * …` and the top
`* (1 - 0.65*smoothstep(-1.5, -6 km) - 0.15*smoothstep(-6, -16 km))`: by 6 km behind the front the dust is ~0.1
of the wall's strength and ~0.35 of its top, gone 26 km back. The wall's front face and everything ahead of it are
untouched (same e, top, colour, gap/head noise).

**Measured** (report view, `report load ... shot=1`, 1920x1009, `time pause` + settle): spatial luminance sd
2.12 -> 16.94 at the reported camera (the reporter's own frame reads 15.80 on the same metric), RGB
196/175/150 -> 173/168/173. From the same place and time facing the front: sd 4.13 -> 17.91 (the wall's lumpy top
with sky over it). The dust grid at that view: 13067 -> 12529 cells (-4.1%, the trailing cells under the stamp
floor), `e_max` 0.9987 both. A spawned wall from the ground: sd 29.59 -> 29.66, mean diff 1.83 levels, nearest
wall and `e_max` unchanged. A haboob-free control with the metered exposure off: mean 0.223 levels, >8-level
share 0.000% (the build's own repeat on that view: 0.29).

**Still open.** The clear-air ceiling is the grid's *global* highest wall top (`cv2DustFrame[1].w`), and the
clear strip runs -8 .. +12 km across the whole front; both are still hard stamps, but they only act below
~3.1 km AGL and did not appear in either reported frame, so they are left alone. `issue_20261026_204749`'s
hyper-eroded high layer is the storm cloud field's own look (identical with the haboob switched off:
sd 40.19 vs 40.14, luminance corr 0.99966 above the bottom fifth) and needs a storm-field cluster.

Evidence: `qa/waves/20261030b/evidence/c25/` (README with the numbers, before/after frames and crops, the
haboobs-on/off A/B, the control and the run folders).
