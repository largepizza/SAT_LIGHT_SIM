---
name: wiki-note
description: Record a wiki note for a change just made (behaviour, technique, file format, setting, default, accuracy number) into wiki/_inbox/ for the next wiki pass. Use at the end of any feature or fix that changes what a wiki page describes, or when the user says "note this for the wiki".
---

# wiki-note

Leave a short note in `wiki/_inbox/` so the next wiki pass can update the pages. **Do not edit wiki pages
here.** Pages are rewritten only in a wiki pass (the `wiki-pass` skill), so they stay literature, not logs.

## When

After a change that alters something the wiki describes or should describe: what a system does, how a
technique works, a file format or JSON field, a setting (label, key, default, range), a control, a measured
accuracy or performance number, a new tool or command. Skip pure refactors, and fixes that only make the
code match what the wiki already says.

If unsure which pages it touches, look at the nav in `mkdocs.yml` and grep `wiki/` for the subsystem.

## How

1. Check `wiki/_inbox/` for an existing note on the same change from this session. Update that note rather
   than adding a second one.
2. Write `wiki/_inbox/<YYYY-MM-DD>-<short-slug>.md` (today's date):

```markdown
---
pages: [rendering/sea.md]          # wiki pages to update; "new: rendering/foo.md" proposes a new page
code: [shaders/sat_sky.frag]       # where the truth lives
---
## Now
<2-6 sentences, PRESENT TENSE, describing the system as it is after the change: what it does, the key
numbers, settings (UI label, settings.json key, default). Raw material for the page, so be precise.>

## History
<optional: what it replaced and why, a bug found, an alternative tried and dropped, measured before/after.>
```

3. Tell the user in one line that the note was added and which pages it names.

Keep it short. A note is a pointer plus the facts that are hard to recover from the code: the reasons and
the measurements.
