---
pages: [development/ui.md, development/controls-and-settings.md, using/index.md]
code: [src/UiText.h, src/UiText.cpp, src/UiTextClay.h, src/text/ui_text_en.inc, tools/ui_text_check.py, tools/ui_text_migrate.py, docs/UI_TEXT.md, src/simulations/SatelliteSim.cpp, src/simulations/SatelliteSimHarness.cpp]
---
## Now
The text the player reads is kept in one table, `src/text/ui_text_en.inc`: one `UI_TEXT(Id, "text")` line per
string, read in the code with `tx(Txt::Id)` (or `txc(Txt::Id)` for a Clay string). About 1,750 strings are in it:
window titles, buttons, tooltips, settings labels and notes, section headings, toasts, the tutorial, the boot chooser,
the loading screen's steps, and the Missions, Weather, Sky events, Kessler tracker, Issue reports, Bookmarks and
Cinematics windows. Formatted strings keep their printf placeholders in the table. There is one language, English; a
translation would be a second table with the same ids. Settings keys, harness messages, log lines, keybinding action
names (persisted in settings.json) and data names stay in the code. The rules for new text are in docs/UI_TEXT.md;
`tools/ui_text_check.py` checks the table against the code (unknown or unused ids, non-ASCII text, and the argument
count of every format at its call site).

During Go to, Track (the T key and the selection panel's button) is Go to's own aim lock: it toggles "Aim: locked /
free", and the button is lit while the camera keeps the object centred.

Loading a recorded view (a bookmark's Go, an issue shot's Go, harness `report load` and `snapshot`) ends the lock to
the stars.

The Weather window's featured cards use shorter lines (a hurricane card reads "Sara, Category 4", its wind, then its
place), so they fit at 1024 px.

## History
Until 2026-10-29 the strings were literals at their call sites, several settings tooltips named settings.json keys,
and two settings pages carried notes pointing to where controls had moved (the Constellations tab's "Launches and the
Sun's Fury" pointer, the Launches tab's note about the Missions window). The text was reviewed in the same pass:
jargon and implementation detail removed from player-facing tooltips, one name per thing (Go to, Track, Watch). T did
nothing during Go to (Track's lock was dropped at once while following).
