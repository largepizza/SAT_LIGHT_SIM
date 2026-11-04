---
pages: [development/tools.md, development/harness.md]
code: [CLAUDE.md, AGENTS.md, docs/subsystems/, tools/outline.py, tools/doc_budget.py, tools/harness/res.py, tools/harness/hdoc.py, tools/harness/shader_variant.py, .claude/skills/harness/SKILL.md, .claude/skills/perf-ab/SKILL.md]
---

## Now

The agent-facing architecture reference lives in `docs/subsystems/` (one file per area: clouds, weather, terrain,
launches, kessler, satellites, orbits, sky-bodies, sky-events, render-pipeline, perf, ui, capture, sound, photometry,
architecture, build, harness-internals), mapped in `AGENTS.md` §4. `CLAUDE.md` is a 2 KB header that imports
`AGENTS.md`; the two load into every Claude Code request and must stay under 32 KB together
(`python tools/doc_budget.py`, exit 1 past it or when a subsystem file is missing from the map).
Agent tools: `tools/outline.py <file> [-g regex] [--deep]` prints a markdown / C++ / GLSL / Python file's sections or
functions with line ranges; `tools/harness/res.py [run|live|name part]` reads harness results without dumping them
(one line per command, `-g` dotted paths with fnmatch keys, `--keys` shapes, `--perf` / `--ab` tables grouping
`perf name=<view>_<variant>_r<k>` reps, `--base` for the change against another run); `tools/harness/hdoc.py <cmd>`
looks up docs/HARNESS.md rows and sections; `tools/harness/shader_variant.py <slot> --tag t -s "OLD=>NEW"` builds a
text-substituted shader variant (each substitution must match exactly once across the shader and its headers,
compiled with the build's own glslc flags; an unmodified variant is byte-identical to the build's SPV) and prints
its `shaders reload` line. Claude Code skills: `harness`, `perf-ab`, and `qa-triage` / `qa-fix` / `qa-merge` shims
onto `.dsh/skills/`.

## History

Until 2026-11-03 `CLAUDE.md` held the whole reference (1.15 MB, ~290k tokens) and Claude Code sent it with every
request: the project's transcripts showed ~38,000 requests and ~20.6 billion cache-read tokens, a subagent starting
at ~378k tokens of context. It was moved verbatim (a split script checked every non-blank line arrived in order);
the always-loaded docs are now ~15 KB. `tools/perf_analysis/live_perf_table.py` (GPU total + cloud_march vs a
baseline) is covered by `res.py --perf --base`.
