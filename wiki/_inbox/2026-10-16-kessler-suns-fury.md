---
pages: ["new: simulation/kessler.md", simulation/orbits.md, simulation/time-and-frames.md, rendering/aurora-airglow.md, using/bookmarks.md, development/architecture.md]
code: [src/simulations/Kessler.h, src/simulations/Kessler.cpp, src/simulations/SatelliteSimKessler.cpp, shaders/sat_orbit.comp, shaders/sat_rebake.comp, shaders/cloud_march.comp, src/simulations/SpaceWeather.cpp, tools/make_debris_models.py]
---
## Now
"The Sun's Fury" is a Kessler-syndrome scenario that happens in sim time; there is no mode to switch on. A
Carrington-class CME arrives on 2037-07-15 at 03:00 UTC, two days after the Great Australian Eclipse. Its flare
went off 17.5 h earlier, and it brings a Kp ~11 aurora to mid-latitudes. Before that moment nothing differs from
the plain sky.

What follows:
- The storm kills satellites by altitude and inclination: about 2-8% in LEO, more higher up, crewed stations 10x
  less. Dead satellites tumble on their orbits until drag brings them down.
- Dead formation members (Starmind rings) bump their neighbours, median 4 days later, each bump killing with
  probability one half.
- A statistical collision engine (the cube method: 6-h samples, 20-km cubes, physical rates) carries the cascade
  forward. Every collision is a NASA Standard Breakup Model debris cloud of fragments of at least 10 cm.

Only what can be seen is tracked one by one. Pieces of 1 m and more are records of their own (up to 750k), drawn
with a debris kit of models: torn solar array sections, radiator sections (their OSR tiles flash like mirrors) and
pieces of bus, in three sizes. The kind follows the parent's surface, and a large satellite's appendages shear off
whole: a Starmind leaves array sections ~10 m long. Close up they are meshes, and Go to works. They follow Keplerian
orbits whose J2 precession spreads each cloud around its parent's plane. The fragments from 10 cm to 1 m (magnitude
13-17, never visible) are a density field instead: 16 statistical orbits per breakup, orbit-averaged into altitude
x latitude x heading cells, which the collision engine reads. Dead formation members drift out of their rings
(~1-4 km after a day, hundreds of km after ten).

Each breakup also makes a dust cloud, drawn volumetrically beside the aurora: a sunlit, lumpy Gaussian cloud that
follows the parent's orbit, expands at the fragments' spread speed and shears along its orbit for about three hours.
Its brightness is a physical estimate (5% of the mass as fine dust) put on the same scale as stars and satellites,
times "Dust cloud brightness" (`kessler.dust_gain`, default x100; at x1 it fades below the dark sky within minutes).
The Collision debris row's Hlt tints the pieces orange and draws them as bright census dots.

Everything that drag brings down re-enters as a fireball, drawn at the second its lifetime runs out, under where its
orbit puts it then. It follows what re-entry observations describe (Starlink re-entries over North America, ATV-1,
Hayabusa): the object glows from ~100 km and brightens; its main breakup comes at 75-80 km with a flash; then a
train of many orange fragments (3 to 80, by mass), some leading the main body and some trailing it, each with its own
glowing trail, flickering as they tumble, a few flaring once; light pieces slow and fall behind; nothing glows below
~45 km or under ~2 km/s. A 2.5-t Starmind's main body peaks near magnitude -7.5 from 100 km, a 100-kg piece -4. Heads
are point sprites (bloom, glare), trails and the main body's fading train emissive streaks in the dust renderer.
Settings > THE SUN'S FURY > "Watch the next re-entry" (harness `kessler reentry`) jumps to the next one over a dark sky,
25 s before breakup, from 120 km beside its track.

The debris pool holds up to 5 million LIVE pieces (about 580 MB of GPU memory when full): a piece's slot is reused
5 minutes after it re-enters or is destroyed, so re-entry is the garbage collection, and time still runs both ways
exactly (recent reuses are kept to play backwards over; further back, the world is replayed from its event log).
On an RTX 3070 Ti one simulated year of the cascade computes in about 8.5 minutes (~3 s per day at its busiest,
~1.2 s later) in about 4 GB of memory.

The history is an event log, so time runs both ways exactly. The cascade is computed lazily in the background,
a minute of the current time warp ahead. Sim time holds at the computed frontier, with a banner: "Computing the
debris cascade".

The player can break any satellite up (satellite window > BREAKUP: Explode / ASAT strike) from the storm on.
The world then re-simulates from that moment.

Settings > Constellations > THE SUN'S FURY holds:
- the toggle, `kessler.enabled`, default on;
- dust cloud brightness, `kessler.dust_gain`, default x100;
- collision rate, `kessler.rate_multiplier`, 1 = physical;
- avoidance, `kessler.avoidance`, default 0.9: the share of encounters a working satellite dodges;
- the horizon, `kessler.horizon_years`, default 20;
- live counts.

The default bookmark "The Sun's Fury" lands on the storm's first night over Monument Valley. Results are cached
in `<user data>/kessler/`.

With the shipped roster the cascade runs away: by day 100, 934k satellites are destroyed and 3.6M pieces over 1 m
are in orbit; by day 365, 1.07M destroyed, the pieces down to 1.4M as they shatter and re-enter.

That is expected for ~1M satellites in narrow shells.

Also new for this work:
- The orbit epoch rebake runs on the GPU (`sat_rebake.comp`, exact fixed-point turns): 1 yr/s went from 28.6 to
  16.7 ms a frame.
- The roster can be rebuilt at runtime (harness `roster list|remove|add`).
- Constellations draw their random numbers from per-constellation streams, so the layout is the same on every
  platform.

## History
Built overnight on 2026-10-16 from the user's brief. Asked whether the sim could handle a dynamic number of
satellites and Kessler syndrome, the user picked physical rates, 10-cm fragments and J2 only inside the
scenario, and wanted it to happen after the eclipse with a mystery bookmark. Dead satellites were pool records
at first; the pool passed 2M records by day 120, so they became roster records with a death second. The event
log was too large to ship (about 60 MB by day 200), so the cascade is computed at runtime and cached locally.

Round 2 (same day, after the user's first test: the debris was invisible, "you'd only know because the sim gets
laggy"): the 750k records had been 10-cm representative fragments, mostly hitting each other. Measured, rendering
them cost ~0.2 ms; the slowness was the engine and the time hold. Records became the visible pieces, the small
fragments a density field, the dust clouds were added. The first grid cut gave every hit the cell's mean fragment
mass (~0.5 kg), so nearly every hit was catastrophic (1.1M by day 60); projectile sizes are now drawn from the power
law. The dust's first cut used the sky's radiance units directly and was invisible even at 100000x: the night
exposure is not physical, so the clouds are calibrated through the point-source magnitude scale.
