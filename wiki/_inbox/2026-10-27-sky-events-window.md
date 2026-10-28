---
pages: [new: using/sky-events.md, using/controls.md, using/features.md, simulation/sun-moon-planets.md, development/harness.md]
code: [src/simulations/SatelliteSimSkyEvents.cpp, src/simulations/SkyEvents.h, src/simulations/SatelliteSim.cpp (skyPlanetGeoAt)]
---
## Now
The Sky events window (crescent-Moon button beside Weather on the right HUD panel, key K, harness `ui open skyevents` /
`skyevents state|events|list <kind>|watch <kind> [n=]|watch <row>`) lists celestial events: solar and lunar eclipses,
conjunctions (two of Mercury .. Saturn within 2.5 deg, or the Moon and a planet brighter than mag 1.5 within 1.5 deg, both
>= 12 deg from the Sun) and lunar occultations of Mercury .. Saturn. One fixed-height card per kind (its next event) and
10 fixed UPCOMING rows (the next events of every kind within 2 years, by time). Each has ONE Watch: the clock is set
inside the event (never before it starts; an occultation one minute before the planet reappears at the drawn Moon's
limb), at 1x, unpaused; the observer stands where it is seen (dark sky for conjunctions, occultations and lunar eclipses,
Sun >= 15 deg up for a solar one; a clear sky on the CPU cloud map preferred), the camera is aimed (FOV solar 16, lunar
12, conjunction 4 x the separation, occultation 5 deg) and kept on the bodies until the player moves it; the planet is
selected and a toast says what you see. The searches run off the main thread (~150 ms in all over 2 years) and restart
when sim time jumps more than 30 days or goes back. Each kind is one row of a table (a search, a Watch, an icon, the card
text): the planned Moon-base array glints, the 2037-12-25 Betelgeuse supernova and meteor showers are added as rows.
Events use the sim's own positions (the Moon of date, the planets J2000), so they are the ones the sim draws.

## History
First cut judged an occultation's place at the central moment; the drawn Moon (2.5 x true) makes an occultation 2-3 h
long at one place, and the Watch landed with the Moon on the horizon. Places are now judged at the Watch moment.
Measured: the open window costs ~0.07 ms a frame of build_ui; a Watch ~4 ms on its frame.
