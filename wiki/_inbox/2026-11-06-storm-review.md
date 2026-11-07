---
pages: [tools/harness.md]
code: [tools/harness/storm_review.py, tools/harness/storm_review_checklist.md]
---
## Now
`tools/harness/storm_review.py` reviews each weather object (a cumulus field, the lab supercell and squall line, a
hurricane) the same way every time: 8 bearings x 3 heights looking at it, from above and from under the core, at several
ages, a still 1x flicker run and per-frame motion runs at its time scale. It writes contact sheets, mp4s and numbers
(the frame-to-frame change normalised for lighting; a spike is a jump) and a checklist built from the user's reports for
a reviewer who was not told what changed.

## History
Before 2026-11-06 storm changes were judged from a few settled stills; square columns, straight cuts, a frozen hurricane
and a flicker over a quarter of the sky all passed that way.
