---
name: wiki-pass
description: Run a wiki pass - fold the pending notes in wiki/_inbox/ into the wiki pages as present-tense literature, move history into the History section, delete processed notes, and pass tools/wiki/check.py. Use when the user asks for a wiki pass / wiki update / to process the wiki inbox, or before a release.
---

# wiki-pass

A dedicated, concentrated update of the wiki (`wiki/`, built by `mkdocs.yml`). Read
`wiki/development/style-guide.md` and `wiki/development/wiki.md` first. They are the contract.

## Steps

1. **Inventory.** `python tools/wiki/check.py --inbox` (use `tools/wiki/.venv/Scripts/python` if the venv
   exists; create it from `tools/wiki/requirements.txt` otherwise). Group the notes by page. If the user
   named a scope (a page or section), restrict to it, and leave notes outside the scope in place.
2. **Per page** (pages are independent, so parallel subagents work well with more than ~4 pages; give each
   the style guide, its notes and the page):
   - Read the page, every note naming it, and **the code the notes cite**. The code is the authority. A
     note can be wrong or outdated by later commits.
   - Rewrite the affected sections as finished prose: present tense, explain the design and its reasons,
     settings tables updated, invariants as `!!! warning "Invariant"`. Rewrite the section; never append a
     "Recent changes" paragraph. Remove anything that is no longer true. Re-read the whole page afterwards for
     contradictions it now has with itself.
   - `new: <path>` in a note: create the page and add it to the `nav` in `mkdocs.yml`.
3. **History.** Each note's `## History` goes to `wiki/history/design-notes.md` under the subsystem's
   heading, dated (the note's date), as one or two sentences, or to `wiki/history/known-issues.md` if it's
   an open problem. Drop trivia.
4. **Delete** the processed notes (never `_inbox/README.md`).
5. **Gate.** `python tools/wiki/check.py` must print OK: strict build (no broken links), no history-leak
   phrasing on main pages, every page in the nav. Fix, don't suppress. `<!-- history-ok -->` is only for
   real-world dates and the like.
6. **Report** to the user: pages rewritten, pages created, notes processed, anything in a note the code
   contradicted, anything left for a later pass.

## Also in a pass

If time allows, spot-check one or two pages the notes didn't touch against the code, since silent drift
accumulates. Record any drift found but not fixed in `history/known-issues.md`.
