---
pages: [simulation/sun-moon-planets.md, rendering/atmosphere-and-sky.md, rendering/moon-and-eclipses.md, using/getting-started.md]
code: [src/simulations/SatelliteSimStars.cpp, src/simulations/SatelliteSim.cpp, tools/make_star_catalog.py, src/simulations/SatelliteSimSkyEvents.cpp]
---
## Now
The sky's stars come from `assets/stars/star_catalog.bin`, built by `tools/make_star_catalog.py` from the HYG database
v4.1 (CC BY-SA 4.0): 61,816 stars to V 8.5, sorted brightest first, 417 with IAU proper names, each with its Bayer /
Flamsteed designation, spectral type, constellation, B-V and distance (Betelgeuse at 168 pc). Each frame only the stars
that can show are written and drawn: those brighter than the point style's limit and the darkest direction's naked-eye
limit. Every star is gated by the naked-eye limit of the sky in its own direction (Schaefer-style, from the city dome,
the twilight, the Moon and the supernova), so the faint stars appear only under dark skies; under a pristine sky the
faint stars are drawn as if the point limit were 1 mag fainter (`stars.dark_sky_extra`). Named stars and stars brighter
than V 4.5 can be clicked like planets: the panel gives the name, spectral type, constellation, magnitude, distance,
designations and colour. Settings `stars.sky_limit`, `stars.limit_boost` (1.2 mag), `stars.dark_sky_extra` (1),
`stars.supernova`, `stars.betelgeuse_variable`.

Betelgeuse goes supernova (Type II-P) on 2037-12-25 03:00 UTC: breakout to mag -6 in 20 minutes, a peak of -11.2 a
week later, a plateau fading to -10.2 by day 100, a 3-mag drop, then the radioactive tail; blue-white at its peak,
orange-red on the tail, below its old +0.5 again about two years later. It is drawn with bloom and glare, is visible in
daylight near its peak, dims the faint stars like moonlight and adds its light to the night's ground and cloud
ambient. It is a Sky events kind (SUPERNOVA) whose one Watch lands at its peak (or now while it is under way) at a
dark site with Orion up. The Moon is now drawn at its true size by default ("Moon size (x real)" 1, was 2.5).

## History
Until 2026-10-27 the stars were the compiled-in Yale Bright Star Catalogue to 6.5 (8404 stars) with no names, every
star rewritten and re-uploaded each frame. The per-star refraction call and Chapman table were replaced by one table
over elevation and a per-frame grid of the naked-eye limit; the loop runs in chunks on the storm catalog's thread pool.
