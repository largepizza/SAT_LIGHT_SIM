---
pages: ["new: simulation/kessler.md", simulation/orbits.md, simulation/time-and-frames.md, rendering/aurora-airglow.md, using/bookmarks.md, development/architecture.md]
code: [src/simulations/Kessler.h, src/simulations/Kessler.cpp, src/simulations/SatelliteSimKessler.cpp, shaders/sat_orbit.comp, shaders/sat_rebake.comp, src/simulations/SpaceWeather.cpp]
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

Fragments are tracked as representative records: at most 750k, each standing for many real fragments. They
follow Keplerian orbits whose J2 precession spreads each cloud around its parent's plane. They are magnitude
13-17, so a "Highlight the debris" toggle draws them as dots.

The history is an event log, so time runs both ways exactly. The cascade is computed lazily in the background,
a minute of the current time warp ahead. Sim time holds at the computed frontier, with a banner: "Computing the
debris cascade".

The player can break any satellite up (satellite window > BREAKUP: Explode / ASAT strike) from the storm on.
The world then re-simulates from that moment.

Settings > Constellations > THE SUN'S FURY holds:
- the toggle, `kessler.enabled`, default on;
- collision rate, `kessler.rate_multiplier`, 1 = physical;
- avoidance, `kessler.avoidance`, default 0.9: the share of encounters a working satellite dodges;
- the horizon, `kessler.horizon_years`, default 20;
- live counts.

The default bookmark "The Sun's Fury" lands on the storm's first night over Monument Valley. Results are cached
in `<user data>/kessler/`.

With the shipped roster the cascade runs away:
- about 80 collisions on day 1;
- by day 200, 126k dead satellites and ~600k live fragment records, far more real fragments.

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
