---
name: qa-fix
description: Take one QA cluster from its brief to a verified fix in its own git worktree, then draft the resolution text and evidence for the merge step. Use when assigned a cluster (c01, c02 …), when the user says fix issue <id>, or when resuming a claimed cluster.
---

# qa-fix

Inputs: `qa/waves/<date>/briefs/<cluster>.md` and its `plan.json` entry. Read `AGENTS.md`,
`docs/QA_LOOP.md`, then the brief. One cluster, one branch, one worktree — never work on `main`.

## 1. Worktree

```bash
git worktree add .worktrees/<slug> -b qa/<date>/<slug> perf-frames
cd .worktrees/<slug>
cmake -B build -S .
cmake --build build --config Release --target SatLightSim
```

Branch from the **integration branch in `plan.json`** (`base`), which is the branch the user runs — at the time
of writing `perf-frames`. `main` is 817 commits behind it: a fix branched from `main` cannot apply to the
user's build.

## 2. Reproduce before changing anything

The brief carries the replay line from `reports.py show`. As one script:

```bash
python tools/harness/run.py -c "report load \"<issue dir>\" shot=1; wait 30; capture repro" --out harness_runs/qa-<cluster>-repro
```

For a shot whose `capture` line says MOVING, add `path goto 0` then `path play record=<name>`; a settled
capture hides motion problems, and a moving shot is often the whole point.

Then `read_image` the reporter's `shot_NN.png` (crop first if the brief names one) beside your capture. If
they do not match, find out why before going on: wrong build, a setting, time not paused, a different
resolution. Say which in the report back.

## 3. Fix

Minimal, in the mechanism the hypothesis names. Do not retune anything the report does not mention, and do not
"improve" neighbouring behaviour. If the real root cause differs from the hypothesis, correct the brief.

## 4. Verify against the criteria, in order

Every criterion in the brief must pass. Evidence into `qa/waves/<date>/evidence/<cluster>/`: before/after at
the shot's view, one settled capture, and the decisive run folder. Gates as the criteria say:

```bash
python tools/check_cloud_params.py                      # any cloud parameter change
cmake --build build --target accuracy-gate              # photometry
python tools/harness/tstab.py ...                       # motion / ghosting
python tools/harness/ui_lowres.py run                   # windows
python tools/harness/hitch_check.py                     # frame time
```

A failing gate is a failed cluster, not a note at the bottom.

## 5. Commit and document

- Message names the issue ids: `Fix anvil growth/decay: add rate sliders (issue_20261030_210137)`.
- `wiki-note` skill if behaviour, a setting, a default, or a measured number changed; `CHANGELOG.md` if it is
  user-visible.
- A harness command change updates `docs/HARNESS.md` **and** `kHelp` in `src/simulations/SatelliteSimHarness.cpp`.

## 6. Draft the resolution — do not apply it

`qa/waves/<date>/resolutions/<cluster>.md`: root cause in one sentence, what changed, evidence paths, what is
still open or known-bad, and per-issue text where the issues differ. ASCII only (the app's font).

**Do not run `reports.py resolve`.** The resolution's commit must exist on `main`, or the reporter is pointed at
a commit their build does not have. The merge step applies the draft.

## 7. Report back

Update the plan entry: `state: "fixed"`, `verification.result` (`pass`/`fail`), evidence paths, commit. Message
the orchestrator with cluster, commit, gates run, and anything the user should know — including "not
reproducible", with what you tried.

## Never

- resolve an issue you did not reproduce and verify;
- merge into `main` (qa-merge owns that);
- leave the worktree dirty or half-built when you hand off;
- hide a failed criterion in the wording of the resolution.
