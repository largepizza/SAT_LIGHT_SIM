# Sky TAA: the still weight where the view moves less than a pixel (2026-10-31, c27)

**What changed.** `shaders/sky_taa.comp`: the ramp that raises the new-sample weight from the still weight
(0.1) toward `display.sky_taa_weight_moving` (0.35) with motion now yields to the still weight where three
things hold at once:

1. the eye's parallax (the translation's own reprojection offset) is under ~1 px/frame (fade 1 -> 2 px), so
   the ten samples a 0.1 weight holds land on the same place in the image;
2. the rotation's own displacement is under 0.5 px/frame — below where the pass's rotation ramp starts on
   purpose (the Glasgow city-light fix);
3. `par x (dHi3 - dLo3)` is under c23's 0.10-0.30 bound (a 3x3 that spans sky and ground is excluded).

Where any of them fails, the ramp is exactly the previous form, so the change can only *add* history: no view
gets sharper and no weight rises. The rotation displacement is a new `rotPx` next to the existing `par`.

**Why.** The still weight is what averages the sparse cloud march's one-jittered-sample-per-visit; the
moving weight exists because a moving view must take its frame. Where the reprojection is exact both things
are wanted, so which one wins is a question of *how far the frame moves per frame* against the ~1/0.1 = ten
samples the still weight keeps. Measured per frame on the reporter's protocol (0.1 x AGL/s): 0.45-0.6 px at
5 158 m, 1.3 at 47 500 m, 3.2 at 1 320 km.

**Numbers** (`qa/waves/20261030b/evidence/c27/numbers.md`, this worktree's Release build):

| view | flicker (>12 reversal share) | moving-vs-settled |
|---|---|---|
| 5 158 m, 0.1 x AGL/s | 0.0237% -> **0.0076%** (-68%) | 1.24 -> 1.29 |
| 47 500 m | 0.0228% -> **0.0173%** (-24%) | 1.84 -> 1.77 |
| 1 320 km (orbit) | 0.4171% -> 0.4174% (+0.07%, noise) | 1.33 -> 1.35 |
| 12 m over land, overcast | unchanged (0.0000 / 0.0006%) | 2.88 -> 2.89 / 1.96 -> 1.96 |
| 5 158 m at the player's 1 x AGL/s | 0.6078% -> 0.6076% (gate does not fire) | 2.41 -> 2.43 |

Yes, the stills are untouched (lap_mean 2.86 / 1.51 / 2.44). cloud_suite over the 6 scenes the gate can fire
on: stills, paused drift and time-running unchanged, motion +0.00..+0.10 (the largest is a climb path's
bottom fifth).

**What it is not.** A fix for the reported "clouds flicker around the edge while moving": at the player's own
1 x AGL/s the flicker is 0.61% at the 5 km view and does not move, because 8.5 px/frame fails the 1-px test
by an order of magnitude. The orbit view's flicker is unchanged by design (c23: it needs a per-pixel
latest-sample redesign of the sparse march). What this buys is the slow-drift / cinematic / hovering case
(the release cinematic's drift shots) plus the near-field sea speckle at altitude views.

**Protocol lesson (worth keeping).** `wait settle auto max=160` stops at a different frame count before and
after a temporal change (a run went 226.8 s -> 158.5 s), so later captures in the same script see a different
sim clock: that alone swung a placed scene's moving-vs-settled number by +-0.1 with no shader change. A fixed
`wait settle 160` makes before/after the same frames. `display.dynamic_resolution false` too, so the render
scale (and therefore the per-frame pixel displacement the gate reasons about) does not adapt.
