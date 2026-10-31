---
pages: [simulation/launches.md, simulation/rockets.md]
code: [src/simulations/SatelliteSimLaunches.cpp, data/launches.json, tools/make_rocket_models.py]
---
## Now
The Pez stack (`launches.json` program `"stack": {"layout": "pez"}`: Starmind, Starlink, Starlink DTC, all on
Starship) is a column of plates along the ship's body, and it is drawn on the ship while the payload door is open.
`stackSlot` places member 0 (the first out) in the door and the rest aft of it one `pitch_m` apart (`0` up the
stage: `slot = (0, offset_m, door_m - i * pitch_m)`), each plate's face across the ship's facing (R: the member's
X out of the door, its Z along the stage's -Z, i.e. the stow attitude was already the perpendicular one).
`offset_m` is new: the column's centre out from the ship's axis, `hull radius 4.5 + 0.9 clearance + the member's
stowed half height along X` (Starmind 8.515 m, Starlink 9.15, DTC 7.9), derived and checked by
`tools/make_rocket_models.py` (`SHIP_HULL_R`, `PEZ_CLEAR_M`). Before this, every Pez member was placed at ONE point
on the ship's axis inside the hull (`(0, 0, door_m)`, `pitch_m` unused), so the eight plates interpenetrated each
other and the hull, and no stack was ever drawn on a Starship: `launchMeshJobs` only queued a stack for `t >= tFair`,
and a vehicle without a fairing keeps `tFair = -1`. The stack now appears in the payload door's own window
(`kPayloadDoorLeadS` 90 s before the first release to `kPayloadDoorLagS` 60 s after the last), and pez layouts are
no longer excluded from `LaunchState::stackDraws`. `launch state` -> `stack_meshes` reads 8 on a Starmind
Starship at deploy-10 s (it was 0).

Measured (the stowed point cloud of `spacex_ai_model` through the layout, tools-free script
`qa/waves/20261030b/evidence/c40/c40_stackgeo.py`): adjacent members centre-to-centre 2.13 m, closest surfaces
0.11 m (they were coincident: 0.00 m centre and surface, i.e. 8 plates in one place); the nearest member point to
the ship's axis 5.63 m against the 4.5 m barrel -> 1.13 m clear of the hull (it was 0.06 m, i.e. 4.44 m inside
it); the column's axial extent 15.15-32.13 m up the stage (17 m, the door slot is 29.8-32.4). The plates' faces
sit 0 degrees off the ship's axis (0 = across the ship's facing) before and after: the orientation itself was
already the one the report asks for. The release is unchanged in kind (each member leaves radially, `exitDir`
(0,1,0), from its own slot now instead of all from one point).

## History
2026-10-31, issue_20261019_223922 ("Starmind Orientation": "starminds exit the starship at a flat angle ...
satellites are stacked flat on top of each other going up the body of the cargo section. They need to be
perpendicular to the facing of the starship"). 369ec5f added the folding bus, the 8-to-a-ship fit and the release
sequence (`q0` = the stack attitude), which already fixed the reported exit attitude; what was left was that the
layout had no stack in it. The 2026-10-30 verification wave drew the opposite conclusion from its crops ("flat
plates broadside off the dorsal line"): it was looking at the released members, which do present their faces to a
camera behind the ship, and at a hull with no stack on it at all. `plan_stacks`' `fits.starship` corner check
(4.0 m within the bay's 4.1 m) is unchanged: the plates still fit inside the 8.2 m bay as they would really ride;
they are drawn outside it only because Starship's hull is a closed shell in the model (the door is a painted slot,
`payload_door_slot`) and there is nothing else to show.
