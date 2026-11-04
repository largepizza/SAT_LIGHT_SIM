# CLAUDE.md

Claude Code loads this file, and everything it imports, into **every request of every session and every
subagent**. Until 2026-11-03 it held the whole architecture reference: 1.15 MB, ~290k tokens re-read on each
of ~38,000 recorded requests (~20 billion cache-read tokens). The reference now lives in
`docs/subsystems/` and is read on demand. Keep what loads here small:

- **CLAUDE.md + AGENTS.md together stay under 32 KB.** Check with `python tools/doc_budget.py` after editing
  either (it fails past the budget, or when a `docs/subsystems/` file is missing from the AGENTS.md map).
- **Subsystem notes go in `docs/subsystems/<area>.md`** (map: AGENTS.md §4), next to the section for the code
  they describe — never here. A note is present-tense: what the code does, the invariant, why, the measurement.
- **Never read a subsystem file, a shader or a big .cpp whole.** `python tools/outline.py <file> [-g regex]`
  prints its sections / functions with line ranges (clouds.md is 200 KB, SatelliteSimLaunches.cpp 16k lines);
  read the range you need. Older text that cites `CLAUDE.md "<section>"` means that heading in
  `docs/subsystems/` (`grep -rn "<section>" docs/subsystems`).

@AGENTS.md

## Claude Code specifics

- **Skills** (`.claude/skills/`): `harness` (run and see the app cheaply), `perf-ab` (interleaved A/B timing in one
  live app), `qa-triage` / `qa-fix` / `qa-merge` (the issue-report loop; shims onto `.dsh/skills/`), `wiki-note`,
  `wiki-pass`. Load the skill before doing that kind of work; they carry the token-saving procedure.
- **Subagents re-load this file, AGENTS.md and the tool definitions** (~25k tokens each before any work). Give one
  a narrow brief with the file paths and line ranges it needs (from `outline.py`), not "read the clouds docs".
- **Harness output**: read results with `python tools/harness/res.py` (one line per command, `-g` paths,
  `--keys` shapes, `--perf` / `--ab` tables). Never `cat` results.jsonl or print a `perf` / `state` result whole
  (a `perf` result embeds a ~100 KB state). Look up commands with `python tools/harness/hdoc.py <cmd>` instead
  of reading docs/HARNESS.md. Look at captures as crops or contact sheets (`imgtools.py crop|sheet|stats`).
- Memory notes in the auto-memory folder may cite CLAUDE.md sections: they now live in `docs/subsystems/`.
