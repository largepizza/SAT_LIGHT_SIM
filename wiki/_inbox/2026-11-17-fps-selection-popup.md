---
pages: [development/controls-and-settings.md, development/harness.md]
code: [src/simulations/SatelliteSimFps.cpp, src/simulations/SatelliteSimUI.cpp, src/simulations/SatelliteSim.cpp]
---
## Now
In FPS mode the screen centre is the pointer of the selection popup (the panel by the selected satellite with
the Info / Go to / Trace / Track buttons), and of nothing else: the toolbars, windows and other HUD stay inert.
The popup is anchored to the satellite on screen (18 px right, 8 px up of it), so the player turns the view to put
the centre on a button; that button hovers (colour, sound, tooltip at the centre) and a left click, the pad's A or
"Select Satellite" presses it instead of picking. A click on the popup between its buttons does nothing; off the
popup the click picks as before. Info and Trace open a window (menu mode); Go to and Track keep FPS mode. While
the popup is drawn in FPS mode a steady 4 px dot (6 px at UI scale 1.5, `kFpsDotDiamPx`) marks the centre, never
in clean shots. While Track holds, the look is locked, so the popup cannot be aimed at (T untracks).
Harness: `mouse aim <ElementId>[:index]` (FPS mouse moves onto an element), `fps state` `centre_popup` /
`centre_dot`, `mouse click` `popup`; checked in `tools/harness/scripts/fps_mode.satcmd` section 10.

## History
Asked for by the owner (2026-11-17): the actions are "often the next thing after clicking a satellite ... the
tooltip is right there and center". Before, FPS mode left the whole HUD inert and the actions needed Tab first.
