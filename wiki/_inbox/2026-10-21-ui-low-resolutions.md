---
pages: [development/ui.md, using/graphics-settings.md, development/harness.md]
code: [src/simulations/SatelliteSim.h, src/simulations/SatelliteSimUI.cpp, src/UIRenderer.cpp, tools/harness/ui_lowres.py]
---
## Now
The interface scales with the window's height: the effective UI scale is "Text scale" (`display.ui_scale`, 1.5)
times clamp(height / 1000, 0.7, 1) while "Scale with window" (`display.ui_auto_scale`, Display tab, default on) is
on, so nothing changes at 1000 px and taller (a maximized window on a 1080p desktop). The automatic factor never takes text below 16 px (the baked font's
floor). No window is ever larger than the screen: sizes and minimums are capped at the screen less 8 px each frame and
windows are kept on screen after a resolution change. Default window sizes are the 1080p size where it fits, else a
share of the screen (Missions at most 42% x 70%, docked right; Issue reports 60% x 72%; Bookmarks 55% x 66%;
Cinematics 45% wide; Info a third of the width, at least 350 px). The right HUD panel drops its FPS and version
label when it would reach the left one (1024 px wide). `tools/harness/ui_lowres.py run` checks every window and the
HUD at 1024x768, 1280x720, 1366x768, 1600x900 and 1920x1009.

## History
User report 2026-10-21: "UI scaling needs work for low resolutions, the new missions window for example occupies the
entire screen basically". Before: the UI scale was 1.5 at every size, window sizes were fixed pixel values (several
kept their last position from a larger screen), the HUD panels overlapped at 1024 px and a long tooltip could start
off the left edge.
