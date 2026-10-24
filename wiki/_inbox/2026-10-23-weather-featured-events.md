---
pages: [new: using/weather.md, using/features.md]
code: [src/simulations/SatelliteSimWeatherFeatured.cpp, src/simulations/SatelliteSimWeatherWindow.cpp, tools/make_icons.py]
---
## Now
The Weather window's Storms tab opens with EVENTS: one card per kind of event, always in the same order and the same
size — the next tornado (daylight preferred, up to 24 h ahead), the strongest hurricane / typhoon now and the next one to
become a major one, the next haboob (an organised storm over dusty ground by day, up to 48 h ahead), the strongest
supercell, the strongest squall line / MCS, the next waterspout, and the biggest wildfire burning. Each card has an icon,
three lines and buttons to go there (Watch / Close for a tornado, Eye / Orbit / Landfall / Next major for a hurricane, Watch
/ Aloft for a haboob, Go to / Watch for storms, Go to / Orbit for a fire). Below it, NEAR YOU shows four fixed slots of
organised storms near the observer; a storm keeps its slot while it stays near. Upcoming tornadoes, waterspouts and
haboobs are found by searching ahead in sim time on a second copy of the storm catalog, a little each frame while the
window is open, so the window never costs a frame hitch and the renderer's storms are not disturbed.

## History
The tab used to list the storms near you (nearest first) and the strongest worldwide: rows appeared, vanished and changed
height as storms came and went, and the window's content jumped about (user QA, 2026-10-23).
