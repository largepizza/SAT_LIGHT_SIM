---
pages: [development/controls-and-settings.md, development/harness.md, new: engine/camera.md]
code: [src/simulations/SatelliteSimCamera.cpp, src/simulations/SatelliteSim.h, src/simulations/SatelliteSimHarness.cpp]
---
## Now
The camera's state is the camera rig: the eye's direction, the look direction and the camera's up as double-precision
Earth-fixed (ECEF) unit vectors (`camEyeDir`, `camFwd`, `camUp`; the height stays `obsHeightOffset`). `obsDir`,
`obsFacing`, lat / lon, `camera.azDeg` / `elDeg` / `rollDeg` and the view matrix are copies published from it; every
move and turn goes through the `cam*` setters (`SatelliteSimCamera.cpp`), and a copy written directly is adopted with a
log line. WASD moves at the set speed in the facing direction at any height (2 m above the sea, 3 m/s, W for 3 s moves
9.1 m on bearing 0.00 deg). The GPU still gets the float direction (~0.4 m steps of the rendered eye).

Horizon auto-level (Settings > Controls: "Auto-level the horizon" `camera.auto_level`, default on; "Auto-level up to
(km)" `camera.auto_level_km`, default 100, 10-2000): below that height the mouse yaws about the local up, pitch stops
at +-89 deg and a roll eases back to level (tau 0.3 s). Above it, or off, the camera turns about its own axes with no
pitch limit, and Z / C roll it (Roll Left / Roll Right, held, 45 deg/s; boost x2, fine x0.25). A toast names the switch.
The view is built from the look and up vectors, so there is no snap near the zenith / nadir (the old view turned by
the azimuth across |el| = 88 deg). Key layout 3: Reset Elevation Z -> X, Toggle Cursor has no key (pad Start).

Harness: `camera roll=<deg>` sets the roll from level (+ = bank right); `inertial move roll=-1|1` holds Z / C through
the real code; `state` -> `camera.roll_deg`, `observer.eye_dir_ecef` (double), `camera_rig` {`fwd_ecef`, `up_ecef`,
`el_deg`, `roll_deg`, `level_active`, `level_enabled`, `level_km`, `eye_alt_m`, `adopted_writes`}.

## History
2026-11-14, camera revamp phases 1-3 (phase 4: fly where you look). Before, `obsDir` was a float and canonical:
walking under ~10 m/s rounded away (0.2 m of 9 in the test above) and up to ~30 m/s the rounding chose the direction
(34 m on bearing 2.7 deg facing 0 or 45). The view was glm::lookAt from az / el with up = Up, switching to North past
|el| 88: from 420 km, -87.9 -> -88.1 deg changed the frame by 40 levels (now 8.3). A roll audit (ground and orbit, day
and night, roll 30 / 90 / 180) matched the turned roll-0 frame to 0.1-0.4 levels: every pass reads only the view
matrix. Blending the yaw to the camera's up near the nadir under auto-level was tried and dropped (a sideways drag slid
the view from -89 to -79 deg). Details: docs/subsystems/ui.md "Camera rig".
