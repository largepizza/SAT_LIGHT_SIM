# Kessler syndrome: The Sun's Fury

"The Sun's Fury" is a Kessler-syndrome scenario that happens in sim time: a solar storm kills satellites, the
dead ones collide, every collision makes debris, and the debris makes more collisions. This page covers the
physical model (the storm, the breakup model, the collision engine, drag), how the history is computed and kept
reversible, and how debris, dust clouds and re-entries are drawn. The launches that stop after the storm are on
[Launches](launches.md); the aurora the storm brings is on [Aurora and airglow](../rendering/aurora-airglow.md).

There is no mode to switch on. Before the storm arrives nothing differs from the plain sky, unless the player
breaks something up, which starts the scenario at that moment.

## The scenario

A Carrington-class coronal mass ejection (CME) leaves the Sun on 2037-07-14 at 09:30 UTC and arrives at the <!-- history-ok -->
Earth 17.5 h later, on 2037-07-15 at 03:00 UTC, two days after the Great Australian Eclipse. Its arrival is <!-- history-ok -->
the scenario's **epoch**: every time in it is counted in whole seconds from that moment (see
[Time and reference frames](time-and-frames.md#scenario-seconds)). The storm drives the space-weather model to
Kp about 11 for about half a day, so the aurora reaches mid-latitudes.

What follows, in order:

1. **The storm kills satellites.** Each working satellite present at the epoch dies with a probability that
   depends on its altitude and inclination (the radiation environment), drawn from a hash of its index:

    | Altitude | Probability |
    |---|---|
    | below 650 km | 1.5% |
    | 650 to 1100 km | rising linearly from 1.5% to 5% |
    | 1100 to 5000 km | 8% |
    | above 5000 km | 12% |

    multiplied by \( 0.7 + 0.6\sin^2 i \) (high inclinations cross the auroral zones), and by 0.1 for crewed
    stations (anything over 10 t: shielded and redundant). 85% of the deaths come in the first 15 hours, most
    of them early; the rest are latent failures over the following 1 to 21 days. Debris is passive and is not
    killed.
2. **Dead formation members bump their neighbours.** A dead member of a Starmind ring stops holding its
   station and drifts into the next member of its cluster after a lognormal delay (median 4 days). Each bump
   kills that neighbour with probability one half, and a killed neighbour drifts on into the next.
3. **The collision engine carries the cascade forward.** Every 6 hours of sim time it samples who is close
   to whom and draws collisions at physical rates. Every collision is a debris cloud from the NASA Standard
   Breakup Model, and the debris joins the next sample.

A dead satellite tumbles on its orbit until drag brings it down; a destroyed one is gone. Each roster
satellite therefore has a **fate** at any sim time: working, dead (tumbling), destroyed (broken up) or
re-entered. A destroyed or re-entered satellite cannot be selected, a selection that goes is dropped, and a
lost crewed station gets no more crew flights (see [Crew flights](crew-flights.md)).

## The history: an event log

A collision depends on everything that collided before it, so unlike the rest of the simulation the scenario
has a genuine history. It is kept as an **append-only event log** (`kessler::Event`): a time, the objects
involved, a kind and an origin.

| Kind | What |
|---|---|
| kill | a satellite dies (the storm, or damage) and tumbles from then on |
| bump | a dead formation member drifts into its neighbour |
| collision | a hypervelocity collision between two objects, or between an object and a small fragment |
| explosion | an object breaks up on its own (the player's Explode) |
| ASAT | a 20 kg interceptor at 9 km/s (harness only) |

The origin is the scripted storm, the engine or the player. Applying an event writes the pool records it
creates and the born, dead and end seconds of the objects it touches. Everything drawn is then a **function of
the current second**: a record is shown between its born and death seconds, a roster satellite tumbles from its
dead second and vanishes at its end second. Playing time back before an event simply stops drawing its results,
so time runs both ways exactly, as everywhere else in the simulation.

Random numbers come from hashes of (seed, index, channel), never from a shared stream. Engine seeds are
derived from the 6-hour step's own index on a fixed grid from the epoch, and a step applies its scripted events,
then the engine's, then the player's, in time order. So the history does not depend on thread count, on the
order objects are evaluated in, or on when the background computation happened to pause.

## Dead satellites

A dead roster satellite stays a roster record: it keeps its orbit and never manoeuvres again. Two things change.

- **It tumbles.** Its attitude switches to the tumble law, with an integer period of 40 to 600 s, a phase and
  an axis hashed from its index. The angle is taken modulo one period from the scenario's whole seconds, so the
  GPU and the CPU agree exactly at any date (see [Attitude](attitude.md#the-tumble-law)).
- **A dead formation member drifts out of its ring**, with no station-keeping against differential drag. Its
  along-track offset grows as \( \tfrac12 k\tau^2 \) with \( \tau \) the time since death and
  \( k = \pm j \times 2^{-47} \) turns/s² (\( j \) = 1 to 3, hashed): about 1 to 4 km after a day and 100 to
  400 km after ten. The angle is formed modulo one turn in exact 48-bit integer arithmetic on both sides
  (`formationDriftTurns()` on the CPU, `formationDrift()` in `sat_orbit.comp`); they agree to about 1 m.

Drag sets only its **end second**: when it re-enters (see [Drag](#drag-and-lifetimes)). Its orbit does not
visibly decay before that.

## Breakups: the NASA Standard Breakup Model

Every collision and explosion is evaluated with the NASA Standard Breakup Model (Johnson et al. 2001,
Krisko 2011), in `breakupCollision()` and `breakupExplosion()`.

**Collisions.** With target mass \( M_T \), projectile mass \( M_P \) and relative speed \( v \), the
specific energy is \( E = \tfrac12 M_P v^2 / M_T \). At \( E \geq 40 \) J/g the collision is
**catastrophic**: both objects shatter and the fragmented mass is \( M = M_T + M_P \). Below it the target is
cratered and \( M = M_P\,(v / 1\ \text{km/s})^2 \) of ejecta comes off. The number of fragments larger than a
characteristic length \( L_c \) (m) is

\[
N(>L_c) = 0.1\,M^{0.75}\,L_c^{-1.71}.
\]

**Explosions.** \( N(>L_c) = 6\,S\,L_c^{-1.6} \). The model's scale factor \( S = 1 \) describes a
rocket stage of a few tonnes, so a heavier parent breaks into more pieces:
\( S = \max\!\big(1,\ (m / 2\ \text{t})^{0.75}\big) \), the collision law's mass exponent. The ISS makes about
13 000 pieces over 10 cm and about 330 over 1 m.

Each fragment takes:

- an area \( A = 0.556945\,L_c^{2.0047} \) and an area-to-mass ratio from the model's bimodal distribution in
  \( \chi = \log_{10}(A/m) \), whose two modes and weights depend on \( L_c \);
- an ejection speed lognormal in \( \nu = \log_{10} \Delta v \) with mean \( 0.9\chi + 2.9 \) (collision) or
  \( 0.2\chi + 1.85 \) (explosion) and standard deviation 0.4, in an isotropic direction.

The size distribution is truncated to \( [0.1\ \text{m}, L_\text{max}] \), where the largest piece is 0.15 of
the larger object's bounding diameter (at most 25 m), and it is cut at 1 m into two populations, handled
differently below.

**What happens to the objects.** A catastrophic collision ends both. A cratering collision that leaves no piece
over 1 m, by a projectile that survives it, on a target it does not kill, changes nothing visible and is not
recorded (once debris abounds there are millions of these). A cratering hit kills a working satellite with
probability one half: it tumbles from then on. When a formation member breaks up catastrophically, its cloud
sweeps through its cluster within minutes: each neighbour is damaged (killed, tumbling) with probability 0.6.

Each breakup also records its **dust**: 5% of the fragmented mass as fine dust (30% of that for a cratering
hit), its small fragments' count and rms ejection speed, and where and how fast the parent was moving. These
drive the [dust cloud](#dust-clouds).

## Large pieces: records drawn with the debris kit

Fragments of 1 m and more are what can be seen, so each is a **record** of its own in the debris pool: a
Keplerian orbit (see [Orbits](orbits.md#records-beyond-the-roster)) with a born second, a death second, a mass,
a radius and a ballistic coefficient \( C_d A/m \) with \( C_d = 2.2 \).

- **Count.** Up to 96 records per breakup while the pool is empty, fewer as it fills; beyond that each record
  stands for several real pieces (its weight). The large pieces never carry more than 60% of the parents' mass.
- **Orbit.** The parent's state plus the fragment's ejection velocity, converted to elements. A child keeps its
  parent's secular rates plus the J2 *difference* between its orbit and the parent's, so a cloud spreads around
  its parent's plane by differential precession while the roster itself needs no J2.
- **Spin.** Large pieces tumble slowly, seconds to a couple of minutes a turn: their glints are long flashes.
- **Appendages shear off whole.** The size distribution was made for compact bodies. When a satellite larger
  than 20 m shatters, 2 to 6 solar array sections (if arrays are more than 20% of its surface) and 1 or 2
  radiator sections (more than 5%) of up to 0.15 of its size (at most 12 m) tear off at 10 to 50 m/s. A
  Starmind leaves array sections about 10 m long; they stay near the parent's orbit and are the brightest
  flashes in the debris.

**The debris kit.** Each piece is drawn as one of nine models made by `tools/make_debris_models.py`: a torn,
folded array section with a spar stub (`debris_panel_*`), a radiator section of white paint and optical solar
reflector tiles whose glints flash like mirrors (`debris_radiator_*`), and a piece of bus with insulation and
fittings (`debris_chunk_*`), each small, medium and large. The kind follows the parent's surface (the share of
its facets in array and radiator materials, `debrisArrayShare` / `debrisRadiatorShare`), and the area is
snapped to the nearest size class's mean area, with the mass scaled to keep the fragment's area-to-mass. A
piece's brightness is therefore its model's, exactly: close up it is drawn as a mesh, and Go to and the info
window work on it.

The pieces belong to the **Collision debris** pseudo-constellation, appended to the list at load. Its Show and
Highlight work like any constellation's; highlighted debris is tinted orange and its census dot is drawn eight
times larger than a satellite's, since the census dot alone sits near the eye's limit.

## Small fragments: a density field

The fragments from 10 cm to 1 m are magnitude 13 to 17, never visible, and far more numerous: tracked one by one
they would mostly collide with each other. They are a **density field** instead (`kessler::DebrisGrid`).

- Each breakup's small population is 4 to 16 **statistical orbits**, stratified in size, each carrying an equal
  share of the count.
- Each statistical orbit is spread over its whole orbit into cells of 20 km of altitude × equal-area latitude
  bands (90) × 8 headings of 45 deg (110 altitude bins from 150 km), with the longitude averaged out. Its
  argument of latitude is sampled at 32 points by a rotation recurrence.
- Counts are fixed-point integers (4096 per fragment), so adding and removing deposits in any order is exact,
  and the history does not depend on order.
- They leave the field through **death buckets**: each statistical orbit's drag lifetime puts its deposit in
  the bucket of the 30-day window in which it dies, and when a window ends its bucket is subtracted from the
  grid. Memory is bounded (one bucket per window ahead), and deaths are up to 30 days late.

**Hits from the field.** For an object at radius \( r \) with velocity \( \mathbf{v} \), each heading bin of
its cell contributes \( n_h\,v_{\text{rel},h} \), where \( n_h \) is the bin's density over the cell's volume
and

\[
v_{\text{rel},h} = \sqrt{\big(2|\mathbf{v}|\sin\tfrac{\Delta\psi_h}{2}\big)^2 + (300\ \text{m/s})^2},
\]

\( \Delta\psi_h \) being the angle between the object's heading and the bin's (the 300 m/s stands for the
radial speeds of eccentric orbits). The probability of a hit in a step of \( \Delta t \) is
\( \sum_h n_h v_{\text{rel},h} \times \pi (r_\text{obj} + 0.15\ \text{m})^2 \times \Delta t \), times the
object's weight and the rate setting, times \( 1 - a \) for a working satellite (avoidance \( a \)). The
projectile's size is drawn from the \( L_c^{-1.71} \) power law between 10 cm and 1 m and its mass from the
area-to-mass distribution, so most hits are by 10 cm pieces of tens of grams, which crater rather than
shatter. Collisions between two small fragments are not modelled.

## The collision engine: the cube method

`engineSample()` implements the cube method (Liou 2006). Every 6 hours of sim time it takes one sample at the
step's middle:

1. Every live object is placed: the roster's dead and passive satellites, a quarter of the working ones
   (subsampled, each with weight 4), and every pool record (with its weight).
2. Space is divided into 20 km cubes; objects are sorted by cube.
3. Every pair sharing a cube, at least one of them passive, collides with probability \( P \) below. Pairs
   of two working satellites are skipped: they belong to coordinated constellations.
4. Every object is tested against the small-fragment field (above).

\[
P = \frac{v_\text{rel}\,\pi (r_A + r_B)^2\,\Delta t}{V}\, w_A w_B \times \text{rate} \times (1 - a)^{[\text{one is working}]},
\]

with \( V \) the cube's volume and \( w \) the objects' weights.

The collisions found are spread over the step at hashed times and applied in time order, so collisions do not
all happen in the same second. Collision radii are \( \sqrt{A/\pi} \) of each object's mean projected area.

**Before the storm** (only when the player breaks something up early), working satellites dodge everything
(avoidance 1) and are left out of the sample: only the dead, the debris and passive junk collide. The
configured avoidance applies from the CME on.

### Drag and lifetimes

A record's death second is its drag lifetime: the time for its orbit to decay to about 120 km, integrated
through a piecewise exponential atmosphere for moderate solar activity, with eccentric orbits losing their
apogee first. During the storm the thermosphere's density is multiplied by \( 1 + 7\,s(t) \), with \( s \) the
scripted storm's shape. From 12 days after the CME the storm's factor is exactly zero, so lifetimes depend only
on the orbit and \( C_d A/m \); they come from a lazily filled table (perigee height × log ballistic coefficient,
log-interpolated). Orbits do not visibly decay before the re-entry second; the object vanishes from its altitude
and the fireball appears below it.

## Computing the history in the background

The world (`kessler::World`) lives on a worker thread at below-normal priority, using about half the cores for
placing objects. It is **lazy**: it simulates only a minute of the current time warp ahead of sim time (at
least two days, and always the storm's first two days), and idles otherwise. After a fast warp it stops rather
than computing hundreds of days ahead.

The main thread **mirrors** the worker's output each frame: new and changed records, roster changes, events
and small-fragment streams, worked through within a time budget per frame, then uploaded (a persistent 16 MB
staging buffer, at most about 16 000 new records a frame). At fast warps the deaths of records already on the GPU
are sent in the order they arrive, at most about 24 000 (about 2 ms) a frame, without sorting the backlog; the
roster's dirty list sorts only what is new; and when the GPU pool grows, the buffers it replaced are freed on a
thread. Sim time may run about two seconds of the current
warp (at most one 6-hour step) past what has been mirrored; past that it **holds**, with a banner, "Computing
the debris cascade: day n of N", until the worker catches up. The live counts in the settings are gathered over about a
second of frames, not walked all at once.

### Player breakups

The info window's **Explode** button breaks up the selected satellite or debris piece at the current sim
time, at any date. Rockets, raising trains and retiring satellites cannot be broken up, nor a satellite whose
slot is not filled yet. A breakup before the CME starts the scenario at that step (switching it on if it was
off).

A breakup is computed **in place**, without replaying the history:

1. The worker stops. The world keeps a **checkpoint** at each step start from one step before the player's time,
   and a **journal** of what every change overwrote since the oldest one: pool records, slot generations,
   roster seconds, the free list, the release heap, grid and bucket deposits (undone by their exact negatives).
2. The world is rewound to the last checkpoint at or before the breakup (`World::rollback()`), undoing the
   journal in reverse.
3. That one step is re-run on the main thread: the engine's own events of the step (they were sampled before
   the player acted) and then the breakup. The main thread's mirror is rewound to the same moment.
4. The worker carries on from there.

The debris is there the same frame and time keeps running: 30 to 100 ms on the main thread on day 9 of the
shipped cascade, 229 ms on day 30. The journal is capped at a million entries (about 200 MB); past that, when
time has been played back past the kept checkpoints, or for a breakup before the world's start (the first breakup
before the CME), the breakup falls back to rebuilding the history from the epoch with the breakup in it. That rebuild
(`kesslerRestart()` with `async` true) runs on a thread of its own: sim time holds at the breakup under the banner
"Computing the debris cascade: the breakup" until `kesslerRestartPoll()` installs the new worker (about 0.15 s for a
breakup before the CME), so the frame of the click is not stalled. A breakup clicked while such a rebuild runs is
queued and applied once the rebuild is in, again without a wait on the main thread. Both paths give identical
histories.

!!! warning "Invariant"
    Every change to `World` state must be journaled (`jPool()`, `jSimple()`, `jDeposit()`), or a rollback
    silently keeps it and the in-place and rebuilt histories diverge.

The player's breakups are session state. Settings > THE SUN'S FURY lists them and can undo them all ("Undo my
breakups"), which re-simulates without them.

### The cache

The engine's events and the frontier are written to `<user data>/kessler/suns_fury_<hash>.bin` when the worker
finishes or the app exits, only longer than what is already cached, and never while the player has breakups.
The hash covers the roster and its deployment dates, the types, the scenario settings, the debris kit and a
version number. A later launch replays the cache (applying events without sampling) instead of re-sampling; a
breakup before the CME ignores it. The three newest caches are kept.

!!! warning "Invariant"
    Bump `kCacheVersion` in `SatelliteSimKessler.cpp` whenever the world's logic changes. The hash sees data,
    not code, so an old cache would otherwise replay a history the new code would not produce.

## Slot recycling

The pool holds up to 5 million **live** pieces (about 580 MB of GPU memory when full). A record's slot is
released 300 s after its death (re-entry or destruction, so the fireball can finish) and reused by the next
record born after that, from a last-in-first-out free list fed in time order. Re-entry is therefore the garbage
collection. Each reuse is a new **generation** of the slot. The list is deterministic, so a replay reassigns
every slot identically and the event log's references stay valid.

The worker's change log carries a copy of every record as placed, so a piece that lives and is superseded
between two hand-overs still reaches the main thread. The main thread shows, per slot, the generation alive at
the current second, keeps the generations of reused slots from the last 7 days to play backwards over, and
re-checks a slot when sim time crosses a second at which it changes hands, in either direction. Playing back
further than that rebuilds the world from its event log (a replay, no sampling).

## Dust clouds

Each breakup of the last 3 hours with dust is drawn as a sunlit cloud along its parent's orbit, beside the aurora
in the half-resolution cloud pass (`cloud_march.comp`).

**Shape: a tube along the orbit.** The fragments' positions follow the Clohessy-Wiltshire solution for velocity
kicks \( \Delta\mathbf{v} \sim \mathcal{N}(0, \sigma_v^2 I) \) at the breakup (\( x \) radial, \( y \)
along-track, \( z \) cross-track, \( n \) the mean motion):

\[
\begin{aligned}
x &= \tfrac{\sin nt}{n}\,\Delta v_x + \tfrac{2(1 - \cos nt)}{n}\,\Delta v_y, \\
y &= \tfrac{2(\cos nt - 1)}{n}\,\Delta v_x + \Big(\tfrac{4\sin nt}{n} - 3t\Big)\Delta v_y, \\
z &= \tfrac{\sin nt}{n}\,\Delta v_z,
\end{aligned}
\]

with \( \sigma_v \) the small fragments' rms kick, clamped to 80 to 500 m/s. The cloud is cut into pieces along
\( y \) (arc length on the parent's orbit). Each piece holds the fragments at that \( y \): Gaussian across, with
the radial mean and spread conditional on \( y \), and a cross-track width \( \sigma_v|\sin\theta|/n \) where
\( \theta = nt + y/r \) is the angle those fragments have travelled since the breakup. So the tube **pinches**
through the breakup point and the point opposite it, where every fragment's orbit crosses, as real debris
clouds do. The widths are floored at about 30 m plus a little of the saturated width, and at half the bow of a
piece's straight extent from the arc.

The pieces are Gaussians at a regular spacing \( D \) whose sum is the cloud's along-track profile. \( D \) is a
continuous function of age: short enough that a straight piece stays inside the tube as the arc curves, never
shorter than about three half-resolution pixels at the cloud's nearest part, and longer when the budget binds.
Because it never steps, the cloud stretches into an arc without jumps; the end pieces carry the profile's tails,
so pieces appear and leave with almost no weight. Lumps along the cloud are a smooth noise in the fragments'
along-track kick, so they move and stretch with the fragments.

**Light.** The optical depth through each Gaussian is closed form (an error function along the ray). It is
single-scattered with a phase \( 0.6\,\text{HG}(0.7) + 0.4 \) isotropic and an albedo of 0.25, shadowed by the
Earth at the ray's closest approach, extinguished by the air, and hidden by cloud from below. Brightness is a
physical estimate put on the point sources' scale: a cloud of magnitude \( m \) carries the on-screen energy of
a star of magnitude \( m \). The sky's own night radiance is not physical (the night exposure is a display
choice), so the dust is calibrated through the magnitude scale rather than in sky units. Physically a cloud fades
below the dark sky within minutes; "Dust cloud brightness" (default ×100) keeps it visible longer. A big parent's
cloud grows as the square root of its dust past about 5 t at the default gain, so a station's cloud does not
blow out the view.

**Budget.** The dust may use up to 384 of the 1024 Gaussians a frame of the shared emissive list (the rest go to
fireball trails and rocket plumes). Clouds are ranked by **surface brightness**: their amplitude over
(along-track spread × half-width + (nearest part's range × 0.02 rad)²), so fresh, dense clouds win over old ones
spread across the sky, and a cloud smaller than about a degree ranks as if it were that size. Every cloud kept
gets at least its minimum number of pieces (15, fewer for a cloud small on screen) and the rest is shared out in
proportion to what each wants. Clouds near the budget's cut fade by priority, except the first 60% of those
kept, so nothing pops as the view moves. A cloud fades out over the last 40% of its 3 hours.

## Re-entries

Everything drag brings down re-enters as a fireball: dead satellites, large pieces, and (as a lighter version)
small fragments. A re-entry is drawn for 200 s from the second drag ends the object, under where its orbit puts
it at that moment. Only deaths **at** the drag second are re-entries; a destroyed object makes none.

**The model** follows re-entry observations (Starlink re-entries over North America, ATV-1, Hayabusa):

- The object glows from 100 km at 7.6 km/s, brightening as it descends (6 mag fainter at 100 km than at
  breakup).
- The main breakup comes at 78 km, about 81 s in, with a flash.
- Then a train of 3 to 80 fragments, by mass (\( 3 + 40\sqrt{m / 2500\ \text{kg}} \)). The remnant keeps its
  speed longest (a 160 s deceleration timescale); others slow over 15 to 115 s, light ones falling behind and
  below. About one in eight leads the main body by 150 to 350 m/s; cross-track spread is about 110 m/s.
- Each fragment's light goes as \( v^2 \) times the air's density, with a tumbling flicker; a quarter flare once
  (a tank, a battery). Nothing glows below about 2 km/s or 45 km.
- Colour runs from yellow-white while fast through orange to red-orange as it slows.
- **Brightness:** the main body at breakup is magnitude \( -4 - 2.5\log_{10}(m / 100\ \text{kg}) \) at 100 km:
  −7.5 for a 2.5 t Starmind. The fragments together shine about as the body did. Distance, daylight and the
  extinction along the eye-to-fireball path apply, so a fireball seen from above (an aircraft, orbit) is not
  dimmed by air it does not cross.
- **Material colours** ("Fireball material colours"): each fragment hashes a material that tints its
  temperature colour: steel, soot and composites 38% (no tint), aluminium 25% white, copper 12% green, titanium
  and magnesium 8% blue-white, sodium 10% yellow-orange, lithium cells 7% crimson (strongest while they flare).
  A large object's remnant keeps the hull's temperature colour.

Heads are point sprites appended to the visible list after the photometry pass (`kessler_fireballs.comp`), so
they get bloom and glare like any satellite. Each bright fragment's trail (its last 1.5 s of path, ending at its
head) and the main body's fading train (6 segments over about 18 s) are emissive Gaussians in the dust list.

**Small fragments re-enter too.** Each breakup's small statistical orbits that come down within the horizon are
merged by decay time into at most 4 **streams** (24 bytes each: decay and breakup seconds, a window of 15% of the
lifetime plus the merged spread, 1 hour to 5 days, plane, J2 node rate, count, mean mass). The main thread
expands them **per hour**: each stream overlapping an hour puts its share of re-entries into it, each at a hashed
second, point of the orbit and node spread. That is a pure function of the hour, so time still runs both ways.
They are drawn with the same model as 1 to 3 pieces, at magnitude \( -1.5 - 2.5\log_{10}(m/\text{kg}) \) at 100 km
divided by "Small re-entry brightness" (×1 is physical: about +1 for a typical 0.1 kg piece, meteor-faint). They
are not drawn past 1 h/s of time warp. From the ground they are obvious streaks; from orbit, moving dots.

"Watch the next re-entry" (harness `kessler reentry`) jumps to the next dead satellite coming down over a dark
sky (Sun below −8 deg there) within 48 hours, or else the heaviest piece within 6 hours: 25 s before breakup,
at 1x, from 120 km beside its track and 80 km back, looking at the breakup point.

## Measured behaviour

With the shipped roster at the physical rate (RTX 3070 Ti, 1600x900):

| Day after the CME | State |
|---|---|
| 30 | about 100 000 large pieces of the largest class |
| 60 | about 150 000 catastrophic collisions, 314 000 dead and 155 000 destroyed satellites, about 640 000 large pieces |
| 100 | 934 000 satellites destroyed; 3.6 million large pieces live; 80 to 90 thousand small re-entries an hour worldwide (about 4400 glowing at once), against about 50 large ones |
| 365 | 1.07 million satellites destroyed; live pieces down to 1.4 million as they shatter and re-enter |

The cascade runs away. That is the expected outcome for about a million satellites in narrow shells, where each
catastrophic collision between two Starminds makes about 1800 pieces over 10 cm. The 5 million slots are full
from about day 80. One simulated year computes in about 8.5 minutes: days 0 to 50 in 28 s, about 3 s per sim-day
at the busiest stretch (days 60 to 100), about 1.2 s per sim-day after. The working set grows to about 1.9 GB at
day 50, 3.2 GB at day 100 and 4.3 GB near day 350, mostly the event log.

Drawing is cheap: with a full pool the orbit pass costs about 2.2 ms. What slows a player down is the hold while
warping past the computed frontier, and the engine's threads competing for the CPU during the busiest stretch.
The CPU bucket `kessler` (Performance tab, harness `perf`) holds the main thread's share.

## Settings

The settings are in the Missions window's Controls tab, section THE SUN'S FURY (Settings > Constellations has
a pointer to it). Changing the rate, avoidance or horizon re-simulates from the storm.

| UI label | `settings.json` key | Default |
|---|---|---|
| Scenario (a storm on 2037-07-15) <!-- history-ok --> | `kessler.enabled` | on |
| Collision rate (×0.1, ×1, ×10, ×100) | `kessler.rate_multiplier` | 1 (physical) |
| Collision avoidance (0, 50, 90, 99%) | `kessler.avoidance` | 0.9 |
| Simulate up to (5, 20, 50 y) | `kessler.horizon_years` | 20 |
| Dust cloud brightness (×1 to ×1000) | `kessler.dust_gain` | 100 |
| Fireball material colours (Off, Subtle, Vivid) | `kessler.fireball_colors` | 0.8 (Vivid) |
| Small re-entry brightness (Off, ×1 to ×100) | `kessler.small_reentry_gain` | 30 |

Avoidance is the share of encounters between a working satellite and a tracked object (a dead satellite or a
fragment) that the working satellite dodges; dead satellites dodge nothing. The section also shows live counts
(days since the storm, how far the history is computed, dead, destroyed and re-entered satellites, large pieces,
small fragments made, collisions) and, when the player has broken something, "Undo my breakups". The default
bookmark "The Sun's Fury" lands on the storm's first night over Monument Valley, and the Missions window's
Upcoming tab carries a card counting down to the CME. Launches stand down at the CME; see [Launches](launches.md).

The harness has `kessler state | wait [days=] [done] | breakup <sat> [kind=explosion|asat] | collide <a> <b> |
select [k=] | select dead | reentry` (see [Harness](../development/harness.md)).

## Limits

- Orbits do not decay before re-entry: drag sets only the death second.
- Small-small collisions are not modelled; the small fragments are orbit-averaged in longitude.
- Collision avoidance is a constant share, not a manoeuvre.
- Dust is drawn for 3 hours (after that the ring around the orbit is physically invisible); there are no
  impact flashes and no re-entry smoke trails lit after dusk.
- The storm's kill probabilities are a scenario's choice, not a radiation-effects model.

## Where in the code

| File | Function / symbol |
|---|---|
| `src/simulations/Kessler.h/.cpp` | `kessler::World` (`apply()`, `simulate()`, `rollback()`, `checkpoint()`), `breakupCollision()`, `breakupExplosion()`, `engineSample()`, `DebrisGrid`, `decayTime()`, `stormSeedEvents()`, `deadTumble()`, `formationDriftTurns()` |
| `src/simulations/SatelliteSimKessler.cpp` | `KesslerWorker`, `kesslerRestart()`, `kesslerSync()`, `kesslerBreakupInPlace()`, `explodeSatellite()`, `rosterFateAt()`, `kesslerFillDust()`, `kesslerFillFireballs()`, `fbFrag()`, `kesslerGoToNextReentry()`, the cache, `buildKesslerSettingsSection()`, `kCacheVersion` |
| `src/simulations/SatelliteSim.cpp` | the frontier hold in `recordCompute()` |
| `src/simulations/SpaceWeather.cpp` | the scripted storm (`spaceWeatherSetScriptedStorm()`) |
| `shaders/sat_orbit.comp` | dead satellites' tumble and drift, pool records |
| `shaders/sat_rebake.comp` | re-baking pool records' phases with the roster's |
| `shaders/kessler_fireballs.comp` | fireball heads appended to the visible list |
| `shaders/cloud_march.comp` | dust clouds and emissive trails (`dustCloudsCS`) |
| `tools/make_debris_models.py` | the debris kit models |
