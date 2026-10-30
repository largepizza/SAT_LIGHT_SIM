---
pages: [using/controls.md, development/controls-and-settings.md, development/harness.md]
code: [src/simulations/SatelliteSimUI.cpp, src/simulations/SatelliteSim.h, src/simulations/SatelliteSimInertial.cpp]
---
## Now
In cinematic mode (the Movement popup's switch) the mouse steers only while the look button (RMB) is held. Held, the
look rate follows the mouse's own rate smoothed over 0.12 s (at most 360 deg/s); on release the camera coasts on at
that smoothed rate, capped at 45 deg/s (a release slower than 0.5 deg/s does not coast), with no drag; holding the
button again with the mouse at rest brings the coast to a stop in about 0.3 s. Switching the mode on or off never
starts a turn, and the mouse delta of the two frames after any capture change or mode switch is discarded. The Alt
pan-drift (RMB + Alt) keeps its impulse model, capped at 90 deg/s. Look rates are per second (frame-rate independent).
Harness: `inertial drag dx= dy= frames= [warp_x= warp_y=]` holds the look button through the real code and injects a
release cursor jump; `state` -> `camera.cinematic`; `tools/harness/scripts/cinematic_look.satcmd`.

## History
Issue 20261030_142354: releasing RMB, or just switching cinematic mode on, flung the camera at ~200 deg/s. With RMB up,
every cursor movement over the scene was an impulse on a velocity with no drag (moving the mouse off the popup spun the
camera for good), and GLFW's cursor restore on release (a delta of minus the whole drag, arriving the next frame) was
taken as a flick. Rates were pixels per frame.
