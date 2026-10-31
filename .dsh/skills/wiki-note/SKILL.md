---
name: wiki-note
description: Record a wiki note for a change just made (behaviour, technique, file format, setting, default, accuracy number) into wiki/_inbox/ for the next wiki pass. Use at the end of any feature or fix that changes what a wiki page describes, or when the user says "note this for the wiki".
---

# wiki-note (DeepSeek Harness shim)

The canonical procedure is `.claude/skills/wiki-note/SKILL.md` — read it and follow it. It is the same skill
Claude Code uses; this shim exists only so the DSH skill catalog can find it.

The rules that must not be lost:

1. **Do not edit `wiki/` pages from a fix.** Write one note in `wiki/_inbox/<YYYY-MM-DD>-<slug>.md`, with the
   front matter `pages:` (wiki pages to update) and `code:` (where the truth lives), then `## Now` — 2–6
   sentences of present-tense prose with the numbers, labels and defaults — and optionally `## History`.
2. Update an existing note for the same change from this session instead of adding a second one.
3. Say in one line which note you added and which pages it names. Pages are folded in only by a `wiki-pass`,
   which must pass `python tools/wiki/check.py`.
4. Skip pure refactors, and fixes that only make the code match what the wiki already says.
