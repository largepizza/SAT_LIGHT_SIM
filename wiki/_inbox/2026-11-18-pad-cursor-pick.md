---
pages: [using/controls.md, development/controls-and-settings.md, development/harness.md]
code: [src/simulations/SatelliteSimUI.cpp (buildUI, the left-click pick), src/simulations/SatelliteSimHarness.cpp (pad), src/simulations/SatelliteSimFps.cpp (harnessMouse)]
---
## Now
A gamepad player's menu mode is the virtual cursor (Start), with the FPS mode left on. With the cursor up, A clicks
where it points in either FPS mode: on a satellite it selects it, as a mouse click in menu mode does; without the
cursor, A selects at the screen centre (the crosshair). The first-run hint "Click a satellite" is met by any
selection: the FPS click, a menu-mode mouse click, the pad's A at the centre or at the cursor, a window or list.
Harness: `mouse click left x= y=` clicks with the scripted pointer at a frame pixel; with no pad connected `pad start`
keeps the virtual cursor and `pad a [x= y=]` clicks at it.

## History
Until 2026-11-18 the FPS mode's click branch swallowed the virtual cursor's A: a pad player in menu mode could not
select a satellite by pointing at it, so the "Click a satellite" hint passed only by A at the centre (the user's
report: "only satisfied when the selection is made in FPS mode"). A menu-mode mouse click always met it.
