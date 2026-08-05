# CLOUD_PERF_PLAN.md

Performance plan for the volumetric cloud pipeline (`shaders/cloud_march.comp`, with
`shaders/beam_cloud_block.comp` as its verbatim-duplicated sibling).

Written 2026-08-04 after an audit prompted by comparing against
[EVE Raymarched clouds](https://github.com/LGhassen/EnvironmentalVisualEnhancements/wiki/Raymarched-cloud-configuration)
and the Nubis/Horizon-class renderers.

Read `CLAUDE.md`'s "Active Development: Earth / Terrain Rendering" and
"Subsystem: GPU Performance Profiling" first. Re-measure with `savePerfSnapshot()` +
`tools/perf_analysis/analyze_profile.py` between tiers — do not assume.

---

## Cost model (the thing the whole plan is aimed at)

Per **in-cloud** sample in the main view march (`cloudMarchCS`, cloud_march.comp:1043-1331):

| Work | Texture ops |
|---|---|
| coverage `textureLod(earthCloudsTex)` | 1 |
| `cloudWarpOffset` → `cloudWarpNoiseSample` | 8 |
| `colNoise` + `baseNoise` | 2 |
| `cloudDensity` (3 presence slices + 1 detail) | 4 |
| sun self-shadow cone × `N_CONE`=12 (coverage + colNoise + baseNoise + cloudDensity) | 84 |
| **total** | **~99** |

Per **empty** (non-cloud) sample: **11** — and empty samples dominate. Sizing it with the
current defaults (shell 5586→15000 m, `cloudMaxRenderDistM` 800 km, `cloudDistFadeEndM` ~399 km,
`kCloudMaxStepM` 250 m, `bigStep` = 2× = 500 m): a ground-level horizon ray enters the shell
~160 km out and exits ~375 km out, so ~200 km inside the shell ≈ **400 empty steps × 11 fetches
= 4400 texture ops on a ray that may never touch a cloud**.

Two conclusions drive the ordering below:

1. **Empty-step cost is the biggest single lever**, and it is almost entirely avoidable work.
2. **The sun cone is 85% of in-cloud cost**, so anything that touches it is worth 5× what the
   same change to the primary sample is worth.

---

## Tier 1 — output-identical, no re-tuning  ✅ DONE (2026-08-04)

Everything here is provably equivalent (or, where noted, strictly *more* consistent with
existing call sites). Buildable and A/B-able without changing a single slider.

- [x] **T1.1 — `localCov` early-out in the view march.** cloud_march.comp:1067 computed
      `localCov` and then unconditionally did the warp (8 fetches), `colNoise` and `baseNoise`
      before it could bail at `hFade < 0.001`. The sun cone, `cloudGroundShadow`,
      `cirrusMarchCS` and `beam_cloud_block.comp` **all already** apply
      `if (localCov < 0.02) continue;` — the view march was the one inconsistent site.
      Empty-step cost 11 fetches → 1.

      *Not bit-identical, and the plan should say so:* a sample with `localCov` just under 0.02
      could previously still produce density if BOTH `colNoise` and the presence-slice blend
      exceeded `1 - localCov` (≈0.981). That is rare and produces a sub-threshold wisp which —
      because the other four call sites already threshold at 0.02 — cast no self-shadow, no
      ground shadow, and blocked no beam. Adding the guard removes an inconsistency rather than
      creating one.

- [x] ~~**T1.2 — `cloudWarpNoiseSample`: 8 `texelFetch` → 1 `textureLod`.**~~ **REVERTED
      2026-08-04 — it brought the faceting straight back. Do not retry.** The full post-mortem now
      lives on the function itself in `include/common.glsl`; the short version:

      The algebra was right (hardware LINEAR blends with weight `frac(c*R - 0.5)`, so
      `c = (i0 + u + 0.5)/R` makes that weight exactly `u`) and was verified at the interior case
      and both wrap edges. What it missed is that **GPU texture filtering carries only ~8 bits of
      subtexel precision**, so the weight gets re-quantized to 1/256 *after* the smoothstep.

      The justification given at the time — that 1/256 is finer than the RGBA8 bake's own 1/255
      value quantization — is true and irrelevant. The two quantizations act completely
      differently: stored-value quantization is a static per-texel offset that interpolation then
      *smooths* into a continuous field, while weight quantization creates steps *within* each
      texel span, which is exactly the across-the-cell stair/facet structure the manual blend
      exists to remove — and it is amplified 32× (`kWarpStrength`) into a coordinate whose noise
      domain repeats every ~1 unit, so the error lands as cloud **geometry**, not brightness.

      Reported symptoms, both traced to this: "cloud tessellation is back", and "odd artifacts
      inside of clouds" — the latter because `warpUVW` feeds the density lookups *and* the
      per-column `colH`/`baseH` lookups, so a stepped warp distorts internal structure, not just
      silhouettes. Visible with the shadow cone disabled, and pronounced by it (the cone samples
      the same warped field via the parent's `warpUVW`).

      On revert the helper moved **into `common.glsl`** taking the sampler as a parameter (the
      pattern `terrain.glsl` already uses for `terrainHeightAtDir`), so the two copies in
      `cloud_march.comp` and `beam_cloud_block.comp` can no longer drift.

      *If this needs to get cheaper, the lever is the bake — resolution, format, octave count — or
      removing the need for a coordinate warp at all. Not the interpolation.*

<details><summary>Original (wrong) T1.2 rationale, kept for the record</summary>

      The manual
      smoothstep-weighted trilinear blend existed because hardware trilinear is *linear* and the
      analytic Perlin it replaced was smoothstep-interpolated — a mismatch that was visible as
      "tessellating triangle" faceting. Identical output is available from a single hardware
      fetch by putting the smoothstep into the **coordinate** instead of the weight:
      hardware lerps with `frac(c*R - 0.5)`, so feeding `c = (i0 + u + 0.5)/R` makes that weight
      exactly `u`. Sampler is already `LINEAR`+`REPEAT`, and REPEAT does the `i0 = -1` /
      `i0 = R-1` wrap that `wrapTexel3` was doing by hand.

      Only difference: the interpolation weight becomes 8-bit fixed point. That is a smaller
      quantization than the RGBA8 bake's own value quantization, so the faceting this code exists
      to prevent does not return. Applied in both cloud_march.comp and beam_cloud_block.comp.

</details>

**Revised per-sample texture ops** (T1.2's 8 fetches are back, so the earlier table was optimistic
by 7 on the primary sample only — the cone was never affected, it reuses the parent's `warpUVW`):

| | original | current |
|---|---|---|
| in-cloud primary sample | 15 | **12** |
| sun cone step | 7 | **4** |
| empty step | 11 | **1** |

The empty-step and cone-step wins are untouched, and those are the two that dominate.

- [x] **T1.3 — redundant `length`/`normalize` of the same vector.** `pECEF` is `p` through an
      orthonormal basis, so `|pECEF| == |p|`. Per sample the march paid `length(p)`,
      `normalize(pECEF)`, `length(pECEF)`, `pECEF/pLen`, and `normalize(pECEF)` again — 4
      sqrt/rsqrt where 1 suffices. `pLen = h + R_EARTH` is *bit-identical* to `length(p)` here
      (Sterbenz: `length(p)` ≈ 6.38e6 and `R_EARTH` = 6.371e6 are within 2×, so the subtraction
      is exact and adding it back recovers the original), and `dirECEF` serves every normalize
      site. Same pattern in the sun cone (`cpL` vs `ch`), so ×12.

- [x] **T1.4 — free guards.** `auroraOvalMaskLocal` (acos + cross products + atan, plus a
      `warpPerlin3` = 8 gradient hashes inside the oval) ran per in-cloud sample even when
      `cloud.auroraCloudGain == 0` multiplied the result to zero. Same for `beamCloudLighting`
      when `beamLightCount == 0`.

- [x] **T1.5 — `texture()` → `textureLod(..., 0.0)` in compute.** Implicit-LOD `texture()` is
      only defined in fragment shaders. These volumes are single-mip so it resolves to LOD 0 in
      practice, but the explicit form is spec-correct and cannot make a driver emit derivative
      machinery.

**Expected:** large cut on clear/horizon-heavy views (the empty-step path), moderate cut
everywhere else. **Measure this tier on its own before starting Tier 2** — it is the baseline
every later number is judged against.

### As landed (2026-08-04)

Files touched: `shaders/cloud_march.comp`, `shaders/beam_cloud_block.comp`. No C++ changes, no
new bindings, no struct growth, no settings changes. `sat_sky.frag` needed nothing — its copies of
`cloudWarpOffset`/`cloudDensity` were already deleted when the march moved to compute.

T1.3 was applied in all five marches, not just the main one: `cloudMarchCS` (view loop + sun
cone), `cirrusMarchCS`, `fogMarchCS`, `cloudGroundShadow`, and `beam_cloud_block.comp`'s column
march. `wrapTexel`/`wrapTexel3` are deleted from both files — the sampler's REPEAT mode does that
wrap now, so it is load-bearing again rather than vestigial.

Builds clean (both shaders recompiled, exe links). **Not yet verified in-app or measured** — that
is the next step, and it is the user's:

1. Look at a ground-level horizon view with cloud cover, then the same view from orbit. Nothing
   should have changed visually. The one thing worth looking for specifically is the warp field:
   T1.2 is where a regression would show, as the "tessellating triangle" faceting described in
   `cloud_warp_noise.comp`'s header.
2. `savePerfSnapshot()` before/after at matched camera state — ground-level facing the horizon is
   the case T1.1 targets and where the biggest delta is expected. Then
   `tools/perf_analysis/.venv/Scripts/python.exe tools/perf_analysis/analyze_profile.py`.
3. Knockout bit 64 (cloud self-shadow cone) still isolates the cone, which is what Tier 3 is
   aimed at — worth a reading now so Tier 3's win can be judged against a current number.

---

## Tier 2 — repack the noise bakes  ✅ DONE (2026-08-04)

Touches `cloud_noise.comp` and every `cloudDensity` consumer, so it landed as one change.

- [x] **T2.1 — presence slices + erosion into one bake.** `cloudDensity` read the R channel at
      three fixed Z offsets as three separate 3D fetches, then built a **fixed weighted sum**
      `nsF.g*0.625 + nsF.b*0.25 + nsF.a*0.125` from a fourth. `cloud_noise.comp` now bakes:

      - `R` = presence (Perlin-Worley)
      - `G` = the erosion FBM, pre-summed at bake time (frees B and A)
      - `B` = presence shifted **+54 texels** in Z
      - `A` = presence shifted **+108 texels** in Z

      Presence is one fetch (`.rba`), erosion one more (`.g`). **4 → 2.**

      *The plan originally flagged this as not output-identical.* It is, because the offsets were
      moved onto integer texel boundaries first: 0.28/0.56 are 53.76/107.52 texels, so they could
      not be packed — 54/108 can, and **a whole-texel shift commutes with trilinear filtering**,
      so `.b`/`.a` return exactly what a Z-offset read of `.r` returned. The bake evaluates
      `perlinWorleyAt(uvw_k + 54/192)` = `perlinWorleyAt(uvw_{k+54})` by construction, and the
      noise is periodic with period 1 in uvw (every octave wraps its cell index mod N), so the
      volume-boundary wrap is exact too — matching the sampler's REPEAT on W.

      Net behavioural change: each presence slice moved by **under a quarter texel**. The erosion
      sum's weights total exactly 1.0, so the baked value spans the same [0,1] and now takes one
      round of RGBA8 quantization instead of three — marginally better precision, not worse.

- [x] **T2.2 — column noise, with no new texture.** `colNoise`/`baseNoise` were the R channel at
      Z + 0.25 and Z + 0.85. 0.25 is exactly 48 texels, so one fetch at `uvwCol + 48/192` gives
      `colNoise` in `.r` — and that same fetch's `.a` is the presence field a further +108 texels
      up, i.e. Z + 0.8125. Pointing `baseNoise` at that makes it **2 → 1 with no new volume, no
      new bake, no new binding.**

      `baseH`'s only job is to be a per-column value decorrelated from `colH`'s; the exact slice
      is arbitrary. 0.8125 vs 0.85 is a 7-texel shift within that arbitrary choice, and the 0.5625
      separation from `colH` is still >2 cells at the coarsest Perlin octave (N=4). The
      base-height undulation *pattern* changes; its character and statistics do not. At
      `cloudBaseVariance == 0` there is no visible change at all.

### As landed (2026-08-04)

Files: `cloud_noise.comp` (rechannel), `include/common.glsl` (channel-layout constants + the
invariant that keeps them honest), `cloud_march.comp`, `beam_cloud_block.comp`, `sat_sky.frag`
(stale channel comment only — its one reader is `#if CLOUD_DEBUG == 6`, off).

Per-sample texture ops, cumulative:

| | original | after T1 | after T2 |
|---|---|---|---|
| in-cloud primary sample | 15 | 8 | **5** |
| sun cone step | 7 | 7 | **4** |
| **full in-cloud sample** (primary + 12 cone steps) | **99** | **92** | **53** |
| empty step | 11 | **1** | 1 |

Two things came along with it:

- **`beam_cloud_block.comp` now `#include`s `common.glsl`.** It was hand-declaring `PI`,
  `R_EARTH`, `raySphere`, `rotateZ`, `remap`, `kCloudHorizFreq` and `kCloudColFreq` — every one
  verified byte-identical to the shared version before deletion. This is the *declarations and
  pure helpers* category CLAUDE.md's `optDepth` note explicitly says is safe to share: no trip
  count becomes a runtime parameter, so no unrolling is lost.
- **Bake cost rose ~1.8×** (5 Worley octaves per texel → 9), paid once at init, not per frame.

**Not yet verified in-app.** What to look at: cloud silhouettes generally (T2.1's quarter-texel
slice shift), and cloud *base* undulation specifically if `cloudBaseVariance > 0` (T2.2 uses a
different arbitrary slice — expect a different pattern, not a worse one).

### Measured baseline (post-T1, pre-T2)

RTX 3070 Ti, 1920×1009, render scale 1.0, sea-level ocean at 4.6°N 158°W, elevation 3.4°
(near-horizon), `cloud_march_steps` 215, `cloud_light_steps` 12.9, knockout mask 0:

| pass | ms | share |
|---|---|---|
| **cloud_march** | **14.52** | **63%** |
| sky_background_draw | 7.14 | 31% |
| scene_depth / flare / orbit / draws / UI | ~1.26 | 6% |
| **total GPU** | **22.92** | 38.7 fps |

Only one snapshot exists, so this is a baseline, not an A/B — T1's own win is unmeasured. Worth
capturing at the same spot now, plus one with knockout bit 64 (self-shadow cone) set, to size
Tier 3 before starting it.

---

## Tier 3 — restructure the light march (where the 85% is)

Each item is independently landable and each changes the image slightly, so land them one at a
time with a look pass between.

**Measured, 2026-08-04 — the cone is 72% of `cloud_march` and 52% of the whole GPU frame.** Clean
A/B at the intro spawn, same pose, 15 s apart: mask 0 = 15.82 ms `cloud_march` / 22.08 ms total /
39.5 fps; mask 64 (cone off) = 4.39 ms / 10.26 ms / 70.5 fps. This is the largest single cost in
the renderer by a wide margin.

- [x] **T3.1 — geometric cone stepping.** ✅ DONE 2026-08-04. `coneSeg` was constant. Self-shadow
      is dominated by the cloud immediately sunward; the far end is a slowly-varying bias, and
      uniform steps spent the same budget on both. Each step now grows by `kConeStepRatio = 1.35`.
      The lengths are a geometric series summing to **exactly `coneLen`**, so total reach and
      shadow brightness are preserved at any count (verified numerically at N = 13/10/6/3).

      At N=13 the first step is 136 m vs 1448 m uniform — ~10× finer where the detail is — and the
      last is 4982 m. **This item does not save time on its own; it buys the headroom to lower
      "Light steps."** Try 13 → 6-8.

- [x] **T3.3 — reuse the cone RESULT along the view ray.** ✅ REDESIGNED 2026-08-04 after the
      first version caused visible noise. Read this before touching cone sampling again.

      *v1 (reverted):* scale `N_CONE` by `clamp(T/0.5, 0.25, 1)`. It hit its cost target —
      measured cone 11.43 ms → 5.00 ms, a 2.3× cut — but bought it by raising the **estimator
      variance of a Monte-Carlo integral**, and `coneJitter` is per-*pixel*, so that variance
      lands directly as per-pixel shading noise. Reported as "major noise below 13 light steps,
      extreme at 6" — which is the same mechanism reached by hand via the slider.

      **The general lesson: the cone's sample count is a variance knob, not a cost knob.** Any
      future attempt to make it cheaper by taking fewer samples will re-run this.

      *v2 (current):* the cone integral is a function of WHERE the sample is, not of which march
      step found it. Consecutive in-cloud samples are ≤250 m apart (often ~100 m — `stepLen`
      shrinks with `|dir.z|`) while the cone spans up to ~19 km, so the result varies slowly
      between them. One **full-quality** estimate is now cached and reused across a stride:

      ```
      strideM = 500 m * (1 + sampleDist/30 km) / max(T, 0.15),  capped at 20 km
      ```

      Same cost reduction, per-estimate variance untouched. `N_CONE` is back to the full slider
      value. Correlating neighbouring samples along the ray, if anything, reads calmer than
      jittering each independently. Only the raw cone integral is cached — `shadowFade` and
      `grazeAtten` stay per-sample, since they depend on this sample's own distance and sun
      elevation rather than on cone geometry.

- [x] **T3.1 jitter fix** — a second, independent noise source, and a regression introduced by
      T3.1's first version. The jitter was `coneJitter * seg`, i.e. scaled by *each step's own*
      length. After geometric growth the last step is ~5 km long **and** carries the largest
      `density * seg` weight in the sum, so its position was being randomised over its full 5 km —
      putting the single biggest variance contributor in the far field, which the cone is only
      meant to supply a slowly-varying bias for. Now: midpoint sampling (unbiased) plus a jitter
      of fixed amplitude `±coneSeg0/2` (the first, smallest step). Still decorrelates neighbouring
      pixels in the near field, which is what the dither is for.

### Why `shadowMaxDistM` could not be extended, and why it now can

`cloud.shadowMaxDistM` (~22 km) meant only near-zenith cloud was ever self-shaded, and pushing it
out cost far too much — because cone cost scaled with how much of the screen was BOTH cloud and
in range. It was acting as a **cost** control wearing the clothes of a **look** control.

The distance term in the stride absorbs exactly that: at 200 km the stride is kilometres, but so
is the depth interval it covers, and cloud detail there is sub-pixel. Distant cloud keeps its
shading and simply updates less often along the ray. `shadowMaxDistM` should now be raisable to
whatever looks right — **that is the next thing to try, and it needs a look pass plus a fresh
capture pair.**

- [ ] **T3.2 — low-frequency density in the cone.** cloud_march.comp calls the full `cloudDensity`
      including the erosion octave; erosion-scale detail is invisible in self-shadowing. A
      `cloudDensityLowFreq` (presence + height profile only) drops the cone from 4 fetches/step to
      3 and removes the `remap`/edge-bias chain. **Deferred deliberately:** it is the smallest of
      the three cone items (25%) and the only one whose look change is unquantified — dropping
      erosion makes cone density systematically *higher* (nothing carves it away), so shadows
      darken and it needs a compensating factor tuned in-app rather than derived.

- [ ] **T3.4 — distance-based step growth along the view ray.** `stepLen` is constant for the
      whole ray (cloud_march.comp:1022): a sample 380 km out gets the same 250 m as one 2 km out,
      far below one pixel of angular detail there. This is the LOD-along-ray that makes EVE's long
      render distances affordable, and it targets the 800 km `maxRenderDistM` / 3744-iteration
      `hardCap` directly.

- [ ] **T3.5 — bigger coarse step + step-back.** `bigStep` is only 2× (cloud_march.comp:1024) and
      there is no rewind on first hit, so the leading edge accumulates extinction using the
      *coarse* step (cloud_march.comp:1120) — over-counting it by 2×. Standard form is 4-8×
      coarse with a one-step rewind into fine resolution: a fix and a speedup at once.

- [ ] **T3.6 — coarse-mip space skipping.** One `textureLod(earthCloudsTex, uv, 6.0)` says
      whether a ~300 km neighbourhood is clear; if so, jump instead of stepping. One fetch to
      skip dozens of steps.

---

## Domain-warp shear (2026-08-04) — not a perf item, but found while chasing one

Reported after the T1.2 revert: the warp "does a good job at large scale cloud structure
movements, but it pinches the noise texture significantly and creates a lot of horizontal
banding. Clouds look like wavy chips."

**Diagnosis.** A domain warp `x → x·base + A·W(f·x)` stays a well-behaved (non-folding)
deformation only while `A·f·|∇W| / base < 1`. Past 1 the Jacobian determinant passes through zero
and the field **folds** — mirrored back on itself and infinitely compressed along one direction at
the fold line. Measuring the actual terms (`|∇W| ≈ 2` in the bake's cell space — the FBM's 2×
octave contributes nearly as much *gradient* as the base octave despite only 0.3 amplitude
weight):

| lookup | base freq | shear ratio, old | verdict |
|---|---|---|---|
| detail / presence | `kCloudHorizFreq` = 480 | **0.8** | at the edge; peaks exceed 1 |
| per-column `colH`/`baseH` | `kCloudColFreq` = 80 | **4.8** | folded several times over |

**Root cause: one *absolute* warp offset added to two lookups whose base frequencies differ by
6×.** It was tuned as a sensible fraction of the 480 domain, which makes it 6× too strong for the
80 domain. `colH` sets each column's cloud top and `baseH` its base, so a folded field there is
precisely "wavy chips" with horizontal banding.

**Two changes:**

- **Per-lookup scaling (structural).** The column lookup now takes `warpUVW * kWarpColScale`,
  `kWarpColScale = kCloudColFreq / kCloudHorizFreq = 1/6`. This makes the warp a constant
  *fraction* of whichever domain it is applied to — which is what "advect both fields together"
  actually means — so the two land at the same shear ratio instead of 6× apart. Applied at all
  four column sites (view march, sun cone, ground shadow, `beam_cloud_block.comp`).

- **`kWarpStrength`/`kWarpFreq` → `cloud.cloudWarpStrength`/`cloudWarpFreq`** (UBO, repurposed
  from `pad16`/`pad17`, zero size change; sliders "Warp strength" / "Warp frequency"; persisted).
  Shear is the **product** of the two, while the large-scale structure movement worth keeping is
  the **amplitude alone** — so the two knobs are exactly the balance the question was asking for:
  *raise strength and lower frequency* for big movement without pinching.

  Defaults: strength **32** (unchanged — displacement magnitude preserved) and frequency **6 → 3**,
  which halves the shear. Both domains now sit at ratio **0.4**, comfortably clear of folding.

`cirrusDomainWarp` reads the same two values, so cirrus shear drops proportionally.

---

## Tier 4 — architectural

- [ ] **T4.1 — blue-noise jitter on the primary cloud march.** `cirrusMarchCS` and `fogMarchCS`
      jitter their ray start off `noiseTex`; `cloudMarchCS` does **not** (cloud_march.comp:1025,
      `t = tEnter`). It is paying `cloudMarchSteps = 215` largely to suppress banding a jitter
      would trade into noise — which the half-res→full-res bilinear upsample then partly averages
      away. This is why production renderers run 64-128 steps where this runs 215. Needs a look
      pass to pick the step count / noise trade.

- [ ] **T4.2 — temporal amortization (the real multiplier).** Update 1/4 or 1/16 of the texels
      per frame in a Bayer pattern, reproject history, blend. 4-16× effective reduction.

      **This codebase is unusually well suited to it:** `SkyCamera` is a rotation about a fixed
      observer for most interaction, so reprojection is *exact* — transform the current view ray
      through the previous `skyView` and sample. No depth buffer, no motion vectors, no
      disocclusion holes. WASD translation adds parallax, small at 5-15 km cloud distance, and
      handled by weighting history down on large `|Δobs|`.

      Cost: two more half-res RGBA16F images (ping-pong), `prevSkyView` in `CloudMarchPC`, a
      frame counter for the Bayer index, a history-reject term on camera delta.

- [ ] **T4.3 — occupancy.** `cloud_march.comp` is one ~2000-line kernel running cirrus + cloud +
      fog + aurora + airglow-red + beam debug + ground shadow. Register allocation is set by the
      union of all paths, so every texel pays the worst case; at `local_size 16×16` (256 threads)
      that is likely well under 25% occupancy. Two things to measure with the existing timestamp
      harness: dropping to `8×8`, and splitting the high-altitude passes (aurora + airglow-red,
      additive into `B_total`, sharing almost nothing with the cloud path) into their own
      dispatch.

---

## Explicitly not doing

- **A transmittance LUT for `optDepth`.** CLAUDE.md records that session 29's profiling measured
  its isolated cost near zero and overturned the session-24 guess. `skyAmbientBase` and
  `camPathInscatter` are per-pixel, not per-sample — second-order next to the light march.
- **EVE's flat-2D-beyond-a-distance LOD.** Already present (`cloudDistFadeStartM/EndM` +
  `evalCloudLayer`), and already keyed on the right variable (`tEnter`).
- **EVE's cloud-type / height-gradient LUT.** A look feature, not a perf one; the 3-slice
  presence at cloud_march.comp:702-713 covers the same ground more cheaply.
- **Sharing `optDepth` between shaders.** See CLAUDE.md — tried, measurably slower, reverted.
