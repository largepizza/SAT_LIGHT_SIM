---
pages: [using/settings.md, development/ui.md]
code: [src/simulations/SatelliteSimUI.cpp, src/simulations/SatelliteSim.h, src/simulations/SatelliteSimWeatherWindow.cpp, tools/harness/ui_lowres.py]
---
## Now
Every setting that changes how the sim looks has a row in the Settings window. A search box sits at the top of the
tab strip: typing filters the rows of every tab as it is typed (case-insensitive; every word must appear in the row's
label, or in its section's title, which lists the whole section). Matches are listed under their tab's name (a link
that opens the tab) and section heading, and work in place (sliders, toggles, buttons). With the advanced tabs hidden
they are still searched and counted ("N more in the advanced tabs"), with a button that shows them. A tab button or
Clear ends the search. Rows built from the UI kit (toggle, choice, readout, button rows, the slider rows and
sections) register themselves; a hand-built row calls `searchRow(id, label)`, and hand-built text that is not a row
skips itself while `searchActive()`.

"Show advanced settings" is a card of its own at the top of the Display tab. The Sky tab (under SKY) holds the stars
(the naked-eye limit, its boost, the dark-sky extra stars, the Betelgeuse supernova and its variability), the planets
(with the disc brightness), the Moon (size, relief, shadows, the Moon-base glints), comets, meteors and refraction and
mirages. The Weather tab has the storm-objects switch, hurricane arm cirrus, storm surge, wildfires, smoke and haboobs;
Terrain the solar parks; Ocean the wave motion filter; Launches the plume dispersal; Photometry the star-trails switch
and the zoomed Milky Way switch; Display (advanced) the TAA new-frame weights. The Weather window opens on its Storms
tab, drawn first (Storms, Radar, Cyclones, Controls).

## History
2026-10-28 (issues 20261028_222345, _222628). Until then the storm objects, fires and haboobs were set only in the
Weather window's Controls tab, and comets, meteors, Moon bases, refraction, the stars' limits and several rendering
switches only in settings.json. The Weather window opened on its radar.
