---
pages: [development/harness.md]
code: [src/simulations/SatelliteSimWeather.cpp, src/simulations/SatelliteSimAmbience.cpp]
---
## Now

`storms tornado draw=0` hides only the drawn funnel, its wall cloud and its debris; the per-frame state fill
runs in both draw states. `weather.tornado_list` (the bare `storms` command, and each capture's
`*.state.json`) therefore reports the same `present`, `base_agl_m`, `base_probed`, `tau_above` and `light_k`
with `draw=0` as with `draw=1` at the same sim time and view: at the test tornado (2036-11-22T02:32:40.183Z,
observer 7.4046 N 87.4705 E) both read present ~1.000, base_agl_m 1084, base_probed true, tau_above 594.2,
light_k 0.0917. The tornado roar in `SatelliteSimAmbience.cpp` reads `baseAglM` and `present` from the same
list, so it too now behaves identically whether the funnels are drawn or hidden. The state gives an A/B the
tornado that is there, not zeros; the draw flag changes pixels only (the funnel box 520,300,460,480 at
issue_20261023_223907's frame is 42.9 levels darker with the funnel drawn, 0.01 levels different either way
before/after the change).

## History

Before 2026-10-31 `weatherFillTornadoes` returned before the loop that writes those fields, so an A/B that
dumped the state with `draw=0` saw present 0.0, base_agl_m 0, base_probed false, tau_above 0.0 and
light_k 1.0 for a tornado that was still probed and present - it nearly produced a false "tornado missing"
verdict in the 2026-10-30b QA wave (evidence `qa/waves/20261030b/evidence/c19/notes.md` and `.../c20/`).
The fix moves the early-out to a `continue` just before the first `emit()`, after the state fill; the probe
request, the `tau >= 3` gate and the draw geometry are unchanged.
