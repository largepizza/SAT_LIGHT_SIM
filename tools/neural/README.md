# tools/neural — the neural cloud-lighting sandbox (branch `neural-light`, 2026-11-11)

The question: can an image-space network infer the clouds' lighting from cheap primary-ray features, so the march
runs ONE light march per pixel instead of one per lit sample? The shape is Leonard & Westermann, "Real-Time
Volumetric Light Transport Inference without a Path" (arXiv 2611.12103): a U-Net over per-pixel features (optical
depth, interaction depth, crossing layers, the light transmittance at the interaction point, an environment estimate,
medium) with one decoder per illumination component. Ours is trained on OUR renderer's converged output (the user's
choice: the current look, not a path-traced truth), at the march's grid, with the key light and the sky ambient as
the two components; bounce, night glow, beams and lightning stay marched.

Why it might pay: on 2026-11-09's knockouts (1920x1009, shots 1 / 3 / 5) the cloud bucket was 60 / 47 / 28 ms, the
lighting loop 34 / 23 / 15 of it and the light march itself 30 / 21.5 / 12, with 2.25 / 1.9 / 0.76 lit samples a ray
each marching 6-11 field reads. The net's cost is a few ms at the half-res grid (see the bench) and does not grow with
density.

## Pieces

| Piece | What |
|---|---|
| `cloud_v2_march.comp` `#ifdef CV2_NDUMP` | the march variant that records 56 floats a grid pixel (targets summed over visits, features of the last visit) into march set binding 29; `shader_variant.py march --tag ndump -D CV2_NDUMP` |
| harness `ndump on\|off\|write <name> [frames=32]` | the host-visible record (`cv2NDump*`, SatelliteSim.h / SatelliteSimCloudsV2.cpp / SatelliteSimHarness.cpp): `on` allocates and forces the full-rate march; `write` zeroes, accumulates N frames, drains, writes `captures/<name>.f32 + .json` |
| `ndump.py info\|pack\|sheet` | read a dump; `pack` averages the targets, converts distances to km, and writes a fp16 `.npz` |
| `collect.py plan\|run` | the dataset: the user's cloud-ish issue-report shots + the perf snapshots (`profile_log.jsonl`), each perturbed in time of day / camera / altitude; one live app at 1280x720 (half-res grid 640x360), half-res clouds, user settings; resumable; packs and deletes the raw |
| `train.py stats\|train\|eval\|bench` | the net (a residual U-Net, 6 outputs = key rgb + ambient rgb in a scaled log space), validation held out by SOURCE, a sheet of target / prediction / difference, inference timing |

Setup: `powershell -ExecutionPolicy Bypass -File tools\neural\setup.ps1` (a py3.10 venv with CUDA PyTorch in
`tools/neural/.venv`; `data/` and `runs/` are gitignored).

## Record layout (channel: meaning; `ndump.py info` prints ranges)

0-2 key, 3-5 ambient, 6-8 rest (bounce + night), 9-11 the march's colour (fog and shafts included; zeroed in the
pack), 12 T, 13 mean depth, 14 visits (all SUMS over the visits until packed); 15 the kept samples' optical depth,
16 first contact, 17-20 where that optical depth crosses 0.25 / 1 / 4 / 10, 21 last contact, 22 where T crosses 0.5,
23 the cloud weight; at the INTERACTION sample (T crosses 0.5, else the heaviest): 24 the light march's optical depth,
25 the far side's, 26 the probe's (0 under a large pixel), 27 the Sun volume's transmittance, 28 height, 29 sigma,
30 thin, 31 rain, 32 ice, 33 hf, 34 cos(view, key), 35 hV, 36 top height, 37 footprint, 38-40 key colour, 41-43 sky
colour, 44 t, 45 Moon-is-key, 46 dayness, 47 T within the cloud. Distances are metres in the .f32 and km in the .npz.

## Gates

1. The net reaches the A-vs-A floor (~1.7-2 display levels mean |diff| on cloud pixels) on key + ambient from these
   features, per view class (ground cumulus, inside a layer, storms, orbit, sunset, Moon). If not, stop here.
2. Net + one light march per pixel beats the lighting loop it replaces by a margin on the storm / cumulus views,
   measured on the RTX 3070 Ti (`train.py bench` is the eager-PyTorch upper bound; the in-engine pass would be a
   Vulkan compute net with fp16 cooperative matrices, weights in a storage buffer).

