---
pages: [rendering/sky.md, new: rendering/refraction.md]
code: [src/simulations/Refraction.h, src/simulations/Refraction.cpp, src/simulations/SatelliteSimRefraction.cpp, shaders/sat_sky.frag, shaders/star_point.vert]
---
## Now
Refraction, mirages and optical turbulence come from one ray tracer (`refr::build`, Refraction.cpp) run through the air's
profile at the eye: a standard atmosphere (refractivity N = 77.6 P/T with Edlen dispersion), the surface layer over a sea
or ground warmer / cooler than the air (log profile, sea-air step 0.2-1.6 K by place and day, breathing over tens of
seconds), and on some days an elevated inversion 3-400 m up riding gravity waves (`SatelliteSim::refrProfileAt`, a pure
function of place and sim time). For 512 apparent elevations round the horizon it stores the refraction per colour
channel (or "ends on the sea"), the air columns along the BENT path, the rms angle-of-arrival jitter (2.91 D^-1/3
int Cn^2 ds, D 1 cm) and the scintillation index (Rytov, saturated). Rebuilt on a worker, 10 times a second while the
Sun is within ~4.5 deg of the horizon, else once a second (~7-11 ms a build on 6 threads). The Sun's disc is drawn
through it: flattened, an inferior mirage over a warm sea (Etruscan vase, Omega Sun) or slices under an inversion, the
green rim, and a limb that boils with the turbulence; its light is the physical disc radiance (4 pi / Omega_sun in the
sky's units) through the bent path's columns, saturating per channel, so a clean ocean horizon Sun is brilliant
yellow-orange. Stars and planets are placed by the same table and twinkle / jitter by its turbulence. The sea's sun
glint sparkles (energy-conserving: pixels light with chance q and carry glint / q). Settings `refraction.enabled`,
`mirage` (1), `turbulence` (1), `dispersion` (1 = physical), `sun_disc_gain` (1), and harness overrides
`force_sea_air_dt`, `force_inversion_m/_dt/_dz`; harness `refraction [rebuild]`, `state` -> `refraction`.

## History
Replaced (issue 20261026_203414 "Sun refraction mk 2") Saemundsson above the horizontal and a tangent-height formula
below it with a 0.8 gradient cap: the two disagreed (0.48 vs 0.57 deg) at the horizontal and the cap halved the bending
just below it, so a low Sun's disc was cut flat at the horizontal with a step. The disc was drawn at 300 (about 1/600
of its physical radiance in the sky's units): a horizon Sun was no brighter than the sky round it — that, not the fog /
dust layers (the cloud composite's transmittance toward the Sun measured 1.0 in the report's view, and turning fog,
dust and haze off changed the disc by 0-7 levels), is why every sunset was a dim red disc.
