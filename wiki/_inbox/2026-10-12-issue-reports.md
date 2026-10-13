---
pages: [using/controls.md, development/harness.md, "new: development/issue-reports.md"]
code: [src/simulations/SatelliteSimReports.cpp, src/simulations/SatelliteSimHarness.cpp]
---
## Now
A snapshot is a SHOT in an ISSUE REPORT. An issue is one self-contained folder, `<user data>/reports/<id>/`:
`report.json` (format `sat-light-sim-report/1`: title, description, category, status, shots) and one
full-resolution `shot_NN.png` per shot. Save Snapshot (F9) adds a shot to the active issue; New Issue Snapshot
(F7, `KB_REPORT_NEW`) starts another. After a shot a bar opens at the top with the issue's title and the shot's
one-line note, the note focused: Enter saves, Esc closes, and either key takes the next shot with it open.

A shot holds the perf snapshot record (view, cloud drift, full settings, timings), the harness state, a `capture`
block (cloud / sky-TAA history valid, live render scale, auto-exposure offset, the eye's speed and turn rate),
frame-time statistics over the last 240 frames, the camera's last 12 s at 30 Hz (`trail`), and the settings that
changed since the issue's previous shot. Its image is the frame presented right after the key press, UI left
out; nothing is paused, settled or reset for it. Each shot also appends its record to
`perf_profiles/profile_log.jsonl` with a `report` {id, shot} key.

Harness: `report list [dir]`, `report info <issue>`, `report load <issue> [shot=] [settings=on|off] [clamp=on]
[trail=on|off]` (stands in the shot's view, time paused, and makes the trail the current camera path for
`path play`), `report shot [note] [new=1] [title=] [bar=1]`. `report load` and `snapshot` apply settings as
saved, past `applySettingsJson`'s range guards (`settingClamp`); `clamp=on` restores them.

## History
Snapshots were bare lines in profile_log.jsonl whose meaning lived in the chat they were handed over in; several
shots of one problem had no link, and no image recorded what the reporter saw. Settings replay used to clamp a
few keys on load, which changed values the user had typed past their range in dev mode.

## Now (step 2: the window)
The flag button on the right HUD panel opens "Issue reports": a card per issue (thumbnail, title, shot count,
category, status; Open / Make active / Delete), and per issue its title, category (clouds, terrain, sea, sky,
lighting, satellites, performance, ui, other), status (open, sent, fixed, closed), a multi-line description, the
selected shot's frame and the shots (note, Go, Go + settings, Delete). A click on the frame sets that shot's pin
(`pin` {x, y}, 0..1 from the top left): where in the frame the problem is. Go restores the place, camera, moment
(paused) and clouds; Go + settings also switches to the shot's settings. Each shot has a `shot_NN_thumb.png`
beside its PNG. "Open folder" opens the issue's folder, which is what gets zipped and sent.

## Now (step 3: sending and resolving)
"Export zip" in an issue writes `<user data>/report_exports/<id>.zip`: the issue's folder plus `log_tail.txt`
(the end of the session log, with the install and user folders redacted), and marks an open issue Sent. That file
is what a player sends. On the development side `tools/harness/reports.py` lists and shows issues, prints a brief
for whoever takes one, imports a received zip, and writes a resolution back (`resolve`: text, commit, images,
status fixed). The window shows the resolution, lists fixed issues in green, and the reporter closes or reopens
them after looking (Refresh re-reads the files).
