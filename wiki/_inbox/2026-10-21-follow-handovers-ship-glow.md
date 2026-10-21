---
pages: [using/missions.md, using/controls.md, rendering/launch-visuals.md, development/harness.md]
code: [src/simulations/SatelliteSim.cpp (updateFollow), src/simulations/SatelliteSimLaunches.cpp (transientPosAt, ship entry glow), src/simulations/SatelliteSimHarness.cpp (follow log)]
---
## Now
Go to keeps the player's offset and aim relative to the object through every milestone and handover of a launch
(staging, capsule separation, the ship's entry interface and descent, the catch, landings). For a rocket, ship,
capsule or fireball the follow frame's heading is the last frame's carried along (parallel transport in the local
horizontal) and turned toward the velocity's heading at up to 1.5 deg per sim second or 20 deg per real second; a
velocity that reverses (a booster's boostback) keeps the side; a jump in time takes the velocity's heading again.
At a handover the camera glides across the eased jump with its view held in the object's frame (no snap of the view).
Starship's ship glows where its entry heating is: on the belly (heat shield) along the body's middle, standing off
the windward side, not at the engines; as the mesh resolves the point gives way to a glow sheath along the belly
(the capsules' rule). Booster entry burns stay at the engines (they are engine burns).
Harness: `follow log on|off [name=]` records the camera against the object per frame and ranks the jumps.

## History
2026-10-21 (user: "camera resets when mission objectives occur", "re-entry emits from the rocket engine"). Measured
with `follow log`: at the ship's entry interface the handover's "from" point was the stage's position a frame old
(its model refuses times past its end), a jump of one frame of motion (1.28 km at 10x), and the view was snapped onto
the new object (21.7 deg), then 12-37 deg at the descent handover where the frame's heading also jumped; Crew
Dragon's separation snapped the view 6.2 deg from 68 m. The ship's glow point (and its light on clouds) sat at the
model origin, the aft end.
