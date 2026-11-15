---
pages: [development/harness.md, new: engine/camera.md]
code: [src/simulations/SatelliteSimCamera.cpp, src/simulations/SatelliteSim.h, src/simulations/SatelliteSimHarness.cpp]
---
## Now
The camera's state is the camera rig: the eye's direction and the look direction as double-precision Earth-fixed
(ECEF) unit vectors (`camEyeDir`, `camFwd`; the height stays `obsHeightOffset`). `obsDir`, `obsFacing`, lat / lon and
`camera.azDeg` / `elDeg` are float copies published from it; every move and turn goes through the `cam*` setters
(`SatelliteSimCamera.cpp`), and a copy written directly is adopted with a log line. WASD moves at the set speed in the
facing direction at any height (in double: 2 m above the sea, 3 m/s, W for 3 s moves 9.1 m on bearing 0.00 deg).
The GPU still gets the float direction (~0.4 m steps of the rendered eye).

Harness: `camera roll=<deg>` banks the view about its look direction (+ = bank right; `SkyCamera::rollDeg`, 0 =
the view unchanged; the player has no roll control yet); `state` -> `camera.roll_deg`, `observer.eye_dir_ecef`
(double), `camera_rig` {`fwd_ecef`, `el_deg`, `adopted_writes`}. A roll audit (ground and orbit, day and night, roll
30 / 90 / 180) matched the turned roll-0 frame to 0.1-0.4 levels: every pass reads only `skyView`.

## History
2026-11-14, camera revamp phases 1-2 (toward first-person fly-where-you-look, horizon auto-level with roll above
100 km). Before, `obsDir` was a float and canonical: walking under ~10 m/s rounded away (0.2 m of 9 in the test
above) and up to ~30 m/s the rounding chose the direction (34 m on bearing 2.7 deg facing 0 or 45). The audit also
measured the nadir snap: `viewMatrix()` switches its up vector at |el| = 88 deg and the image turns by the azimuth.
Details: docs/subsystems/ui.md "Camera rig".