## Status

See the bottom of this file as the night progresses (appended by the session).

### 2026-11-12 00:05 — phase 0 done, collection at 600+ views

- Dump path verified: binding 29, device-local host-visible record; 82.5 ms/frame at full rate with the variant at
  1280x720 half-res (grid 640x360). Two bugs on the way: the march descriptor pool had exactly 9 SSBOs (needs 10),
  and binding 28 was already the ice-optics table (the GPU fault).
- Dataset: 1800 planned views (214 cloud-ish report shots + 479 perf snapshots, perturbed in time / camera /
  altitude), ~7 views a minute, ~6 MB a view packed. `train.py stats` on the first 600: key mean 0.21, ambient
  0.04 (radiance); 36 views carry a few hundred pixels of extreme key (hundreds) — forward-scatter peaks with the Sun
  in or near the frame, kept (the log-space target compresses them).
- `train.py eval --filter "<expr over s>" --per-view` evaluates one view class (orbit / sunset / Moon / ...).

### 2026-11-12 01:50 — first training read, record widened to 56 floats, recollecting

- Preliminary trainings on ~690 views (3000 steps, width 32): a softplus output head died in the warmup (fixed:
  linear head, decode clamps), the footprint feature overflowed fp16 past 65 km (clamped), then `pre3` reached 8.2
  display levels mean |diff| over all validation views — but that average is carried by night views (0.1-0.6 levels,
  trivially dark); daytime views sit at 11-14 levels (worst 35), far from the ~2-level floor.
- The worst misses are regimes where the key light is a multi-scatter effect the net has to re-derive from raw optical
  depths (a low stratus deck at Sun 8°: key 0.67 behind a light optical depth of 15), and thin ice plumes where the
  single-visit features are speckled.
- Decision: dump the march's OWN per-sample key and ambient terms (and ambVis / ambLow) at the interaction sample as
  features 48-55 (what one light march per pixel buys in-engine); the net then learns the one-sample-to-integral
  correction. Record k = 56; the 48-float views in `data/views/` stay as they are, the new collection goes to
  `data/views2/`.

### 2026-11-12 01:50 — baselines on the way to the new-feature training

| predictor | day (levels) | twilight | night |
|---|---|---|---|
| old 48-float features, width 32, 12k steps on 852 views (`runs/base48`), single frame | 11.0-13.4 by altitude band | 5.0-8.0 | 0.1-0.6 |
| no net: w_acc x the march's single-sample key + ambient term, single frame | 20.5 | 10.9 | 0.14 |
| the same averaged over 8 single-frame dumps | 15.1 | 5.6 | 0.07 |

The old features plateau at ~12 levels by day however long the net trains (7.7 -> 7.0 overall from step 1000 to 12000
with night views pulling the mean down). ~5 levels of the single-frame numbers are the jitter noise of the
interaction sample's own terms, which the sky TAA averages: the frame-averaged reading (`eval --frames-dir`) is the
fair gate-1 number. `collect.py --frames-each 8` records those for the validation sources.

### 2026-11-12 04:30 — run A2 (56-float features, 620 training views, 20k steps)

Training notes: fp16 autocast diverged at step 8350 (a loss spike, then non-finite weights); the trainer now runs
bfloat16, restores the last good weights after 3 non-finite losses in a row, and `--resume`s weights. A2 = run A's
step-8000 weights + 12k steps. The key light's elevation joined the inputs (a constant plane; 47 inputs).

| class (validation, held out by source) | n | single frame (levels) | 8-frame mean | worst |
|---|---|---|---|---|
| day / ground | 19 | 4.84 | 4.42 | 10.2 |
| day / aircraft | 29 | 6.70 | 6.23 | 23.9 |
| day / orbit | 22 | 7.95 | 6.73 | 11.5 |
| twilight (all) | 23 | 2.8-4.2 | 2.5-4.1 | 16.0 |
| night (all) | 29 | 0.04-0.45 | | 4.8 |

Day overall 6.59 single / 5.90 frame-averaged (pre4 on 234 views, 3k steps: 10.6 / 10.0; the old features: 11-13).
`runs/fullA2/sheet_mix.png`: the structure and tone are right in every class; what remains is per-pixel grain from the
single-visit features and a global dimming on one outlier (a view inside a saturated cloud mass at 24 km). Run B =
the same weights continued on every view collected (~1500) with the GPU free, for the data-scaling read.

