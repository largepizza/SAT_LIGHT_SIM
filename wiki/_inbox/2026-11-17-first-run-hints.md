---
pages: [development/ui.md, using/controls.md, using/features.md, development/controls-and-settings.md, development/harness.md]
code: [src/simulations/SatelliteSimHints.cpp, src/simulations/SatelliteSim.h (HintGates, HintRun, kHint* constants), src/simulations/SatelliteSim.cpp (movement block signals, updateIntro's hintsReset, finishIntro), src/simulations/SatelliteSimHarness.cpp (hints), docs/subsystems/ui.md "Subsystem: Hints"]
---
## Now
The first run teaches the controls with one small hint at a time (`HintPopup`: keycaps from the live bindings, then
one line), chosen from what the player has not done: 1 look, 2 move (W A S D), 3 rise / descend (Q / E), 4 go
faster (Shift), 5 zoom (the wheel), 6 the cursor (Tab or right click), 7 click a satellite, 9 the time controls and
10 the menu buttons (each outlined on its toolbar), 8 Tab to look around again; pad players see the stick and button
labels, and Start (the virtual cursor) is their menu mode. Gates are sticky facts measured on the merged input
(keys, pad, harness `inertial move`): 30 deg looked, 1 s moved, 100 m climbed by Q / E, boost while moving, a zoom,
a selection, the time bar or a menu window used. Rules: nothing before the opening's third caption (15.22 s), one
hint at a time 3 s apart, 0.3 s fades, gone 2 s after its gate (measured 2.30-2.35 s at 20 steps / s), never shown
when its gate came first, ignored 20 s -> rests 30 s, two offers; 6, 7 (after the captions) and the final 2 never
time out; two FPS clicks within 5 s bring 6 at once. The pass completes (and saves `display.tutorial_done`) when the
captions have ended, a satellite is selected, the player has moved and 9 and 10 are done; 3, 4, 5 and 8 may then
fire once more. A pass runs on a genuine first run and after every Replay Intro; never over a cinematic, an HQ
photo, the old card or a hidden HUD. Above the caption band while one shows (attached to `CineCaption`), else bottom
centre clear of the toolbars. Harness: `hints on|off|reset|state|set|force`, `mouse scroll dy=`, `state.hints`
(`seen`, `log`), `tools/harness/scripts/hints_players.satcmd`.

## History
Replaces the 2026-10-03 card tutorial's teaching (14 cards after the intro, forcing menu mode): that card no longer
starts on its own (Settings > Display > Replay Tutorial still shows it; a later step deletes it). The "two quick
clicks take the slot for 6" rule was added because hint 2 (shown at beat 2 to anyone who has looked) would
otherwise hold the slot while a clicker looks for the cursor.
