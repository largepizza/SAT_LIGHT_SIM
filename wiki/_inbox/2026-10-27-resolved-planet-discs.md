---
pages: [rendering/sky.md, using/settings.md]
code: [src/simulations/SatelliteSimPlanets.cpp, shaders/planet_disc.frag, tools/make_planet_maps.py]
---
## Now
Zoomed in, a planet whose equatorial radius passes ~1 px becomes a resolved disc (its own small pipeline, drawn after the
planet points, additive at infinity depth): phase and terminator from the Sun, an oblate spheroid with the IAU pole and
rotation, Mars / Jupiter / Saturn maps (Solar System Scope, CC BY 4.0, baked by tools/make_planet_maps.py), Saturn's rings
(a radial profile with the Cassini division, both shadows, lit / unlit face), Venus's atmospheric rim near inferior
conjunction, Jupiter's Galilean moons (Meeus ch. 44: eclipses, occultations, transits, their shadows on the disc). The
disc carries the planet's magnitude (Saturn's now includes the rings' term); from 1 to 2.5 px it cross-fades from the
point, its light matched to the point's at 2.5 px and physical by 5 px. It is not dimmed by the point model's day /
light-pollution suppression (it is added to the real sky, like the Moon). Settings: "Resolved planet discs"
(`planets.resolved_discs`, on), `planets.disc_gain` 1.5. The selection panel shows the angular diameter, Jupiter's moon
configuration, Saturn's ring opening and Mars's central meridian; the Galilean moons are selectable.

## History
Galilean positions checked against JPL Horizons on 2036-11-22 07:00 UT: east-west offsets within 0.7" (Io 86.8" vs 86.2"),
north-south within ~2". Cost with Jupiter resolved: GPU within noise, CPU +0.04 ms.
