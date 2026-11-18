---
pages: [development/ui.md, using/controls.md, using/features.md, development/controls-and-settings.md, development/harness.md]
code: [src/simulations/SatelliteSimHints.cpp, src/simulations/SatelliteSim.h (kHint* constants, HintSaved), src/simulations/SatelliteSimUI.cpp (display.hints / hint_gates / hint_offers, the Display tab toggle), docs/subsystems/ui.md "Subsystem: Hints"]
---
## Now
The hint scheduler runs in every session, not only the first run's pass. The pass gains hint 11 (hold the right
mouse button and drag to look, shown the first time in menu mode, before "Click a satellite"; not for pad players)
and shows boost before rise / descend: once 1 s of movement is met, "Hold [Shift] to go faster" follows 1 s
(`kHintHandoffS`) after the move hint leaves, then "[Q] / [E] to rise and descend" once boost was used or its hint
had its turn. Contextual hints 12-19 stay armed until their gate is met or they have been offered twice, both kept in
settings (`display.hint_gates`, `display.hint_offers` by name): 12 "[Z] / [C] to roll" with the horizon free (above
the auto-level height), 13 "[G] to go to it" with a satellite selected, 14 the coronagraph after the Sun has been
within 10 deg of the view centre for 4 s (outlines the View button), 15 the star lock above 35,786 km (outlines the
Movement button), and one per silence between music tracks (or every 240 s with no audible music): 16 Sky events,
17 Missions, 18 Weather (each outlining its toolbar button), 19 "[F12] for a screenshot, [F8] for a high-resolution
photo" (outlines both buttons). They wait for the captions to end, any window to close and text entry to finish, and
give way to a pass hint. Settings > Display > Startup "Show hints" (`display.hints`, default on) turns all hints off;
Replay Intro teaches every hint again. Harness: `hints gap`, `hints set <contextual gate>`, `state.hints.saved`.

## History
From the user's review of the first round (2026-11-17): near a dark ground WASD "genuinely seems like you are going
nowhere until the Boost hint shows up", so boost moved ahead of Q / E (it used to wait for a 4-s movement streak);
the other contextual hints were their list (roll above auto-level, the right-drag on first Tab, the coronagraph,
the star lock past GEO, the windows and picture keys in the song gaps, Go to).
