---
name: qa-merge
description: Merge verified QA clusters into main in order, re-run the gates on the merged tree, write the resolutions back into the issue reports with reports.py resolve, and pitch the wave to the user. Use when fix agents report clusters verified, or when the user says merge the wave.
---

# qa-merge

Read `AGENTS.md`, `docs/QA_LOOP.md` and the wave's `plan.json`. **You are the only agent that writes to `main`
or to the issue reports.**

## 1. Preconditions

Merge only clusters whose `state` is `verified` and whose criteria all passed with evidence. If a cluster says
`fixed` but a criterion failed, send it back — do not merge it hopefully.

## 2. Merge in blast-radius order

data/docs-only → UI → subsystem → core renderer. Per cluster, into the **integration branch** (`plan.json`'s
`base` — the branch the user runs; `main` is stale):

```bash
git -C .worktrees/<slug> rebase <base>
cmake --build .worktrees/<slug>/build --config Release --target SatLightSim
# re-run the cluster's criteria in the worktree after the rebase
git merge --no-ff qa/<date>/<slug>
```

A rebase that breaks the build is a cluster to send back, not something to fix inside the merge.

## 3. Re-gate the merged tree (main checkout)

Per-cluster gates do not prove the wave:

```bash
cmake --build build --config Release --target SatLightSim
python tools/harness/selftest.py                                        # harness health
python tools/harness/run.py -c "report load \"<issue dir>\" shot=1; wait 30; capture verify" --out harness_runs/qa-<cluster>-merged
cmake --build build --target accuracy-gate                              # if photometry moved
python tools/harness/ui_lowres.py run                                   # if a window moved
python tools/harness/hitch_check.py                                     # if perf moved
```

A failure is `git revert -m 1 <merge>`, the cluster back to `fixed`, and the reason recorded. Never resolve an
issue on a tree that fails its gate.

## 4. Resolve — this is the pitch

Per issue, on `main`, with the merge commit:

```bash
python tools/harness/reports.py resolve <id> --text "<root cause; what changed; what is still open>" \
  --commit <merge commit> --image <before.png> <after.png> --by "<owner>"
```

Text is ASCII (the app's font renders nothing else). Name the setting or file the reporter needs to see it.
Issues in a cluster that are not identical get their own text. Anything not actually fixed:
`--status open` and the honest reason.

## 5. `pitch.md`

`qa/waves/<date>/pitch.md` — one page:

- header: date, clusters merged, issues resolved, issues sent back;
- table: cluster | issues | one-line fix | evidence folder;
- known-bad / still open, with why;
- **what to QA next**: for each cluster, the shot to re-open (Go), the time scale or settings to try, and what
  "still slightly off" looks like. That is what makes the next round cheap.

Then one short message to the user with the same core: what changed, what to look at, what you could not fix.

## 6. Reap

```bash
git worktree remove .worktrees/<slug>
git branch -d qa/<date>/<slug>
```

Keep `.claude/worktree_rescue/` — those are rescued patches from earlier rounds, not this wave's debris. Set
`plan.json` state to `resolved`, append to `qa/ledger.jsonl`, and keep the wave folder: the next audit reads it.

## Never

- merge without re-running the criteria on the rebased tree;
- resolve with a worktree hash instead of the merge commit;
- leave merged branches or worktrees behind (the repo already has ~50 and ~190);
- pitch a cluster as fixed while one of its criteria failed.