### 2026-11-12 06:30 — run B (1126 training views), the gate verdicts

Run B = A2's weights + 12k steps on 1126 training views (1665 collected, 161 held out by source; the RAM budget
dropped the rest). Both nets evaluated on the SAME 161 held-out views (class cut-offs: Sun > 0° day, > -18°
twilight; eye < 3 km ground, < 100 km aircraft):

| class (n) | A2 single / 8-frame mean | B single / 8-frame mean | worst (B) |
|---|---|---|---|
| day / ground (36) | 4.42 / 4.02 | 4.10 / 3.67 | 9.7 |
| day / aircraft (40) | 6.98 / 6.52 | 6.34 / 5.82 | 19.1 (s0680_2: inside a saturated cloud mass at 24 km) |
| day / orbit (26) | 7.53 / 6.40 | 7.09 / 5.99 | 10.9 |
| twilight (30) | 1.97 / 1.89 | 1.80 / 1.71 | 12.6 |
| night (29) | 0.25 / 0.20 | 0.23 / 0.19 | 4.5 |
| day overall (102) | 6.22 / 5.61 | 5.74 / 5.10 | |

Data scaling 620 -> 1126 views buys ~0.5 levels by day. At that rate the ~2-level floor is several doublings
of data away (the whole night's collection was 1665 views at 10.5 s each), so more data alone does not get there.

**Gate 1: not met by day** (ground 3.7, aircraft 5.8, orbit 6.0 frame-averaged levels against a floor of ~2);
met at twilight (1.7) and at night. The structure and tone are right (see `runs/fullB/sheet_*.png`); what
remains is a per-pixel error the single-visit features do not carry (the march's own interaction-sample terms
jitter from frame to frame, and the net sees one visit) plus occasional global dimming inside dense cloud.

**Gate 2: not met.** `train.py bench` (RTX 3070 Ti, fp16 eager PyTorch, channels_last, the GPU free):

| grid | width 16 (2.1 M) | width 24 (4.7 M) | width 32 (8.3 M, the trained net) |
|---|---|---|---|
| 640x360 (1280x720 half-res) | 5.2 ms | 8.5 ms | 11.3 ms |
| 960x540 (the user's 1920x1009 half-res, ~960x505) | 11.2 ms | | 24.3 ms |

The loop the net would replace costs 34 / 23 / 15 ms on the three reference views, of which the light march
itself is 30 / 21.5 / 12 ms at 2.25 / 1.9 / 0.76 lit samples a ray: ONE light march per pixel would still cost
roughly 13 / 11 / 16 ms (the per-sample march cost, once per pixel; an estimate, not measured). A Vulkan
fp16 cooperative-matrix net might run 3-5x faster than eager PyTorch (also unmeasured), i.e. ~5-8 ms for width
32 at the user's grid: net + one march ~18-24 ms against 34 / 23 / 15. A small win on the heaviest storm view
only, a loss wherever lit samples a ray are near 1 — and the net's cost is fixed per frame while the loop's
falls with the cloud cover. Width 16 would halve the net but has not been trained (and the trained net is
already short of gate 1).

Verdict: do not build the in-engine path on this formulation. What stays useful: the `-DCV2_NDUMP` dump path
and harness `ndump`, the 1665-view / 56-float dataset (~10 GB, `data/views2/`), the trainer. What could change
the answer: features that carry the multi-visit statistics the TAA would otherwise average (e.g. the march's
terms at TWO interaction samples, or the previous frame's net output as an input, the paper's temporal
recurrence); a target restricted to the ambient / multi-scatter part (keep the key light's single march,
infer only what the loop adds), which is the part the loop spends most of its reads on; and a width-16 net
measured in a real Vulkan compute pass rather than estimated.

Sheets (`runs/fullB/sheet_median.png`: the median view of each day class and twilight; `sheet_worst.png`: the
worst): on the medians the structure and tone match, the difference image is grain. The worst views are
large-scale misses: on s0569 (twilight, 12.6 levels) the key light's self-shadow boundary across a big soft
cloud face sits in the wrong place; on s0680_2 (24 km inside dense cloud, 19 levels) the whole mass comes out
warmer and brighter. Both are multi-scatter / shadowing structure that no single-sample per-pixel feature
carries; that, not the net's capacity or the data count, is the ceiling of this formulation.
