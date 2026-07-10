# Cloud Sky-Dome Seam — Investigation Log (RESOLVED)

**RESOLVED — confirmed visually fixed.** Root cause: `cloud_noise.comp`'s Worley octave
frequencies must be **powers of 2**. Any non-power-of-2 N (regardless of whether it divides the
192³ bake resolution) bakes a real, reproducible discontinuity at the texture's own domain seam
(u,v,w≡0), which then reappears as a family of circles (constant-`dirECEF`-axis planes
intersected with the unit sphere) wherever the noise is sampled at scale across the globe. Fix:
every octave frequency in `cloud_noise.comp` is now a power of 2 (Perlin R: 4/8/16, Worley
floor: 2/4, erosion G/B/A: 8/16/32). See finding #13–#15 below for the full evidence chain
(two independent RenderDoc measurement rounds) — **do not introduce a non-power-of-2 octave
frequency in this file without reading that first.** The exact mechanism (why non-power-of-2
constant N breaks the GPU's negative-operand integer modulo) was never confirmed via
disassembly — treat it as a strong, empirically-proven constraint, not a fully explained one.

The rest of this document is kept as-is (unresolved-session framing) as a record of the full
investigation, including ~13 dead-end hypotheses that are still worth avoiding if a *different*
seam-like artifact shows up in the future — don't assume every visual cloud artifact has this
same root cause.

---

## Symptom

Looking up at the volumetric cloud layer (base 2 km, top 11 km — see `cloudMarch` in
`sat_sky.frag`), a grid of hard seams is visible cutting through the cloud noise. Confirmed
properties, established across many rounds of live testing:

- **Aligned with lines of latitude/longitude**, not with screen space or camera orientation.
  Near the equator the grid spacing was originally estimated at ~1°, though the observer later
  measured closer to ~0.1° from a different vantage — take neither as exact, they were rough
  visual estimates.
- Forms clean **4-way crossings** (a "+" shape) where a lat-line and a lon-line intersect —
  looking straight up at one such crossing shows 4 quadrants of visually distinct, internally
  coherent noise meeting at a sharp point, with clean horizontal + vertical dividers.
- Cloud **top silhouette height is discontinuous across the seam** ("tops of clouds are not
  perfectly aligned at the seams") — this isn't just a density/shading artifact, the actual
  cloud geometry differs on either side.
- Present in both the volumetric march (`cloudMarch`) output and the raw density value `d`
  computed by `cloudDensity`, down to the raw single-texel noise sample in some tests (see
  below — this result was inconsistent across attempts, see "Contradictory evidence").
- Did **not** exist before the `cloud_noise.comp` rebake (128³ → 192³, new hash, new octave
  frequencies) earlier in the session, even though the coordinate system (`dirECEF`-based
  sampling) it's now blamed on was already in place before that rebake. This timing is a real,
  unexplained clue — see "Open questions" below.

---

## Confirmed NOT the cause (tested and ruled out this session)

Each of these was a genuine hypothesis, implemented, built, and visually verified live —
not just reasoned about. All failed to fix the seam:

1. **Equirect UV pole singularity** (`atan2`/`asin`-based coordinate) — this was a *real* bug
   fixed early in the session (poles are now clean, confirmed), but is unrelated to this seam,
   which appeared *after* that fix, nowhere near the poles.
2. **`gradHash`/`worleyTile` hash function precision** — replaced the classic
   `fract(sin(dot(...))*C)` hash (which has known GPU precision loss for large dot-product
   arguments) with an integer bit-mixing hash (`hashU`/`hash3`, pcg3d-style). Seam unaffected.
3. **`posZ` discontinuity** — the old `fract(pUV.x*340+pUV.y*460)` was a genuine sawtooth
   discontinuity bug (real, but separate from this seam) fed into the noise Z-coordinate; fixed
   with a smooth `sin()`-based formula. Seam unaffected.
4. **`earthCloudsTex` (2D coverage map) texel noise hitting `cloudDensity`'s hard threshold** —
   bumped sample LOD from 2/3 up to 4/5 (heavy blur). Seam unaffected. Then tested more
   definitively: **forced coverage to a uniform constant, bypassing `earthCloudsTex` sampling
   entirely.** Seam still present, identically. This conclusively rules out the 2D coverage
   texture as a cause.
5. **Manual `fract()` before `texture()`** — theorized that pre-wrapping the coordinate before
   an implicit-LOD `texture()` call creates a derivative discontinuity that could bias sampling
   near tile edges. Removed **every** manual `fract()` around `cloudNoiseTex` sampling
   (`cloudDensity`'s two texture reads, `uvwDetail` construction at all 3 call sites, `colNoise`),
   relying purely on the sampler's `REPEAT` address mode. Seam unaffected, pixel-for-pixel
   identical in the debug view.
6. **Octave frequency harmonic alignment** — original Perlin FBM used 4/8/16 (exact power-of-2
   multiples) and Worley floor used 3/6 (exact 2×); their cell boundaries all coincide at the
   same fractional tile positions, which could reinforce a per-octave gradient-noise boundary
   bias. Rebaked with decorrelated (coprime-ish) frequencies: Perlin 4/7/13, Worley floor 3/5,
   erosion 8/17/37 (was 8/18/36). Seam unaffected, still forms a clean grid.
7. **Erosion frequency vs. bake resolution (undersampling/bilinear-faceting)** — theorized that
   at extreme near-zenith zoom, the ~5 texels/cell representing the finest erosion octave (freq
   37 in a 192³ volume) could show bilinear-interpolation faceting that reads as a seam. Rebaked
   with drastically reduced erosion frequencies (3/4/5 — very coarse, ~40-60 texels/cell). Seam
   still present at the same location, though the overall cloud look became much blobbier.
8. **`kCloudHorizFreq` tile-boundary proximity** — the strongest structural theory: `dirECEF`'s
   own Cartesian components (X/Y/Z) cross integer multiples of `1/kCloudHorizFreq` along lines
   of exact constant latitude (Z) and near-constant longitude close to the equator (X/Y) — this
   is mathematically real (see "What's confirmed true" below) and was the original motivating
   theory. Tested directly and cheaply (no rebake — just dropped `kCloudHorizFreq` from 480 to
   2.0, which keeps the *entire visible sky* deep inside a single tile, nowhere near any
   boundary crossing). **Seam still present, unchanged.** This is a very strong negative result:
   if the seam tracked tile-boundary proximity, it should have vanished here. It didn't.
9. **A `cloudDomainWarp()` perturbation** (smooth `sin()`-based offset added to `dirECEF` before
   scaling, meant to break up coherent alignment along lat/lon curves) — implemented and tested.
   Made things **worse**: seam frequency visually increased roughly 10×. Reverted. Left
   unexplained; see "Open questions."
10. **Mesh/primitive seams** — the sky is drawn as a single full-screen triangle
    (`vkCmdDraw(cmd, 3, 1, 0, 0)`), confirmed in `SatelliteSim.cpp`. No multi-primitive mesh
    exists, so a mesh-boundary seam is structurally impossible. Ruled out by inspection.
11. **Vertex shader interpolation** — `sat_sky.vert` computes `enuDir` as a straightforward
    linear/affine function of screen position (standard full-screen-triangle ray reconstruction,
    `mat3(transpose(skyView)) * camDir`). Nothing non-linear or seam-prone. Ruled out by
    inspection.
12. **[RETRACTED — see #13] `cloud_noise.comp` bake not tiling at its own volume boundary** — a
    RenderDoc capture was taken and the raw 3D `cloudNoiseTex` was inspected directly (Texture
    Viewer, scrubbing Z slices), motivated by wanting hard data instead of another screenshot
    round-trip. Exporting slice 0 and slice 191 as PNGs and eyeballing them side by side looked
    like a real seam. This was initially "ruled out" using a from-scratch Python reimplementation
    of the bake math, which proved the *formula* is exactly periodic (`f(u+1)=f(u)` to float
    epsilon) and showed no anomaly in a wrap-vs-internal jump comparison. **That reimplementation
    was then cross-checked pixel-for-pixel against the actual exported PNG and found to disagree
    substantially for the higher-frequency channels (B, A), including at interior points nowhere
    near any edge** — meaning the Python model does not faithfully reproduce what the GPU actually
    baked (root cause of that specific discrepancy still unknown — possibly a transcription bug,
    possibly a real GPU/compiler difference from the idealized formula). **The "ruled out"
    conclusion was wrong and is superseded by #13**, which re-tested using only the real exported
    PNG data with no model involved.
13. **`cloud_noise.comp` bake has a real, localized defect at its own domain seam (u,v=0↔1) —
    CONFIRMED, mechanism still open.** Redid the tiling test using *only* the actual exported
    `slice0.png`/`slice191.png` pixel data (no reimplemented math, so immune to whatever caused
    #12's model to diverge). Two results:
    - **The texel(191)→texel(0) wrap boundary shows a real, non-random discontinuity**, on both
      the X and Y edges: e.g. R channel jumps mean=13.9 (max 39) at the wrap vs. mean=0.88
      (max 4) for typical adjacent-texel jumps elsewhere in the same slice — a 16x excess. B
      channel: 31.2 vs 11.2 (2.8x). G channel (Worley N=8 only) shows *no* excess (4.9 vs 5.0) —
      the anomaly is channel/octave-dependent, not universal.
    - **Every *internal* Worley cell-grid corner is clean.** Tested the grid-line crossings for
      every octave N used in the bake (3,4,5,7,8,13,17,37 — i.e. every `k/N` position for
      `k=1..N-1`) and all of them measure statistically identical to normal background noise
      variation (mean/max match the typical adjacent-texel jump distribution). **The defect is
      isolated to exactly the outer domain edge (`k=0`, i.e. u≡0≡1), not a general grid-corner
      instability.**
    - **Why this explains the visible sky seam, and reconciles hypothesis #8's negative result**:
      `dirECEF` (the sphere direction used for all noise sampling) crosses exactly zero on each
      Cartesian axis along real geographic great circles — `Z=0` is the equator, `X=0`/`Y=0` are
      meridian pairs. Scaled by `kCloudHorizFreq=480`, every subsequent integer crossing of
      `dirECEF.axis * 480` as you sweep latitude/longitude lands back on this broken domain edge
      — roughly every `1/480` rad ≈ 0.12°, close to the "~0.1°" spacing estimate. Separately, the
      erosion Z-coordinate (`hNorm * kVertTiles` with `kVertTiles = 1.5`) sweeps a range **wider
      than 1.0** within every single march column, so it crosses this same broken seam on *every
      ray regardless of `kCloudHorizFreq`* — this is why hypothesis #8 (dropping
      `kCloudHorizFreq` to 2.0) didn't fix it: shrinking the horizontal tiling frequency does
      nothing to stop the vertical erosion sweep from crossing the domain edge on every column,
      and does nothing to stop the geographic equator/meridians (fixed zero-crossings of
      `dirECEF`, independent of the frequency multiplier) from doing the same.
    - **Confirmed real, not an export artifact.** RenderDoc's live Texture Viewer pixel picker
      (UNORM float readout, not PNG export) at texel (0,96) vs (191,96) gave `(0.73725, 0.52157,
      0.4549, 0.52549)` vs `(0.72941, 0.56471, 0.41569, 0.77647)` — ×255 this is
      `(188,133,116,134)` vs `(186,144,106,198)`, an **exact match** to the PNG export values.
      Alpha's jump alone (`0.52549→0.77647`, ≈64/255) exceeds the largest jump measured *anywhere
      else* in the whole texture for that channel (max internal jump was 50/255). Not a RenderDoc
      export quirk — this is what the GPU actually stored.
14. **Root cause isolated: octave frequency N must evenly divide the bake resolution (192).**
    Added a temporary diagnostic bake (`NOISE_BAKE_DEBUG` switch in `cloud_noise.comp`, since
    removed) outputting four isolated single-octave `worleyTile` channels to separate "doesn't
    evenly divide 192" from "just high/undersampled frequency": R=N8 (divides, low freq, known
    clean control), G=N32 (divides, HIGH freq), B=N37 (doesn't divide, known-broken control),
    A=N5 (doesn't divide, LOW freq). Full-slice wrap-vs-internal-jump ratio, measured from real
    exported PNG data:

    | Channel | N | Divides 192? | X-wrap ratio | Y-wrap ratio |
    |---|---|---|---|---|
    | R | 8  | yes | 0.98x | 1.13x |
    | G | 32 | yes | 1.05x | 0.75x |
    | B | 37 | **no** | **1.80x** | **1.72x** |
    | A | 5  | **no** | **3.99x** | **10.27x** |

    Both divisors are clean regardless of frequency (N=32 is high-frequency and still clean).
    Both non-divisors are anomalous regardless of frequency (N=5 is *lower* frequency than N=37
    and *more* anomalous in relative terms) — this rules out "high frequency/undersampling" as
    the mechanism and isolates it specifically to N-vs-192 divisibility. **Mechanism still not
    understood** (the continuous formula is provably exactly periodic regardless of this
    relationship — see #12/confirmed-true section — so whatever's actually happening lives in
    the gap between formula and GPU execution/storage, not yet found by inspection). But the
    empirical rule is clean, strong, and reproduced across two independent bakes.
    **Fix applied** (`cloud_noise.comp`): every octave frequency changed to a divisor of 192
    (`= 2^6*3`, valid set: 3,4,6,8,12,16,24,32): Perlin R 4/7/13 → **4/8/16**; Worley floor 3/5 →
    **3/6**; erosion G/B/A 8/17/37 → **12/24/32**. Build verified clean.
    **RESULT: seam persisted after this fix.** New observation from the live render (round 2):
    the seams are **not strictly a lat/lon grid** — they cut across at arbitrary angles, and each
    one traces a *complete circle* all the way around the planet. This is a strong additional
    confirmation of the underlying mechanism (still consistent with #13): a plane of constant
    `dirECEF.x`, `.y`, or `.z` intersected with the unit sphere is *always* a full circle: only
    the `=0` cases (equator, meridian pair) align with the geographic grid, every other
    integer-crossing of `dirECEF.axis * kCloudHorizFreq` produces a circle at an "arbitrary" tilt
    relative to lat/lon. This also rules out anything lat/lon-specific (`earthCloudsTex`, `pUV`)
    as the mechanism, consistent with prior ruled-out hypotheses.
    **Refined hypothesis (round 2) — CONFIRMED.** The two confirmed-clean N values from round 1
    (8, 32) were *both powers of 2*; the two confirmed-broken values (5, 37) were both
    non-divisors — "divides 192" and "is a power of 2" were never actually separated. Tested
    directly: N=3, 6, 12, 24 (all divide 192 exactly, none a power of 2) — **all four badly
    broken**, ratios 2.55x–19.15x (worse than the original N=37 in some cases). Combined with
    round 1, the rule is now airtight across 8 tested values: **every power-of-2 N is clean,
    every non-power-of-2 N is broken, regardless of whether it divides 192.** "Divides 192" was
    a red herring — the round-1 fix happened to include two power-of-2 values (8, 32) by
    coincidence, which is why it partially worked, but also included non-power-of-2 divisors
    (3, 6, 12, 24) which were still broken and explain why the seam persisted.
15. **Fix, round 2: every octave frequency changed to a power of 2.** Perlin R stayed 4/8/16
    (already powers of 2). Worley floor 3/6 → **2/4**. Erosion G/B/A 12/24/32 → **8/16/32**.
    Likely mechanism (not verified via disassembly): `cell.x % int(N)` in `gradHash`/`worleyTile`
    is only ever evaluated with a *negative* `cell.x` right at the domain edge (texel 0's `dx=-1`
    neighbor cell). For power-of-2 N this reduces to a bitmask that's correct for negative two's-
    complement operands by construction; for non-power-of-2 constant N, compilers commonly lower
    `%` to a multiply-by-reciprocal "magic number" trick, a known place for signed/negative-
    operand bugs. Build verified clean. **Not yet visually confirmed in the real render — this
    is the next thing to check.**

## Contradictory / inconclusive evidence (important — read before re-testing)

- **Two of the `CLOUD_DEBUG=4` tests were themselves flawed**, and produced misleading results
  before being corrected:
  - First version displayed the raw `fract()`-wrapped `uvwDetail` coordinate directly as color.
    This **trivially** shows a hard color jump at every tile edge by construction (that's what
    `fract()` does) — it says nothing about whether the actual *sampled texture value* is
    continuous there. Looked like "proof" of a coordinate bug; wasn't.
  - Second version sampled the actual texture color, but at a **different march sample** than
    where the density seam actually forms (`i == N/2`, a fixed step index, vs. the real seam
    location at the first `d > 0.001` hit). Showed "smooth" — but was testing the wrong point,
    so this "smooth" result should not be trusted either.
  - Third version (corrected: samples exactly at the first-hit point, same as `d`) showed a
    **faint** seam even in the raw single-texel `R` channel (no math applied at all beyond one
    `texture()` call). This directly contradicts hypothesis #5 above (fract removal, tested
    after this and found not to matter) and is hard to reconcile with #8 (kCloudHorizFreq test)
    showing no seam at all when the whole screen is deep inside one tile. **These two results
    are in tension and were never fully reconciled.** Possible explanations not yet explored:
    the "first-hit" sample in adjacent screen pixels may not correspond to the same *kind* of
    point once the adaptive step size (`bigStep`/`stepLen` in `cloudMarch`) diverges between
    rays; or there may be a genuine but small artifact that gets amplified downstream, distinct
    from the dramatic seam.
- The `cloudDomainWarp()` test (#9) made the seam *worse* rather than removing it, which is not
  well explained by any theory currently on the table. A smooth perturbation should not be able
  to *increase* a discontinuity's visibility unless it's interacting with something not yet
  understood (possibly aliasing/moiré between the warp's own frequency and the existing
  480×-per-globe tiling — not investigated further).

## What's confirmed true (not in question)

- `dirECEF = normalize(pECEF)` sampling is mathematically pole-safe and isometric (no scale
  distortion) — this was the correct fix for the *original* polar noise-compression bug, and
  remains correct. It is a *separate* issue from this seam.
- `dirECEF`'s Cartesian axis-crossings genuinely do align with lines of constant lat (Z axis,
  exactly) and near-constant lon close to the equator (X/Y axes) — this is basic trigonometry,
  not in question. What's in question is whether this geometric fact has anything to do with
  the observed seam, given hypothesis #8 above found no effect from moving far away from any
  such crossing.
- `worleyTile`/`perlinTile`/`gradHash`, evaluated as an idealized formula (verified via a from-
  scratch Python reimplementation), are exactly periodic with period 1 in their `uvw` input for
  any integer `N` — `f(u) = f(u+1)` to float epsilon. **This is true of the formula in the
  abstract, but per #13 below, the actual baked texture does NOT match this reimplementation for
  the higher-frequency channels even away from any edge, and DOES show a real discontinuity at
  the domain seam.** So this bullet describes the intended math, not a proven property of what's
  actually in `cloudNoiseTex` — treat it as a design intent, not a guarantee, until #13's open
  question (mechanism) is resolved.

## Open questions / not yet tested

These are the live leads for the next session, roughly ordered by how cheap they are to test:

0. **[DONE — see #13/#14] Confirm domain-edge defect is real, and isolate the mechanism.**
   Confirmed real via live pixel picker (not export artifact). Isolated to "N doesn't evenly
   divide 192" via a 4-channel diagnostic bake (R=N8✓clean, G=N32✓clean, B=N37✗broken,
   A=N5✗broken — divisibility, not frequency, is the deciding factor). Fix applied: all octave
   frequencies in `cloud_noise.comp` changed to divisors of 192 (4/8/16, 3/6, 12/24/32).
   **NEXT: visually confirm the sky-dome seam is actually gone with this bake** — rebuild is
   already done and clean, just needs a look at the real render. If the seam persists even with
   divisor-only frequencies, the divisibility theory, while a real and now-fixed defect in its
   own right, isn't the (sole) explanation for the visible seam and the search continues (try
   open question #4 next: erosion-stage isolation, or #5: does the seam move with the observer).
   If the seam is gone, declare victory but keep the mechanism as a documented open curiosity —
   don't reintroduce non-divisor frequencies in future tuning without re-reading this doc.
1. **Reconcile the contradictory evidence above.** Before anything else, re-run the corrected
   `CLOUD_DEBUG=4` test (R=raw noise, G=Stage-1 base, B=final `d`, all sampled at the exact
   first-hit point — this capture code is still in `cloudMarch`, just gated behind
   `CLOUD_DEBUG == 4`, currently set to `0`) *and* the `kCloudHorizFreq`-near-zero test
   *simultaneously comparable* — e.g. run the low-frequency test with the R/G/B debug capture
   active too, at the same exact spot, so we're not comparing across sessions with slightly
   different camera framing.
2. **[RULED OUT this session]** Sun-shadow self-shadowing cone isolation
   (`CLOUD_ISOLATE_SHADOW 1`) — tested, seam persists unchanged. Not the (sole) cause.
3. **[RULED OUT this session]** `colH`/`topFade` column-height silhouette isolation
   (`CLOUD_ISOLATE_COLH 1`) — tested, seam persists unchanged. Not the (sole) cause. (The
   `kCloudColFreq` reasoning that motivated this test was sound, but the isolation test itself
   came back negative — the seam survives with `colH` fully bypassed, consistent with #13's
   finding that the defect is baked into `cloudNoiseTex` itself and reachable through multiple
   independent sampling paths, not specific to the colH call site.)
4. **Erosion stage (Stage 2) in isolation** — try short-circuiting `cloudDensity` to return just
   `base` (skip erosion entirely) and see if the seam is present in the *presence-only* path.
5. **Does the seam move with the observer, or is it geography-fixed?** Not tested this session.
   Move the observer to a very different lat/lon and re-check: does a seam still appear at
   zenith (suggesting something about the viewing geometry itself, e.g. `dir.z ≈ 1`, rather than
   a specific geographic coordinate)? Does the *specific* seam from this session's screenshots
   stay fixed at 56.3°N/83.5°W if you fly away and back?
6. **Direct GPU inspection via RenderDoc or Nsight Graphics.** Every test this session went
   through screenshot round-trips of color-coded debug visualizations, which is slow and (as the
   two flawed `CLOUD_DEBUG=4` attempts show) error-prone to interpret correctly. A real frame
   capture tool would let you read exact float values at exact pixels, inspect the actual
   `cloudNoiseTex` texel data directly with no shader involved, and step through execution. This
   would very likely have resolved the contradictory evidence above in one capture instead of
   several rebuild-and-eyeball cycles.
7. **Why did this only appear after the `cloud_noise.comp` rebake**, given the coordinate system
   was unchanged? Never explained. Worth deliberately reverting *just* the bake (128³, old hash,
   old 4/8/16+3/6+6/10/14 frequencies) while keeping the current `sat_sky.frag`, to see if the
   seam genuinely disappears with the old bake — if so, that re-opens the bake as a suspect
   despite hypotheses #6/#7 above not finding the specific mechanism.

---

## Current code state (as of end of this session)

All **diagnostic-only** overrides have been reverted back to real values — the build reflects
"best real fixes applied, seam still present," not any of the mid-session diagnostic states.

**Kept (believed to be genuine, independent improvements, unrelated to the seam):**
- Pole-safe `dirECEF` sampling for `cloudNoiseTex` (replacing equirect UV) — fixes the original
  polar compression/aliasing/perf bug.
- Integer bit-mixing hash in `cloud_noise.comp` (replaces sin-based hash).
- Smooth `sin()`-based `posZ` (replaces the `fract()`-sawtooth version — that was a real, if
  different, bug).
- `cloudDensity` Stage 1 threshold-then-blend (two archetype slices, post-threshold mix) —
  reduces the "perfect cylinder" tower look.
- 192³ bake resolution, decorrelated octave frequencies (4/7/13 Perlin, 3/5 Worley floor,
  8/17/37 erosion).
- No manual `fract()` before any `cloudNoiseTex` sample (relies on `REPEAT` addressing).
- `cloudShadowFactor` (ground/terrain cloud shadow) march-distance cap (60 km) — fixes grazing
  sun-ray undersampling, independent of this seam.
- Coverage texture LOD bumped to 4/5 (was 2/3) — harmless blur, doesn't hurt even though it
  didn't fix the seam.

**Debug infrastructure left in place for next session** (`sat_sky.frag`, `CLOUD_DEBUG` macro,
currently `0` = normal rendering):
- Mode 1: raw 2D coverage map value.
- Mode 4: at first in-cloud hit, R=raw presence noise sample, G=Stage-1 post-threshold base,
  B=final density `d` — all resampled at the same point (the *corrected* version).
- Mode 6: minimal direct `cloudNoiseTex` sample from view direction alone, bypassing the entire
  march (note: this specific mode's coordinate mapping is **not equivalent** to the real march's
  geographic sampling — it ignores the `R_EARTH` offset — so a clean result from mode 6 does
  **not** prove the real march path is clean; see hypothesis discussion above for why this
  mattered).

**Also added this session, both default OFF (`0`, normal rendering)** — see open questions #2/#3:
- `CLOUD_ISOLATE_COLH` — bypasses `colH`/`topFade`, forcing uniform full-height columns. Set to
  `1` and rebuild to test whether the noise-driven column-height logic (`kCloudColFreq=80`,
  never neutralized by the earlier `kCloudHorizFreq` test) is the seam's source.
- `CLOUD_ISOLATE_SHADOW` — hardcodes `sunOptDepth=0.0`, skipping the self-shadow cone loop
  entirely. Set to `1` and rebuild to test whether the shadow cone's parallel sampling logic
  contributes. Test these **one at a time**, not together, so a positive result is unambiguous.

**Not reflected in code, purely diagnostic and already reverted:** uniform-coverage override,
`kCloudHorizFreq = 2.0`, erosion frequencies `3/4/5`, `cloudDomainWarp()`.

---

## Process notes for next session

- **Use a GPU frame-capture tool (RenderDoc/Nsight) instead of more screenshot round-trips.**
  This is the single highest-leverage change available. Every hypothesis this session cost a
  full edit→rebuild→relaunch→re-navigate→screenshot cycle (several minutes each), and two of
  the "conclusive" debug-mode results turned out to be measuring the wrong thing — a mistake a
  real pixel/shader debugger would have caught immediately by construction.
- **Order tests cheap-to-expensive.** The `kCloudHorizFreq` test (a one-line constant change, no
  rebake) should have been tried before any of the `cloud_noise.comp` rebakes (each requiring a
  ~30s+ rebuild and a full re-navigation to the test spot). It ended up being one of the most
  informative negative results in the session and could have been had for free, early.
- **Bookmark the test camera state.** Testing relied on manually re-navigating to "the same
  spot" (56.3°N, 83.5°W, looking straight up) each time by hand. If the sim supports saving/
  loading a camera position, use it — small framing differences between screenshots make it
  harder to be sure two tests are really comparing the same location.
- **When reporting a visual result, note precisely:** which `CLOUD_DEBUG` mode was active,
  observer lat/lon, and roughly which screen quadrant the feature of interest is in. This
  session's reports were good about lat/lon; less consistent about confirming which debug mode
  was on screen, which contributed to the confusion between the two flawed mode-4 attempts.
- **A discontinuity visible in a debug view is only meaningful if the debug view displays a
  final sampled/computed VALUE.** Never trust a debug view that displays a `fract()`-wrapped (or
  otherwise intentionally-discontinuous) coordinate directly as color — it will show a seam at
  every tile edge by definition, whether or not anything is actually wrong.
