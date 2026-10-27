---
pages: [rendering/clouds/march.md, rendering/launch-visuals.md, rendering/clouds/weather.md, "new: simulation/wildfires.md"]
code: [shaders/cloud_march.comp, src/simulations/SatelliteSimHaboob.cpp, src/simulations/SatelliteSimWeatherFeatured.cpp, src/simulations/Wildfire.cpp, src/simulations/SatelliteSimHarness.cpp]
---
## Now
- **Plume / smoke multiple scattering is the stack's.** In the composite (cloud_march.comp dustCloudsCS) the Sun's and the
  sky's multiple scattering of plumes and smoke is taken of the optical depth stacked along the ray: each Gaussian gets its
  share of x ms(x) at the running depth (ms(x) = 0.075 (1 - e^-0.6x) + 0.17 (1 - e^-0.25x^2)), so the sum is tauP ms(tauP).
  Engine lights keep a per-Gaussian term. Composite at 128 registers.
- **Wildfire events need a burning front.** wx::fireBurningShare = the share of the front's heat (48 points) on ground the
  biome map lets burn (smoothstep 0.08-0.3 of burnableAt), the same weighting the smoke sources use. The Weather window's
  Wildfire card ranks by size x (0.3 + activity) x (1 + pyro) x share and skips fires under 0.4; harness `fires go` skips
  them too; `fires` reports `front_burnable_share`.
- **Wildfire Watch** stands 5 km beside the smoke ~10 min old (wx::fireSmokeAt 600 s), 1.5 km upwind, 150 m up, looking at
  it a few degrees up (half the column's angle, 3-12 deg); it was 4 km beside the fire's head.
- **Haboob Watch** and the Weather window's haboob search use one test: haboobStrongestPoint on the actual front
  (haboobFrontOf, with the lingering outflow's lagged towers from the catalog in use: main or scout), at the drift phase
  of the time asked (not now). The search's dust prefilter uses the front's own lead (8 + 22 km x the age ramp). The Watch
  steps forward 2 min at a time (up to an hour) when the regenerating towers are weak at the event's moment.

## History
- The unified composite (2026-10-26) made multiple scattering per Gaussian; with the trail pieces (hundreds of thin
  Gaussians) a pall lost ~2-3x of its light and, from the ground under cumulus, the smoke between the clouds vanished
  (A/B: the pre-unified composite drew it; per-segment accumulators instead of the running form went to 249 registers).
- The 'smoke only from burnable front' change left fires like Guerrero 17.9 N 99.2 W (2.5% of the front burnable, a
  32-km head run into towns) ranked first by `fires go` though they drew no smoke or flames.
- Haboob: the search accepted lines on the old 5 + 14 km lead's dust, the Watch built a front 8 + 22 km out, dusty along
  half, from the towers alone (no lagged sustain) and with the drift phase of NOW: marginal storms passed the search and
  the Watch said "raises no dust" (harness_runs/recap, 2026-10-27).
- Tornadoes in heavy rain (rain_amount 3) can be wholly hidden from the Watch (e.g. 23.3 S 33.3 E, 2036-11-23 14:05:
  rain rate ~1 on every side out to 9 km); the funnel draws (rain 0 / 1 shows it), unchanged since before 2026-10-26.
