---
pages: [using/sky-events.md, simulation/sun-moon-planets.md, development/harness.md, new: modding/lunar-bases.md]
code: [src/simulations/LunarBases.h, src/simulations/LunarBases.cpp, src/simulations/SatelliteSimMoonBases.cpp, src/simulations/SatelliteSimSkyEvents.cpp, data/lunar_bases.json, tools/lunar_glint_tool/main.cpp]
---
## Now
Two bases near the Moon's south pole carry sun-tracking solar arrays (data/lunar_bases.json, every value a scenario
assumption): China's ILRS on the Shackleton south-east ridge (88.8 S 123.4 E, the Chang'e-7 target; 1 km^2 in 500
rows, built 2032-01 to 2035-06) and NASA's Artemis Base Camp where the Shackleton - de Gerlache connecting ridge meets
Shackleton's rim (89.684 S 196.739 E; 0.1 km^2 in 100 rows, 2032-06 to 2033-12; the base runs mostly on Fission Surface
Power). The panels are vertical, on vertical-axis trackers following the Sun's azimuth, each row with its own hashed
tilt (0.8 / 0.6 deg) and azimuth (0.3 / 0.25 deg) error, 0.05-deg panel flatness, glass F0 0.04 (Schlick).

A vertical panel facing the Sun mirrors it back toward the Sun's azimuth at elevation -e_sun, so the beam reaches the
Earth only when the Earth stands at about the Sun's azimuth in the base's sky (the Moon within a few degrees of full)
and near the right elevation (the Moon near a node: the eclipse seasons). The glint seen from a place on Earth is the
sum over the rows of a flat mirror's flux, R(i) A_row cos(i) p(Delta) / D^2 of the Sun's irradiance, p the mirrored
Sun's disc blurred by the flatness; the Sun at the base is cut by its local horizon (1.5-deg dip from the rim) and by
the Earth (a lunar eclipse hides the glint). One flat panel aimed at the observer reproduces the hand formula
m = -26.74 - 2.5 log10(R A cos i / (Omega_sun D^2)) to 1e-6 mag.

Over 2036-11-22 .. 2038-11-22 the ILRS array glints brighter than mag 3 (above the air) 12 times and Artemis 7, all
within a Moon phase angle of 0.4-5 deg (hours of full Moon), in two seasons a year (January-March and May-July in
2037-38), each window 2-11 h; the ILRS peaks at mag -1.2 .. -2.4, Artemis at 0 .. +1.9. The best place on Earth usually
has the Moon low (the beam grazes the Earth's limb).

On screen the glint is a point source at the base's place on the drawn Moon (x "Moon size", refracted, extinguished),
with the point, bloom and glare of a city light; nothing per pixel. The Sky events window has a MOON BASE GLINT kind:
its Watch lands during the glint at 1x, at the brightest clear place, the camera on the base at a 1-deg FOV.
Settings `moon_bases.enabled`, `moon_bases.tilt_jitter_scale`, `moon_bases.event_mag_limit` (3). Harness
`moonbase [state] | windows | check`; offline `LunarGlintTool`.

## History
2026-10-27: added (branch moon-base-glints).
