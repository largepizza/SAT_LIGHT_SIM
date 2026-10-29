---
pages: [using/graphics-settings.md, rendering/hardware-tiers.md, using/weather.md]
code: [src/simulations/SatelliteSim.h, src/simulations/SatelliteSimWeatherWindow.cpp, src/simulations/SatelliteSimBookmarks.cpp, src/simulations/SatelliteSimSkyEvents.cpp, src/simulations/SatelliteSimMissions.cpp, src/simulations/SatelliteSim.cpp, shaders/sky_taa.comp]
---
## Now
Planetarium and Potato offer only views they can draw. Without the volumetric clouds the Weather window shows a
short notice instead of its tabs and runs nothing; its HUD button and its Settings tab are dimmed. Watches and
bookmark Go buttons that would land on nothing are disabled, with the reason as the tooltip: the Rainbow, Beam-lit
Snow and Typhoon bookmarks need the volumetric clouds; Solar Storm, The Sun's Fury bookmark and the Sun's Fury Watch
in the Missions window need the aurora; on Potato the solar and lunar eclipse Watches and the two eclipse bookmarks
are off, because the Potato sky draws neither the Moon over the Sun nor the Earth's shadow on the Moon. Everything
else lands on something visible: comets with their tails, conjunctions, occultations, Moon-base glints, meteor heads,
the supernova, launches, re-entries and Starmind.

Potato draws the point sources of events: rockets, re-entry fireballs, meteor heads, comet heads, Moon-base glints and
the supernova (it lists them every frame, as the other presets do). It does not draw plumes, streaks or dust.

The background's temporal upscaling (render scale below 100%: Low 50%, Medium 67%) weighs this frame's nearest
sample by a kernel the width of an input pixel while the view moves, so in motion the image follows at once instead
of lagging; a still view accumulates the jittered samples as before.

Preset cost (RTX 3070 Ti, GPU ms at 1920x1009): Potato 1-3, Planetarium 2-5, Low 4-12, Medium 5-19, High 8-29.

## History
2026-10-29 (workstream C). Before, the Weather window, its storm Watches and these bookmarks opened on an empty sky on
Planetarium and Potato, and Potato skipped the per-frame list of event point sources with the cloud pass, so launch,
meteor, comet and re-entry Watches showed nothing there and Betelgeuse went dark at its supernova. The upscaler took
the nearest sample at a 0.6-output-pixel kernel in motion too: at 50% most pixels moved by 6-25% of the moving weight,
so Low lagged and ghosted while turning.
