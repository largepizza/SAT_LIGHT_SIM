---
name: perf-ab
description: Measure whether a shader or setting change is faster (or changes the image) with an interleaved A/B in one live Sat Light Sim app — variant SPVs via shader_variant.py + `shaders reload`, setting variants via `set`, reps grouped by res.py --ab, parity by imgtools diff. Use for any perf claim, optimisation, cost attribution, or "did this change the look" check.
---

# perf-ab — interleaved A/B in one live app

Run-to-run noise between separate launches is 5-10 %, and other apps on the GPU add 20-100 ms. So: **one live app,
alone on the GPU, variants interleaved, 3 reps, compared against the same configuration run twice.**

## 1. The app

```bash
python tools/harness/live.py status            # someone else's app? use it, never stop it
SATLIGHT_HARNESS_MAX_APPS=1 python tools/harness/live.py start [--settings user_data/settings.json]
```

Uncapped timing needs `set display.fps_cap_mode 0` (VSync caps frames at 16.7 ms; GPU timestamps are not capped).
Automatic render scale off for timing: `set display.dynamic_resolution false`.

## 2. Variants

- **Shader**: `python tools/harness/shader_variant.py <march|resolve|lightning|composite|weather|depth|sky|sky_taa>
  --tag <name> -s "OLD=>NEW" [-s ...] [-f subs.txt] [-D NAME]` → prints `shaders reload <slot>=shaders/v_<tag>_<slot>.spv`.
  Build `--tag base` with no substitutions as the A side (byte-identical to the build's SPV). Each `-s` must match
  exactly once across the shader and every header, so a variant cannot silently miss.
- **Read the register count** in the `shaders reload` result (`res.py live -c shaders -g message`): the cloud march has
  a 128 / ~227-register cliff where trivial edits cost 20-60 %. Compare variants only at the same count, or report it.
- **Setting**: `set <key> <value>` per variant. A cloud-field setting refills the clipmap: `wait 160` after it.

## 3. The batch

Per view: set the view once (`report load ... trail=off`, or `time set` / `observer` / `camera`), `wait settle`, then
reps × variants interleaved, the same frames every time:

```
shaders reload march=shaders/v_base_march.spv
wait 60
perf frames=90 name=storm_A_r1
shaders reload march=shaders/v_nofar_march.spv
wait 60
perf frames=90 name=storm_B_r1
... r2, r3 ...
```

Write the batch with a few lines of Python (views × variants × reps) into `harness_runs/<topic>/ab.satcmd`, send it
with `live.py send -f`, and keep the generator next to it so a rerun is one command.

## 4. Reading it

```bash
python tools/harness/res.py live --ab -b cloud_march,sky_background_draw,scene_depth
```

One row per view/variant: n, mean frame, mean GPU ± half-range over reps, per-bucket means. Report a difference only
when it beats the A-vs-A spread (run `base` twice in the batch to get that floor). Per-pass cloud timers:
`res.py live -c state -g clouds_v2.pass_ms`.

## 5. Parity (did the image change?)

Capture each variant at the same pose and frame count (`capture <view>_<variant>`), then
`imgtools.py diff a.png b.png -o heat.png`; compare its mean / p99 against two captures of the SAME variant (the floor:
~0.5-0.9 levels in storm views). Look at the heatmap only if the numbers say something moved.

## 6. Write it down

A kept optimisation gets its measured numbers (machine, resolution, views, before → after, register count) as a
present-tense note in the matching `docs/subsystems/` file (usually `perf.md` or the subsystem's own), and a
`wiki-note` if the wiki describes it. Dead ends get one line each too: they stop the next agent repeating them.
