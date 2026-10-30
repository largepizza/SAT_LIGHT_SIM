---
pages: [using/graphics-settings.md, using/missions.md, using/features.md]
code: [src/simulations/SatelliteSimUI.cpp, src/simulations/SatelliteSimSkyEvents.cpp, src/simulations/SatelliteSimMissions.cpp]
---
## Now
Settings > Sky and Photometry fold their sections ("+" / "-" headers, folded on open, like the Clouds tab); a search
shows every match regardless. The explanatory notes under Advanced settings, Constellations, Sky, Launches and the
exhaust sliders are gone.

The Sun's Fury is a sky event ("SUN'S FURY", kind `cme`): its card in Sky events shows the flare / arrival times, then
the cascade's day, and Watch goes to the storm night. It is no longer a card in the Missions window. The Sky events
window has no "Lock view to the stars" button (the Movement popup's Reference frame is the control).

The HUD's screenshot and HQ photo buttons share one camera icon (the HQ one adds a star), and the left toolbar's
buttons use the right toolbar's look.
## History
2026-10-30 (issue 20261029_205641): UI wording and organisation pass.
