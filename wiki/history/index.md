# History

This section is the dated record of SAT LIGHT SIM: versions, decisions, alternatives that were tried and
dropped, bugs whose lessons shaped the code, and problems still open. It is the only part of the wiki where
dates, review numbers and "it used to be..." are allowed.

## What lives here, and what doesn't

Every other page describes the system **as it is now**, in the present tense, as if it had always been that
way (see the [style guide](../development/style-guide.md)). A main page may explain *why* the design is
right ("the depth buffer is R32F because a half float tops out at 65 504"); the story of *how that was
learnt* (the bug, the date, the reversal) belongs here.

| Page | What it holds |
|---|---|
| [Changelog](changelog.md) | the user-facing release notes, included verbatim from `CHANGELOG.md` |
| [Design notes](design-notes.md) | why the design is the way it is: dated decisions, alternatives dropped, measured before/after, bugs that became invariants, by subsystem |
| [Known issues](known-issues.md) | open problems, suspected bugs and documentation drift, each with the date it was recorded |

This section is not a commit log. `git log` has every change; these pages keep the ones a future developer
would want to know about before touching the code.

## How entries arrive

Entries are added during **wiki passes**, not alongside code changes (see
[Working on this wiki](../development/wiki.md)). During development a contributor writes a short note in
`wiki/_inbox/`; a note may end with an optional `## History` part. At the next pass:

1. The note's present-tense part is folded into the main pages it names.
2. Its `## History` part becomes a dated bullet in [Design notes](design-notes.md), under the subsystem
   heading of the page it concerns, in chronological order (oldest first), linking that page.
3. An open problem goes to [Known issues](known-issues.md); when a pass finds a known issue resolved, it
   moves the item to Design notes as a dated entry and deletes it from Known issues.
4. Drift between a source note (`CLAUDE.md`, a plan) and the code goes to Known issues, "Documentation
   drift", until the source is corrected.

An entry is one to three sentences: the date, what was decided or learnt, and the reason. Measured numbers
are welcome; long narratives are not.

## Versions

| Version | Date | Git tag | Notes |
|---|---|---|---|
| 1.0 | 2026-04-18 | `v1_0` | first release: the satellite flare visualizer, Reflect Orbital mirrors, Moon, cinematic mode |
| 1.1.0 | 2026-08-15 | `v1.1.0` | terrain, volumetric clouds (v1), aurora, airglow, planets, controller support, the intro cinematic; no `CHANGELOG.md` section of its own |
| 1.1.1 | 2026-09-08 (tag 2026-09-10) | `v1.1.1` | zodiacal light, weak-hardware tiers (Potato / Planetarium), the 128-byte push-constant trim, cross-platform release CI |
| 1.2.0 | unreleased | none | the internal version in `VERSION`: geometry-model satellites and photometric benchmarking, satellite meshes, clouds v2, terrain detail, cities, the sea, the Moon as a body, sound, the automation harness |

Dates are the tag's commit date from `git log`; the 1.1.1 changelog heading gives the release day. The
project began on 2026-03-11 as a general shader playground ("Shader_Fun"); the satellite simulation started
on 2026-03-17 and became the only active simulation.
