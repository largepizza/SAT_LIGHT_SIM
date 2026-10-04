---
pages: [using/features.md, using/controls.md, using/graphics-settings.md, development/controls-and-settings.md, development/profiling.md, development/harness.md]
code: [src/simulations/SatelliteSimUI.cpp, src/simulations/SatelliteSimBookmarks.cpp, src/simulations/SatelliteSimHarness.cpp, tools/harness/scripts/ui_windows.satcmd]
---
## Now
**Windows share one UI kit** (SatelliteSimUI.cpp "UI kit"): sections are small caps over a 1-px rule, readouts a
label column and a value column, stat tiles a label over a large value, toggles/choices/buttons 20-px pills.
The **info window** shows four tiles under the 3D render (Magnitude, Elevation, Range, Phase), then collapsible
label/value sections: Satellite and Orbit open; Brightness (magnitude as seen, above the air, extinction, at
1000 km, phase angle; Trace pass), Sky position (visibility, elevation, azimuth, range), View (camera Free / From
you / Toward you, light Live / Studio, pose Sunlit / Rest, Spin, Shadows, Reflections, Surface detail, Glare,
Markers, Reset view) and Check (render, model, difference; Run check) collapsed. The **trace window** has two tile
rows — PASS (peak magnitude, its time, length, max elevation) and NOW (clock, magnitude, elevation, phase) — the
legend and axis titles on one line, the plot, and Retrace / Live / Export CSV.

**Settings**: the tab strip is grouped (GENERAL: Display, Controls, Sound; SKY: Constellations, Photometry;
RENDERING, behind "Show advanced settings": Clouds, Weather, Atmosphere, Terrain, Night lights, Ocean, Beams,
Performance; ABOUT: Attributions) and scrolls on its own. Display has sections Graphics (preset, render scale,
temporal AA, frame limiter, beam pointing rays), Window (mode, text scale, units), HQ photo, Startup (play intro,
Replay intro / tutorial), Settings (advanced, reset). The new **Performance** tab holds the GPU / CPU frame
breakdowns, Save snapshot, Run knockout sweep and the knockouts (Skip / On). The Camera tab is gone: Controls has
Movement, Bindings (Action / Keyboard / Gamepad columns), Mouse (look, zoom, select), Invert look, Help (controls
overlay). Sound, Constellations (column header) and Photometry (Brightness, Sky dimming, Point sources, Bloom and
glare, Dark sky, Star trails, Satellite models, Bulk export) are sectioned. Setting keys are unchanged.

**HUD text entry**: click the time bar's UTC clock, latitude, longitude or altitude to type. Time: "2036-11-22
02:06[:30]", ISO, or "HH:MM[:SS]" for the current date. Coordinates: decimal degrees with a sign or N/S/E/W, or
"lat, lon" in either field. Altitude: in the HUD's unit (km / mi) or with m/km/ft/mi, MSL or AGL by the toggle.
Unparsed text shows an amber toast and changes nothing, and the clock keeps the last valid time: the whole text
must match one form, the day must exist in its month (leap years included), and the year must be 1800-2200.

**HUD layout**: the bottom-left panel is the clock, the time buttons and the picture buttons (Screenshot, HQ
photo, Star trails); the bottom-right panel (position, altitude, fps, version) ends in the menu buttons —
Bookmarks, Cinematics, Settings — each lit while its window is open. The tutorial's last step is "Menus".
**Icons** are solid white silhouettes with hard pixel edges (`tools/make_icons.py`, `PIXEL_STYLE`: a 24-px grid
doubled to 48); Cinematics is a clapperboard, HQ photo a solid camera with a sparkle, Bookmarks a solid ribbon
with a star cut out, and the 3D view's chips are a thick spin arrow, a telescope (Observer), a solid bulb
(Studio) and four corner brackets (Maximize).

**Bookmarks** (bookmark button in the time bar): "Add current view" saves the place, camera, sim time and the
cloud map's drift with a 256x144 thumbnail of the next clean frame; cards show the thumbnail (click = go there),
an editable name, when / where, Go / Update / Delete (two clicks). Up to 112. Stored in
`<user data>/bookmarks/bookmarks.json` (`sat-light-sim-bookmarks/1`) + `<id>.png`. Harness: `bookmark add [name] |
go <n> | update <n> | rename <n> <name> | delete <n> | list`, `ui open bookmarks`, `expect state.<path>`.

## History
The windows were lines of prose ("Mag 2.31 as you see it (ext 0.12)", hint lines, "(selected)" in titles,
"retrace 1.2 ms"); the settings tabs each had their own row style and Display mixed user options with profiling.
The HUD altitude readout was wrong above the ground: it showed terrain + offset as MSL and the offset as AGL, but
the offset is already above sea level (floored at the ground) — now MSL = max(ground, offset), AGL = max(0,
offset - ground). The grouped strip first stretched short windows (its content spilled below the window) until it
became its own scroll view. The typed time first checked the day only against 31, so "2036-11-31" rolled over
to December 1 and was kept. Cinematics and Bookmarks sat in the time bar at first; the film, HQ photo and bookmark
icons and the 3D view's chips were thin outlines, unlike the filled gear and camera.
