---
pages: [development/controls-and-settings.md, development/harness.md]
code: [src/simulations/SatelliteSimFps.cpp, src/simulations/SatelliteSimUI.cpp, src/simulations/SatelliteSim.cpp, src/App.cpp]
---
## Now
Two mouse modes. Menu mode: free cursor, hold the right button and drag to look. FPS mode (Tab, the "Toggle
Cursor" binding on the keyboard; the pad's Start is still the virtual cursor): the OS cursor is hidden and captured
(GLFW raw motion when supported), the mouse looks with no button, a left click picks at the window centre (shared
with the pad's "Select Satellite": window coordinates, not the framebuffer's) and flashes a ring there (a lone click 0.6 s; a
click while it shows adds 1 s, capped at 4 s; then a 0.3 s fade), and the HUD stays drawn but receives no pointer. Tab or a right click
(which never starts a right-drag) leave it; so do any window, the note bar or the first-run tutorial card appearing,
the console or a text entry, and losing window focus. Setting `camera.fps_mode`, default on (absent = on). Not live during
the intro, a playing cinematic or an HQ photo (menu mode meanwhile); live during the player's own video recording. "Toggle UI" moved from Tab to
U (key layout 4 migrates a saved Tab). New "Mouse sensitivity" slider (Controls > MOUSE, `controls.mouse_sensitivity`,
0.1-4, default 1) multiplies the 0.12 deg/px look rate in both modes; the pad's look rate is unaffected. Harness:
`fps on|off|state`, `mouse move dx= dy= [frames=]`, `mouse click left|right`; a harness run starts in menu mode, never
captures the OS cursor, and a loaded settings file never turns the mode on.

## History
Before 2026-11-16 the only look was the right-drag, and `KB_TOGGLE_CURSOR` had no keyboard meaning. The centre pick
used `swapExtent` (framebuffer pixels) while the mouse pick used window coordinates.
