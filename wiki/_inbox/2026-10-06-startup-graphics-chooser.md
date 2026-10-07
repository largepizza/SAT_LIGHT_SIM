---
pages: [using/graphics-settings.md, development/harness.md]
code: [src/App.cpp, src/simulations/SatelliteSimBootChooser.cpp, src/simulations/SatelliteSim.cpp, tools/harness/run.py]
---
## Now
Before anything heavy loads (textures, satellite models, the simulation's pipelines), the loading screen asks for a
graphics mode: **Full graphics**, **Planetarium** (no clouds) or a smaller **Potato (very old hardware)** option.
It pre-selects the last session's mode, or for a first run the device's recommendation (integrated / CPU / virtual
GPU -> Planetarium, a discrete GPU -> Full); after a session that did not close cleanly it pre-selects one tier
lighter and says why. Mouse, keyboard (arrows, Enter) and gamepad (d-pad, A); it never continues on its own.
"Full graphics" keeps a full preset already chosen, else the device's (Medium; Low on an integrated GPU).
Setting: "Ask for graphics mode on startup" (Display tab, STARTUP; also the chooser's "Ask on every startup" box),
`display.ask_graphics_mode`, default on. On Planetarium / Potato the full sky shader's pipelines are not built at
startup or on a resize; they are built the first time a full preset or a render scale below 100% needs them.
Safety net: if the first frames after loading take more than 250 ms each (median of 10 frames, or of at least 3 once they add up to 5 s, after ~2 s of warm-up), the
app steps one tier down (Full -> Planetarium, Planetarium -> Potato) once and shows a banner.
Harness: skipped in a run; `run.py --boot-chooser N` scripts it (0 Full, 1 Planetarium, 2 Potato),
`--boot-safety-ms MS` arms the safety net; `scripts/boot_chooser.satcmd`.

## History
Before 2026-10-06 the first run seeded a preset from the device type and the intro benchmark adjusted it, but only
after everything (including the full sky shader for every tier) had loaded; a crash forced Planetarium on the next
launch without asking. The chooser's pick now replaces both for that launch.
