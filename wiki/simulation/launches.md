# Launches and the constellations' life cycle

The constellations are not static: every orbital plane is refreshed by launches, its new satellites climb
from a low parking orbit to their slots, and the satellites they replace are lowered and re-enter. This page
covers the schedule (programs, planes, batches, pads), the orbit-raising law, how a train leaves its rocket
and unfolds, payload stacks and fairings, the deployment era that builds the constellations up before 2036,
and how all of it is stored. The rocket's own flight (ascent, staging, booster recovery, upper-stage deorbit,
Starship's return) is on [Rockets](rockets.md); crewed and cargo flights to stations are on
[Crew and cargo flights](crew-flights.md); plumes, flames and pads are drawn as described on
[Launch visuals](../rendering/launch-visuals.md).

## The model in one paragraph

A **program** in `data/launches.json` keeps one or more constellations flying. Each **plane** of a matched
constellation is refreshed once per design life, in **batches** (one launch each) of `payload` satellites.
A batch's arrival time on its slots is a closed-form function of its plane, refresh number and batch
number; the launch is the plane's first pass over a suitable pad before that, moved to another pad or a
later pass if a higher-priority launch wants the same pad within 90 minutes. The rocket deploys a **train**
in a low parking orbit; each member waits there as long as its phase needs, then climbs linearly in radius
and arrives exactly on its slot at the batch's arrival time. At that moment the satellites in those slots
become **retirees**: they are lowered to 200 km over weeks and re-enter. The roster never changes size: a
roster satellite is a position in a plane, and its occupant changes at each refresh.

!!! warning "Invariant"
    Everything here is a **pure function of sim time** (and of the Kessler event log, see below). Batches,
    launch times, pads, member schedules and records are built from hashes and closed forms, never from a
    history. Time warp, reverse playback and bookmarks see the same sky. Caches (`LaunchState::batches`,
    candidates, rival buckets) only memoise these functions; any new state must keep that property.

## Programs, planes and batches

### Programs

| `launches.json` program | Constellations | Vehicles | Payload | Design life |
|---|---|---|---|---|
| Starlink direct-to-cell | Starlink Gen3 Direct-to-Cell | Starship | 36 | 5 y |
| Starlink | Starlink Gen2, Gen3 Broadband | Starship | 30 | 5 y |
| Starlink Gen1 | Starlink Gen1 | Falcon 9 | 31 | 5 y |
| Starmind | SpaceX Starmind | Starship | 8 | 6 y |
| Amazon Leo | Amazon Leo | New Glenn, Vulcan, Ariane 64, Falcon 9 | 45 | 5 y |
| Guowang | Guowang | Long March 5B / 8A, Zhuque-3 | 10 | 6 y |
| Qianfan | Qianfan | Long March 6A / 8A, Zhuque-3, Tianlong-3, Pallas-1 | 18 | 6 y |
| OneWeb | OneWeb | Falcon 9, LVM3 | 36 | 7 y |
| Reflect Orbital | Reflect Orbital | Falcon 9 | 40 | 5 y |

The crew and cargo programs (ISS, Tiangong, the commercial stations, Progress, Tianzhou, Cargo Dragon) use
the same scheduler with a `crew` block; see [Crew and cargo flights](crew-flights.md).

A program matches every constellation whose name contains one of its `match` strings. A constellation needs
at least two satellites to be scheduled this way (a station, a constellation of one, is only served by crew
programs).

### Planes

