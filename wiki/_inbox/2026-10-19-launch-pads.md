---
pages: [using/features.md, simulation/launches.md, rendering/terrain.md, rendering/city-lights.md, using/settings.md, development/harness.md]
code: [src/simulations/SatelliteSimLaunches.cpp, src/simulations/Launches.cpp, shaders/sat_sky.frag, shaders/include/terrain_detail.glsl, shaders/city_sprites.comp, tools/make_pad_models.py, data/launches.json]
---
## Now
**Launch sites are drawn on the ground.** Every distinct pad in launches.json (35) has its graded ground, a concrete
apron, a flame trench (or Starship's steel deflector and soot), tank slabs, an access road with lamps, and (Falcon
pads) a landing zone ~2.3 km out with its painted ring and cross; the terrain is graded flat to the pad's height
(read from the elevation map). The towers stand on them as meshes: Starship's 146-m catch tower with its chopsticks
over the 20-m launch mount, a Falcon pad's red service structure and lightning masts, the tank farms. At night the
floodlights, red beacons and lit concrete show; within ~3 km of a pad there are no city lights, street grid, farms
or city light sprites. Everything averages out with distance: from orbit a pad is a pale speck. Setting: Settings >
Constellations > LAUNCHES "Launch site graphics" (`launches.pad_graphics`, on).

**Starships come home.** A ship's deorbit burn is timed so that its entry ends a few hundred km short of a Starship
pad; it then falls belly first to ~1 km over the tower, flips, fires its landing burn and is caught by the
chopsticks (55 m up), where it stays for half an hour. Super Heavy boosters are caught the same way (30 m above the
ground, on the mount's centre). The entry glow starts exactly where the deorbit ellipse is (it jumped ~0.5 km).
"Watch a Starship catch" in the LAUNCHES section jumps to 90 s before the next catch.

Harness: `launch pads`, `launch pad <site> [dist_m=] [az=] [h=]`, `launch landing [booster=1] [lead=] [dist_m=] [day=1]`,
`debugview pads`; `launch check` reports the landing's continuity (entry start 8 m, hand-over 0 m / 7 m/s, catch
0 m off centre). launches.json: vehicles `mount_m` / `catch_m`, sites `pad_kind` / `pad_heading_deg`.

## History
Until 2026-10-19 the ship re-entered as an 8-minute plasma glow and was not followed to landing, rockets stood at the
sea's level whatever the site's height (Jiuquan 1.6 km), a caught booster vanished at touchdown, and city lights ran
across the launch sites. The first cut of the pads graded every coastal pad into a round island and turned the
Starbase lagoon into land (the elevation map reads 0 m there); water and sea-level land are now left alone except the
apron itself.
