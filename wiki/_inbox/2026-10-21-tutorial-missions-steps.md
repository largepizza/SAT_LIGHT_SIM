---
pages: [using/getting-started.md, using/features.md, development/controls-and-settings.md, development/harness.md]
code: [src/simulations/SatelliteSimTutorial.cpp, src/simulations/SatelliteSimMissions.cpp, src/simulations/SatelliteSimUI.cpp, src/simulations/SatelliteSimHarness.cpp]
---
## Now
The first-run tutorial has 13 steps (a 14th, Graphics, only on Planetarium / Potato): look, move, climb, go faster,
pick a satellite, Satellite info (open the info window), Go to (ride along, then leave with Exit = fly home or Stay
here = stay floating up there), Trace and Track (either), time, pictures, Missions: what's next (open the Missions
window with the rocket button or M, then Go to / Watch a launch on Upcoming, or a Sun's Fury button), Missions:
under way (5 s on the Ongoing tab, or any of its buttons), and menus (Bookmarks, Cinematics, Issue reports,
Settings). Each step outlines only its own HUD control. Steps that need a selection offer "Pick one for me" (a
sunlit, high station or Starlink with a 3D model; the camera turns to it). After the Missions step moves the clock,
every later card offers "Back to <UTC>", which restores the time, place, camera, clock speed and cloud drift from
before the jump. The right HUD panel's menu buttons are Missions, Bookmarks, Cinematics, Issue reports, Settings
(the Issue reports flag sits immediately left of Settings). Harness: `tutorial pick`, `tutorial return`; step
names info / goto / trace / missions / ongoing.

## History
Until 2026-10-21 one "Your satellite" step covered Info, Go to, Trace and Track (any one finished it) and the
Missions window had no step; the Issue reports flag was the first menu button. The user asked for the Missions
tab to be taught explicitly and for Go to / Info to be forced one at a time.
