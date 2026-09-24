# Known photometric residuals

The inaccuracies we know about and have accepted, as of commit `e61f67f` (2026-09-23, end of
lighting-overhaul Phase 3). Every item passes or is outside the accuracy gate
(`cmake --build <dir> --target accuracy-gate`, see `cmake/AccuracyGate.cmake`). They are logged so
nobody re-discovers them as bugs, and so a future change that moves one can be judged against where
it started. This is a satellite-lighting simulator, not a model optimizer: each item below is small
next to the features built on top of it.

Numbers come from the gate's configuration (5000 samples, seed 1) unless noted. They are identical
on MSVC and GCC to the printed precision. The full history is on the "Satellite Brightness
Benchmarking" design page (Results log).

## Against published measurements

| Item | Model | Published | Gap | Why / notes |
|---|---|---|---|---|
| VisorSat mean m1000 (Mallama 2021) | 6.967 | 7.218 | −0.250 (tol 0.3) | Plain mean samples our random geometry; the observers' geometry selection is unpublished. Phase-matched mean 7.220 (+0.002) is the like-for-like comparison. |
| VisorSat median m1000 | 7.069 | 7.365 | −0.296 (tol 0.3) | **Narrowest gated margin (0.004).** Same cause as the mean. Accepted 2026-09-23: not worth re-gating. |
| VisorSat 110–120° phase bin | 7.059 | 7.433 (25 obs) | −0.374 | Model too bright at high phase. The only scored bin outside 0.3; curve RMS 0.134 passes. Bins 120°+ have ≤ 5 observations (unscored). |
| VisorSat 50–60° / 70–100° bins | — | — | −0.13 / +0.11..0.12 | Mild S-shaped residual around the observed curve. |
| VisorSat scatter (sd m1000) | 0.723 | 0.854 | −0.13 | Attitude is fixed; real satellites jitter and fly varying roll. No attitude-noise model yet. |
| VisorSat phase slope | 0.0036 mag/deg | 0.0049 | −0.0013 | Informational. |
| VisorSat − V1.0 differential | +1.042 | +1.29 | −0.248 (tol 0.3) | The visor's real shape is unpublished (derived from Cole's 23° full-shade constraint). |
| V1.0 mean m1000 (Mallama 2020a) | 5.926 | 5.93 | −0.004 | Held out of all fitting. sd 0.769 vs 0.67 published (informational). |
| V1.0 app bulk export (32.7°S, sim 2036-11-21) | 5.934 | 5.93 | +0.004 | Out-of-sample site and season; not a paper's sampling. |
| V2 Mini app bulk export | 5.81 (median 5.31, sd 1.57) | 7.87 mitigated; ~5.2 unmitigated (Mallama et al. 2023) | — | **Uncalibrated model**, flown without SpaceX's brightness-mitigation attitude. Next benchmark candidate. |

## Fitted, estimated or stood-in values

- **Calibrated (fitted to VisorSat, M8):** `solar_cell` diffuse albedo 0.06 → 0.02; the VisorSat
  visor size (inset 0.55 m, gap 0.233 m) at Cole's 23° cutoff. V1.0 stayed held out.
- **Estimates (model `sources` blocks, status `estimate`):** antenna-region size and albedo (0.8) on
  both 2020 Starlink models; V2 Mini wing hinge placement and tracking; most material presets
  outside `solar_cell`.
- **Hubble** flies an anti-sun stand-in attitude (no inertial-pointing law) and has no benchmark.
- **GGX, not Beckmann,** for the polished bus (Beckmann made 70–110° 0.3–0.5 mag too faint once
  earthshine was fixed), even though that material's α came from a Beckmann↔Phong equivalence.

## Model approximations (measured against exact references)

| Item | Error | Where checked |
|---|---|---|
| Occlusion sampling (16 samples/lobe) vs 25-point ray-cast reference | VisorSat p95 0.16–0.19, max 0.3; V2 Mini p95 0.06–0.07, max 0.25–0.36; others ≤ 0.02 | `--selftest` (gate p95 < 0.25; exact value varies per platform with the selftest's configurations) |
| GPU occlusion: lobes under 1e-4 of the total left unoccluded; skipped below mag 10 | ≤ 0.005 mag | design choice (`kOccMinLobeFrac`, `kOcclusionMagFloor`) |
| Cone occluder uses its larger radius | conservative (over-shadows) | `buildSatOcclusion` |
| Earthshine table vs exact integral | p95 0.004, max 0.014 mag | `--selftest` |
| Earth is a uniform Lambertian sphere, albedo 0.3 | no clouds, oceans, glint or seasonal albedo | model limit |
| Extinction: Chapman column (erfcx fit) vs brute-force ray integral | max 0.32% of the column | `--selftest` |
| Extinction constants: 8 km / 1.2 km scale heights, 60/40 split | fixed, no weather or site haze | model limit |
| Sun disk folded into every lobe as α = 0.0023 | flat-mirror peak +2% (1.50e4 vs 1.47e4 m²) | M0 |
| Merged lobes beyond the budget (48, or 256 for rosters ≤ 10k) | exact for the shipped models within budget; Hubble at a forced 24: p95 0.20 | lobe validator |

## Simulation frame and precision

- **Sim Earth rotation omits GMST at J2000** (≈ 280.46°): the Sun's hour angle is off real UTC by
  ≈ +5.25 h. Self-consistent everywhere; replaying real timestamped observations must correct it.
- **GPU orbit math is float32:** the GPU–CPU parity gap exceeds 0.02 mag (and motion is choppy)
  only with the observer very close to a satellite. Phase 4 follow mode needs the double-precision,
  camera-relative path.
- **Benchmarks exclude penumbra** (fully sunlit only, as the papers do); the app draws penumbra.
- **Legacy two-surface types are not physical magnitudes** (tuned display units); they cannot be
  traced, exported or benchmarked until they get geometry models.
