---
pages: [using/controls.md, simulation/time-and-frames.md, using/graphics-settings.md]
code: [src/simulations/SatelliteSimHudPopups.cpp, src/simulations/SatelliteSimInertial.cpp, src/simulations/SatelliteSimUI.cpp, src/simulations/SatelliteSim.cpp]
---
## Now
The right HUD panel has two buttons, Movement and View, where the MSL / AGL toggle was. Each opens a small rounded popup
over its button; an unpinned popup closes on any click in the sim, a pinned one (the round pin button, top right) stays
and is dragged by its title. Pinned popups come back at the same place on the next launch.

Movement: Reference frame (Earth, or Stars = the inertial lock: the view is held on the stars and, from orbit height,
the eye is held in space too), Cinematic mode (the Alt pan-drift latched on, with gradual acceleration of WASD, Q / E
and zoom, and a look that coasts to a stop over ~4 s; Settings > Controls > Movement), Altitude gauge (MSL / AGL;
Settings > Display). View: Exposure (a fixed offset of -3..+3 stops with a Reset; the Photometry "Exposure (EV)" slider),
Coronagraph (the Sun's disc, glare and bloom hidden wherever you are, locked to the stars or not; Settings > Photometry),
Precipitation (the rain and snow particles round the eye; the clouds' rain shafts are unchanged; Settings > Weather).
A "Sun" reference frame is not offered: it would be the Earth frame turned once a year, and a view on the Sun is what
Track and the sky events' Watches give.

In the Stars frame with the eye held in space, WASD fly in the camera's own frame (forward along the view direction,
sideways along the screen's right) and Q / E change the altitude. Before, Q / E were overwritten by the held eye every
frame, and a sideways step orbited the Earth's centre about a degenerate azimuth when looking down.

## History
2026-10-29 (issue 20261029_180413). `inertialTick` adopted only a change of `obsDir`, so an altitude change was lost;
WASD stepped along `obsFacing` at constant radius, which at an orbit-height nadir view swings with every degree of look.
Now the tick adopts a height change and `inertialFly` moves the held eye in the camera's frame. The coronagraph
(`inertialOcculter`) no longer needs the lock.
