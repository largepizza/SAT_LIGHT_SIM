---
pages: [new: simulation/wildfires.md, simulation/weather.md]
code: [src/simulations/Biome.cpp, src/simulations/Wildfire.cpp, src/simulations/SatelliteSimWildfire.cpp, shaders/wildfire_ground.comp, shaders/sat_sky.frag, shaders/sat_point.frag, shaders/kessler_fireballs.comp, shaders/include/cloud_params.glsl]
---
## Now
(Supersedes the 2026-10-23 note's paragraphs on the front, its points' brightness, the smoke's source and the glow.)

A fire burns all round only while it is young; within about half a day only an ACTIVE ARC is burning: the head and the
upper flanks (a half angle of 50-80 degrees of the burnt ellipse), turned a little to one side, broken into runs of
flame with long gaps where nothing burns, plus a few isolated flare-ups. Behind the arc the ground is a burn scar: the
day map turns charcoal with grey-white ash patches and unburnt islands, a sparse field of smouldering spots glows at
night, and a band just behind each burning stretch glows red as it burns out. A front stops where the ground cannot burn
(a lake, a town, bare desert), and no fire starts in a town or city: the land-cover map carries an urban mask from the
night map's lights.

The front's points are restrained: a big fire's head is a modest orange line from orbit (each point about magnitude 2)
and never wider than a star's core close up; by day they are dimmed against the sunlit ground. They stand on the ground
as drawn (a small GPU pass puts every point and flame on the detailed terrain, which the CPU does not know): before, most
of them sat under the drawn surface close up and disappeared. Within ~40 km the flames are glowing tongues along the arc.
At night a fire's light (up to three points along the arc, a few km of reach) lights its smoke's base and the ground
round it, no longer the haze over a whole view.

The smoke rises from the whole active front: the front is cut into up to five sections of equal heat, each a source as
wide as its section rising to its own layer (weaker sections lower), so the smoke leaves the arc as a curtain and merges
downwind into one plume; it starts a little above the flames.

## History
The user's QA (2026-10-24): "way too bright, overwhelming even from space"; "they disappear when you are close"; "fire
generates as rings, at best arcs with scattered flames here and there"; "they seem to generate over cities"; "smoke trails
only generate from singular points, not the full arc". Causes found: each point was compressed toward 8 (16x the point
style's white: a swollen disc, a big head from LEO ~5.5); the night glow was 0.06 of the front's light at the head
(~2000 suns m^2 x the night's ~2600x lift: the haze lit orange over a whole view); the front's heat floored every gap and
the back at 10-12%, so the points summed into a closed ring; points sat on the CPU's DEM under the drawn (detailed) ground
and the depth test hid them; a light on the ground behind haze was dimmed by T^2 (the dust over a dry savanna hid fronts
past ~50 km); the fuel had no urban term; one streak rose from a point downwind of the head. Measured: world fires on
2036-08-12 418 -> 340 with the urban mask; burning front samples (of ~3600) on lit land, night luminance >= 30 / >= 60:
74 / 34 with a first, looser mask, 1 / 0 now.
