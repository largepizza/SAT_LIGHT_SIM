---
pages: [rendering/atmosphere-and-sky.md, rendering/aurora-airglow.md, rendering/points-bloom-glare.md, development/harness.md]
code: [src/simulations/SatelliteSim.cpp, shaders/include/darksky.glsl, tools/make_skyglow_sources.py, assets/textures/skyglow_sources.bin]
---
## Now
Light pollution is one physical sky brightness, the ratio of artificial to natural sky (Falchi et al. 2016's convention;
natural = 22.0 mag/arcsec^2). Every lit texel of the 8K night map within 300 km is a source (base removed, decoded as
linear light, boosted 1 + 5 x its clipped neighbours' share because the map clips town cores, x its area; baked by
tools/make_skyglow_sources.py into skyglow_sources.bin, 394k sources). The CPU sums them with Walker's law
(d^2 + 4 km^2)^-1.25 x e^(-d/120 km) x the observer's height above each source (0.2 e^(-dh/8 km) + 0.8 e^(-dh/1.2 km)) x
4.37 into the zenith ratio Z and a per-sector directional excess D (each source's d/(d+15 km) share, toward its bearing).
In a direction: Z x (1 at the zenith .. 1.8 at the horizon) + D(az) x (0 at the zenith .. 1 at the horizon). The Milky
Way, zodiacal light, aurora, comet tails and the stars' naked-eye limit all read it. Calibrated against 16 sites
(Paranal 22.0, La Palma ORM 21.6, Cherry Springs 21.9, Lick 20.5, Boulder 18.6, downtown LA 17.1, Manhattan 17.1):
0.27 mag rms. Naked-eye limit at the zenith 7.8 pristine, ~5.6 in a Bortle 6 town, 3.2 downtown. Setting "Skyglow
(x calibrated)" (`photometry.skyglow_gain`, 1). Harness `state` -> `sky_quality` (zenith / low-elevation mag/arcsec^2,
NELM, ratio). Recomputed when the eye moves 200 m (0.01-0.2 ms); GPU cost unchanged.

## History
Replaced a 16-sector dome of a 37-km blur of the map (weighted max, compressed d/(d+0.08), x "Pollution gain" 25). The
Milky Way and zodiacal light log-mapped it from 1e-6 toward an artistic 13 mag/arcsec^2, so any site near a village read
as a city (Paranal 19.5, La Palma ORM 13.8, a Bortle 4 field near Santiago 15.3: the band was a ghost or gone); the
aurora / stars' linear decode read cities ~2 mag too dark (LA 19.5). Stars were also dimmed 99% by the old dome near any
light on top of their naked-eye gate. The Milky Way gate was 5.5 mag rms off the references, the linear model 1.5.
Settings `light_pollution_gain`, `mw_pollution_threshold_lo/hi`, `dark_sky_city_mag` removed. Residuals: lighting codes
(Flagstaff reads 1.1 mag too bright), the map's 5-km smear; the sky pass's rendered orange city glow is a separate model.