`buildPlanes()` groups each matched constellation's roster satellites by right ascension of the node
(1 deg), inclination (0.25 deg) and altitude (5 km). A "plane" can therefore hold members up to about a
degree of node apart (Starmind's per-cluster jitter). Each plane is sorted by argument of latitude, so a
batch is a run of neighbouring slots. Per plane:

- a hashed **phase** \( \phi \in [0, 1) \) of the refresh cycle, so planes do not refresh in step;
- \( n_b = \lceil N_{\text{plane}} / p \rceil \) batches per refresh, where the payload \( p \) is rounded
  down to a whole number of formation clusters (a Starmind ring of 8 always flies together);
- a batch gap \( g = \min(\texttt{batch\_gap\_days}, L / n_b) \).

### Arrival times

With design life \( L \) (the program's `life_years` times the **Satellite design life** setting), refresh
\( k \) and batch \( b \):

\[
t_A(k, b) = (k + \phi)\,L + b\,g .
\]

The nominal launch is \( t_A - T_r \), with the raise time \( T_r \) hashed in `raise_days`. A launch whose
deployment would come later than half a day before \( t_A \) does not fly ("launch window too late").
Changing the design-life setting reshuffles the whole schedule.

### Choosing a pad and a time

`candidate()` picks a vehicle (the program's list, in a hashed order, skipping vehicles not yet flying) and
a site (the vehicle's list, in a hashed order, skipping sites not yet online). For each site,
`nextCrossing()` finds the next moment after \( t_A - T_r - (t_{\text{SECO}} + t_{\text{deploy}}) \) at
which the plane passes over the pad. A plane passes over a site's latitude twice a day; only the pass whose
prograde ground azimuth lies in the site's `azimuth` range counts (range safety), and a site the plane never
reaches (its latitude above the inclination) is skipped. The first site that works is the candidate.

**Pad deconfliction** (`buildBatch()`): the other launches whose *candidates* are at the same pad within
90 min, and whose arrival times are plausible for that window, are this launch's rivals. Each launch has a
hashed priority; its **rank** is the number of rivals with a higher priority. Rank 0 keeps its candidate.
Otherwise the launch takes the (rank mod \( n \))-th of the vehicle's pads that reach the plane, counted
from the pad after its own in the vehicle's site list, and then rank / \( n \) further passes there, each at
least 90 min later. Because rivals are judged by their candidates rather than their final times there is no
recursion, and the result is still a pure function of time. The rivals are looked up in per-site, per-day lists
of candidates (`rivalBucket()`, cached in `LaunchState::rivalIdx`). The batches built in one frame first gather the
day lists they will need and build each once, in parallel: one list per thread when there are many, a single list
split over the threads when there are few (at 1 month/s to 1 year/s one list at a busy Starship pad covers about 2000
planes). Either way a list holds the same entries in the same order. Fast-track visiting vehicles keep their pass,
since their phasing to the station depends on it.

!!! note
    Adding or removing a site or vehicle re-hashes every launch's pad. Bookmarks that pin a launch time must
    be re-shot after such an edit.

### Recovery choice

Each batch also decides how its booster comes home (`planRecovery()`): a tower catch, return to a landing
zone, a droneship downrange or a ground landing, chosen from the vehicle's `booster` mode and the payload
mass (`sat_mass_kg` × payload or `payload_mass_kg`, against the vehicle's `rtls_max_kg`). The flight itself
is on [Rockets](rockets.md).

## The train: phasing and orbit raising

After insertion the upper stage coasts in the parking orbit (radius \( r_p \), `park_km`) and deploys at
\( t_D = t_L + t_{\text{SECO}} + \) `deploy_after_seco_s`. For a member bound for a slot of radius
\( r_s \) and mean motion \( n_s \) (the slot's own rate, J2 included), with \( T = t_A - t_D \):

1. **Ramp length.** The climb lasts \( T_{\text{ramp}} = T\,(0.55 + 0.25\,h) \), \( h \) hashed per
   member, and is linear in radius from \( r_p \) to \( r_s \).
2. **Ramp phase.** The angle travelled during a ramp whose rate goes from Kepler's at \( r_p \) to the
   slot's \( n_s \) is closed form (`launch::rampPhase()`): with \( a(\tau) = r_p + k\tau \) and a rate
   factor \( f \) linear in \( a \),
   \[
   \Theta(\tau) = \int_0^\tau f\,\sqrt{GM}\,a^{-3/2}\,d\tau' =
   \frac{\sqrt{GM}}{k}\Big[2A\big(a_0^{-1/2} - a^{-1/2}\big) + 2B\big(a^{1/2} - a_0^{1/2}\big)\Big],
   \quad f = A + B a .
   \]
3. **Wait.** In the parking orbit the member gains on its slot at \( \Delta n = n_p - n_s \). The lag at
   deployment (the slot's angle less the dispenser's, plus a small per-member stagger) is advanced by whole
   turns until it exceeds a quarter-day's gain, and the wait \( s = (\text{lag} - \text{ramp lag}) / \Delta n \)
   is chosen so the climb ends exactly on the slot at \( t_A \).
4. **Residual.** If the required wait does not fit in \( T - T_{\text{ramp}} \), the wait is clamped and the
   remaining angle \( \varepsilon \) is spread over the raise by a smoothstep, so the member still arrives on
   its slot with zero relative speed.

**Formations** raise as one group: every member of a cluster takes its leader's wait, ramp and residual, so
a Starmind ring of 8 (one Starship launch) climbs as a ring, the eight within a few km of each other, and
arrives in its formation.

A first fill (below) replaces nobody; a lattice refresh's members arrive in occupied slots, whose old
satellites become retirees at \( t_A \).

### Leaving the stage

Members leave the dispenser one after another, `deploy_interval_s` apart (the stack's own value when it has
one). Each starts **at the dispenser** (Starship's [payload door](rockets.md#ascent-to-the-parking-orbit), 31.1 m up the ship on its leeward side; a
fairing vehicle's stack slot or dispenser point) with the dispenser's velocity plus a separation push.

For the first 6 h each member flies **free**: Clohessy-Wiltshire motion about one reference orbit for the
whole train, the upper stage's parking circle (`launch::cwState()`, curvilinear: \( x \) radial, \( y \) arc
length, \( z \) cross-track):

\[
\begin{aligned}
x &= (4 - 3c)x_0 + \tfrac{s}{n}\dot x_0 + \tfrac{2}{n}(1 - c)\dot y_0,\\
y &= 6(s - n t)x_0 + y_0 - \tfrac{2}{n}(1 - c)\dot x_0 + \tfrac{4s - 3nt}{n}\dot y_0,\\
z &= c\,z_0 + \tfrac{s}{n}\dot z_0, \qquad c = \cos nt,\ s = \sin nt .
\end{aligned}
\]

Every member gets the **same in-plane push** (`releaseInPlane()`): 0.97 of the separation speed up and 0.24
back (retrograde) for Starship's door, a flat stack and a dispenser point; 0.25 up and 0.97 forward for a column.
Members differ only across the plane (a column's sideways exits, plus a small golden-ratio-stepped spread), and
each gets an along-track trim of \( -2 n x_0 \) that cancels the secular drift its radial offset would cause.
Their relative motion is then one trajectory shifted in time, which never closes below the chord it leaves with.
The deploy interval is raised until consecutive members stay at least 30 m apart over an orbit
(`kReleaseGapM` over the trajectory's least speed, `releaseMinRate()`):

| Release | Push | Interval |
|---|---|---|
| Starship door (Starmind, Starlink) | 0.7 m/s | 45 s |
| Falcon 9 dispenser point | 0.45 m/s | about 67 s |
| Starlink Gen1 flat stack | 0.3 m/s | 100 s |
| Guowang column | 0.4 m/s | 75 s |
| Qianfan flat stack | 0.35 m/s | about 86 s |

!!! warning "Invariant"
    The push from a stage is never purely radial. A radial impulse is, in the Clohessy-Wiltshire solution, a
    closed 2:1 ellipse: every satellite would come back to the door it left one orbit later, and every orbit
    after. The retrograde part makes the train drift ahead of the stage, about
    \( 6\pi\,|\dot y_0| / n \approx 2.8 \) km an orbit at Starship's 0.7 m/s, so from three-quarters of an
    orbit on it stays more than a kilometre from the ship (`launch trace` reports this as
    `min_stage_gap_after_orbit_m`).

A Starmind train therefore leaves as one clean string along one arc, curving up and back from the ship and
drifting ahead of it. After 6 h the member's own propulsion takes over: its offset from its own phased path eases out by a
smoothstep until two days after release.

!!! warning "Invariant"
    A member's GPU record must be the orbit it is really on. `memberRecord()` takes the velocity as the
    numerical derivative of `memberState()` (so every eased offset is included), removes the sun-synchronous
    node rate from it and stores it as the record's own node rate, and computes the epoch phase with the mean
    motion **as the GPU's float holds it**. With any of these missing, the GPU's Kepler propagation drifts
    tens to hundreds of metres between rewrites, and a member jumps at each rewrite.

### Retirees

A retiree's radius ramps linearly from its slot to 200 km between \( t_A \) and its re-entry time
\( t_A + T_d \), \( T_d \) hashed in `deorbit_days`, with the phase again from `rampPhase()`. At
\( t_A + T_d \) it re-enters as a fireball, drawn by the same model as the Kessler re-entries (see
[The Sun's Fury](kessler.md)).

## Unfolding after release

A satellite leaves its dispenser **stowed** and unfolds on a timeline its model declares
(`"deployment": {"unfold_delay_s", "unfold_s"}`, start jittered ±15 % per member). Stowable components carry
`"stow"` (packed toward a plane along an axis to a fraction of their extent, over a window `phase` of the
unfolding progress \( P \)) and, for parts that fold flat, `"fold"` (a turn about a hinge line); a station
module can carry `"since"` (absent before that date). The fields are documented on
[Satellite models](../modding/satellite-models.md).

| Model | Starts after release | Lasts |
|---|---|---|
| Starmind AI1 | 600 s | 600 s (radiators first, then trusses and wings) |
| Starlink V2 Mini, DTC, V3 | 900 s | 240 s |
| Starlink V1.5 | 600 s | 300 s |
| Guowang, Qianfan | 300 s | 600 s |
| Amazon Leo | 300 s | 480 s |
| OneWeb | 240 s | 420 s |

The photometry follows the unfolding: each lobe's area is scaled by the share of its stowed parts,
\( A' = A\,(1 - k\,(1 - \text{smoothstep}(p_0, p_1, P))) \), identically on the GPU and in the CPU evaluator.
`"fold"` is mesh-only. Once unfolded, a raising member flies **open book**: every joint at its rest angle,
the arrays flat with the bus and edge-on to the flow. That is the low-drag configuration that makes fresh
trains bright. At arrival the slot's operating attitude takes over.

## Payload stacks and fairings

A program's `"stack"` says how its satellites ride:

| Layout | Used by | Geometry | Exit |
|---|---|---|---|
| `pez` | Starmind, Starlink, direct-to-cell on Starship | flat across the ship at the door | out of the door, up and a little back |
| `flat` | Starlink Gen1 on Falcon 9 (2 × 16), Qianfan (2 × 9) | nadir face down, one or two a tier | up and a little back, off the stack's side |
| `column` | Guowang (two tiers of five) | around a dispenser column, nadir face out | radially |

Payloads per launch follow from what fits: eight Starmind satellites (each bus folded in half, stowed
6.2 × 5.0 × 2.1 m) fit Starship's 8.2 × 22 m bay. `tools/make_rocket_models.py` measures every model stowed,
derives the stack numbers (`pitch_m`, `offset_m`, `member_center`, `column_radius_m`), checks the stack fits
every fairing the program flies, writes the dispensers, and with `--check` fails when `launches.json`
disagrees. Amazon Leo and OneWeb have no stack (no column of their stowed models fits the fairings); their
satellites appear at the dispenser point, stowed.

Stacked satellites are drawn on the bare upper stage until their release, top tier first. Each leaves its
slot in the stack's attitude with a 0.2-0.8 deg/s tip-off rate (half that from Starship's door) and slews
to its attitude law over 180 s, starting 60-120 s after release. This attitude override exists only in the
CPU evaluator and the meshes; the GPU's point keeps the attitude law.

A vehicle with a fairing drops it when its upper stage passes `fairing_sep_km` (110 km; crewed shrouds
85-95 km). The two halves leave at 1.8 m/s outward, tip outward at 9-13 deg/s and fall under gravity and
drag (ballistic coefficient 60 kg/m², exponential air rotating with the Earth, RK4 at 1 s) until they are
below 60 km on the way down. They are selectable objects. The stage's model then switches to its bare
version (`rocket_<vehicle>_upper_bare.json`).

## The deployment era

With **Deployment era (2019-2036)** on (default), the constellations are built up over the late 2010s to
2036 instead of standing complete at every date.

- **First fills.** A program's `deployment` block (one or more, each optionally narrowed by `match`) gives
  `start`, `complete` and `present_2026`, the number up at `deployment_reference`. Its satellites arrive along
  a logistic S-curve of steepness `steepness` (default 8), normalised to 0 at `start` and 1 at `complete`,
  whose midpoint is solved so the curve passes through the reference count (`launch::SCurve`). Each plane's
  batches get **first-fill** launches in a fill order: shells one after another (by altitude within each
  constellation, overlapping by `shell_stagger`), within a shell batch 0 of every plane, then batch 1, and so
  on. Batch \( j \) of the order arrives when the cumulative share reaches its midpoint.
- **A slot does not exist before its first fill arrives.** Its born second goes to the GPU (after the
  Kessler roster words); `sat_orbit.comp` hides it before then, it cannot be selected or exploded, and the
  [Kessler world](kessler.md) leaves it out.
- **Refreshes** keep the design-life lattice, but a lattice refresh of a batch flies only once the first
  fill is at least `min_first_life` (0.25) × the life old.
- **Early vehicles.** Before a block's `early_until`, its `early_vehicles` fly instead (Starlink Gen2 and
  direct-to-cell on Falcon 9 before Starship), without a stack.
- **Sites and vehicles** fly from their `online` dates; a site's `online_for` gives a later date for one
  vehicle (Kennedy 39A's Starship tower). The pads drawn on the ground also follow `pad_history`; see
  [Launch visuals](../rendering/launch-visuals.md).
- **Groups without launches** (`roster_history`): stations by `born` / `gone` dates, a fuel-depot count,
  and the Space Junk field by a piecewise-linear `count` (members appear in a hashed order, so the field
  fills in everywhere at once). The 2020 V1.0 and VisorSat benchmark shells carry `"benchmark": true` and
  are left out of the era's sky unless **In the deployment era** (Settings > Constellations > BENCHMARK
  SHELLS) is on, since Starlink Gen1's first fills stand for the same satellites.

The Missions window's Ongoing tab opens with BUILD-OUT: per cohort, satellites deployed of the target and
launches a day (the first-fill rate from the curve's slope plus the refreshes of planes past their first
life); see [Missions](../using/missions.md). The ambience counts only the satellites present.

Over the scenario the build-up peaks around 2032 at roughly a hundred launches a day, almost all Starship
launches of Starmind; at the 2036 roster the Starmind refreshes alone are about 50 Starship launches a day.

Off, every roster satellite is present at every date and only the refresh lattice flies.

## The Sun's Fury: launches stand down

When the [Kessler scenario](kessler.md) is on, no launch flies from the coronal mass ejection's arrival on.
By default this is permanent: no operator sends replacements into shells being ground into debris.
**After the Sun's Fury: Resume** brings launches back three weeks after the storm. Either way:

- a slot killed by the storm or the cascade before its batch's arrival gets no replacement and no retiree;
- trains and retirees are not engine objects: they never collide, whatever the debris.

With the default (stand down for good) the stand-down is also a boundary on the schedule walk. A batch's lift-off
is never earlier than its arrival less the program's longest raise and the catalog's longest vehicle lead (the
candidate search runs forward from there, and pad ranks only move a launch later), so every batch arriving after
the CME plus that margin is stood down whatever its pad: the walk's window ends there, and such a batch is neither
built nor walked. A batch built anyway (by the Missions window's searches, `launch why`) is stood down before its
candidate pass is searched, and one whose candidate already lifts off after the CME is stood down before its rivals
are looked up. The schedule before the CME is unchanged by this (`SATLIGHTSIM_LAUNCH_STANDDOWN_SKIP=0` turns the
shortcut off for the A/B: `launch list`, `padstats` and `why` match).

A roster satellite whose replacement would launch after the storm reads "Replacement: none scheduled" in
its mission facts. Crew flights follow the same stand-down; their behaviour when a station itself is lost is
on [Crew and cargo flights](crew-flights.md).

## Records and indices

Satellite indices are laid out as:

| Range | What |
|---|---|
| `[0, activeSatCount)` | the roster |
| next `kLaunchCap` = 131072 | launch records (trains and retirees; reserved chunks at the top for the selected / followed transient and the rocket, pad, ship and stack meshes) |
| from `poolIdx0()` | the Kessler debris pool |

Launch records are 64-byte `GpuKepOrbit`s in the Kessler buffer from `kLaunchWord0`, in chunks of 8 slots
per batch and kind. Each holds the member's **osculating** Kepler elements at the write time; the GPU carries
it on as a Kepler orbit until the next rewrite. A batch's records are rewritten together every 20 s of sim
time, every 2 s while its train is leaving the stage (and 10 min after) or when a member has a phasing
residual, and whenever one of its retirees' re-entries passes. At most 1200 records are written a frame,
round-robin. Two flag bits ride in the record's type word: bit 25 (joints at rest: the open-book pose) and
bit 26 (still unfolding; the progress rate and phase sit in the tumble words).

A member's path is not a Kepler orbit (the climb's ramp, the ease from free flight into its slot's path, up to
about 0.05 m/s² of effective thrust), so a record drifts off it between rewrites, and under time warp the write
budget lets records age: at 1 min/s about 20-40 s of sim time (tens of metres), at 5 min/s a few minutes
(hundreds of metres to a kilometre). Two rules keep that out of anything seen close up:

- **The CPU never reads the written record.** `launchOrbitElems()` rebuilds a live member's osculating elements
  from its exact path at the current sim time each time it is asked (`memberRecord()`), so its mesh, Go to, Track
  and the info readouts follow the path to under a millimetre at any warp.
- **Meshed members are rewritten every frame.** The slots drawn as meshes last frame (`meshLaunchSlots`, at most
  256) get their GPU record rewritten ahead of the round-robin, so a mesh's own point, bloom and glare stay on it.

Far points keep the round-robin's record error: a kilometre at 300 km is a couple of pixels in a wide view. Harness
`launch jitter` reports the record errors of the live records by age (`live`, `record_err_m_max`,
`meshed_record_err_m_max`, `mesh_err_m_max`); `launch exact off` reads the records instead, for an A/B.

!!! warning "Invariant"
    Never assume `satOrbits.size() == activeSatCount + pool size`, or that an index past the roster is
    debris. Per-constellation scans run over `activeSatCount`; empty launch entries have
    `constIdx = UINT32_MAX`.

## Settings

| UI label | `settings.json` key | Default |
|---|---|---|
| Launches and retirements | `launches.enabled` | on |
| Deployment era (2019-2036) | `launches.deployment_era` | on |
| Satellite design life | `launches.life_scale` | ×1 (choices ×0.5, ×1, ×2, ×4) |
| After the Sun's Fury | `launches.after_storm` | Stand down (0); Resume (1) |
| In the deployment era (benchmark shells) | `launches.era_benchmark_shells` | off |

These controls are in **Settings > Launches**, section LAUNCHES, with the "Watch the next launch", "Watch a
twilight launch", "Watch a Starship catch" and "Watch a retired satellite re-enter" buttons (each lands on the
event as the Missions window's Watch does; see [Missions](../using/missions.md)) and the next launch, the launches
in the next 24 h and the satellites climbing and deorbiting. The graphics sections of the same tab (plumes, flames,
rocket light, pads) are on [Launch visuals](../rendering/launch-visuals.md#settings), the Sun's Fury on
[Kessler syndrome](kessler.md#settings).

## `data/launches.json` at a glance

| Section | Holds |
|---|---|
| `sites` | `id`, `name`, `lat`, `lon`, `azimuth` [min, max] (deg clockwise from north, may wrap), `online`, `online_for`, `pad_kind`, `pad_heading_deg`, `pad_history`, `fictional` |
| `vehicles` | `sites`, phase times (`meco_s`, `meco_alt_km`, `meco_downrange_km`, `seco_s`, `deploy_after_seco_s`), release (`deploy_interval_s`, `sep_speed_mps`, `sep_spread_mps`, `door_m`), propulsion (`flow*_kg_s`, `exhaust*_m_s`, `flame*_mag_100km`, `soot`), recovery (`booster`, `droneship_km`, `rtls_max_kg`, `landing_ships`, `mount_m`, `catch_m`, `leg_m`), fairing (`fairing_sep_km`, `payload_base_m`, `fairing_center_m`), `spacecraft_dock_m`, `upper_stage_deorbit`, `upper_stage_mass_kg`, `vent_chance`, `online` |
| `programs` | `match`, `vehicles`, `life_years`, `payload`, `park_km`, `raise_days`, `deorbit_days`, `batch_gap_days`, `sat_mass_kg` / `payload_mass_kg`, `stack`, `deployment`, `crew` |
| top level | `deployment_reference`, `min_first_life`, `roster_history` |

The catalogue holds 63 site entries: the real ones (Starbase and Starbase Pad 2, Kennedy 39A and LC-49, Cape
Canaveral SLC-37 / 40 / 41 / LC-36, Vandenberg SLC-4E / 6, Wallops, Kourou, Wenchang, the Hainan commercial
site, Haiyang, Taiyuan, Jiuquan's pads, Xichang, Sriharikota, Tanegashima, Baikonur, Vostochny, Mahia,
SaxaVord, the planned Starbase Louisiana pads A-E) and 25 marked `"fictional": true`, the scenario's 2030s
Starship build-out (Louisiana F-J, Gulf, Pacific and Cape sea platforms). Vehicle figures are public
approximations. Launch-azimuth ranges limit which planes a site can serve: Starship's sun-synchronous
Starmind planes, for instance, pass over Vandenberg and the Louisiana pads (azimuth to 200 deg) at nearly
the same times each day, which is why deconfliction spreads them over many pads.

## Checking it

The harness reports and tests all of this; see [Harness](../development/harness.md). The useful entry
points: `launch check` (arrival and release continuity, stage turn rates), `launch why [hours=]` (why
batches do not fly), `launch list`, `launch jitter` (record jumps at rewrites, CPU and emulated GPU),
`launch formation` (a ring's shape through the climb), `deploy` / `deploy stats` (the build-out),
`launch padstats` (same-pad launches within 90 min), and `tools/harness/mission_ops.py`, which traces the next
launch of every vehicle-program pair at three epochs and judges each phase against one threshold table.

Measured with `launch jitter`: at each rewrite a member's record moves by under 0.1 m in double precision and
about 1 m in the GPU's float propagation (its floor).

## Cost

The CPU bucket `launches` (Performance tab, harness `perf`) holds the schedule walk, the record writes and
the visuals. With about 5300 live batches (the Twilight Starship bookmark, 1600×900) it is about 1.3 ms a
frame at 1x and about 3.4 ms at 1 h/s, where plume integration for newly visible launches dominates.
`launch state` reports the split. Batches are built in parallel, with a per-frame cap (16-400) that follows
their measured cost so a load or a fast warp does not stall a frame, and the window of batches that matter is
re-walked only when sim time moves more than 300 s.

**Background builds.** When the walk finds more than 48 batches missing, or more than four past 1 h/s of warp, or
any after a time jump of an hour or more (a bookmark Go, a typed time, a Watch), it sorts them by urgency (the
launches nearest now first), builds the four most urgent itself (none at a fast warp or after a jump, where the
first builds of a day would look up cold rival lists on the main thread) and hands the rest to one background
thread at below-normal priority on all cores but two. The rockets of the moment after a jump appear a few frames
later. That thread builds them in chunks, and each frame the main thread moves the finished prefix
into the batch cache within about 2 ms. The batches are identical to those built on the main thread
(`SATLIGHTSIM_LAUNCH_BG=0` turns the background build off for the A/B: `launch list` and `padstats` match). A jump
to a busy deployment-era day (about 9700 batches) fills in about a second, with the launch CPU near 3.4 ms a frame
over the first ~40 frames; a bookmark Go fills in under 40 frames at 1.3-2.3 ms. Plume candidates below a quarter of
the last frame's priority cut are not finished, which keeps the Twilight Starship bookmark near 1.5 ms at 1x.

## Where in the code

| File | What |
|---|---|
| `data/launches.json` | sites, vehicles, programs, deployment blocks, roster history |
| `src/simulations/Launches.h/.cpp` | the catalogue loader, `SCurve`, `rampPhase()`, `cwState()` / `cwFree()`, rocket tracks, exhaust parcels |
| `src/simulations/SatelliteSimLaunches.cpp` | `buildPlanes()`, `candidate()`, `nextCrossing()`, `rivalBucket()`, `buildBatch()`, `memberState()`, `memberRecord()`, `memberUnfold()`, `stackSlot()`, `ensureFairing()`, `launchUpdate()`, `rosterLifeEnsure()`, `deployRows()`, `buildLaunchSettingsSection()` |
| `src/simulations/SatModel.cpp` | `stow`, `fold`, `since`, `deployment`, `docking_ports` |
| `shaders/sat_orbit.comp` | launch records, born / gone seconds, open-book and unfolding flags |
| `tools/make_rocket_models.py` | stacks, dispensers and fairing fit checks |
