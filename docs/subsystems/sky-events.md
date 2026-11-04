# Sky events, Moon-base glints, meteors, comets, inertial lock

The Sky events window and its event kinds, the lunar bases' glints, meteor showers, comets and their tails, the inertial camera lock.

Part of the subsystem notes (`docs/subsystems/`, map in `AGENTS.md` §4). Moved verbatim from
`CLAUDE.md` on 2026-11-03. Do not read this file whole: `python tools/outline.py docs/subsystems/sky-events.md`
lists its sections and topics with line numbers. New notes go next to the section they belong to.

## Subsystem: Sky events window (2026-10-27)

`SkyEvents.h` (the event record and the search context), `SatelliteSimSkyEvents.cpp` (the kinds, the worker, the
window, the Watches, the harness), `skyPlanetGeoAt` (SatelliteSim.cpp: planetStates' ephemeris as a pure function of t).
The crescent-Moon button `TimeSkyBtn` beside Weather on the right HUD panel (`pixel--sky-events.png`, icons 32-36 from
`tools/make_icons.py`: `sky-events`, `sky-solar`, `sky-lunar`, `sky-conjunction`, `sky-occultation`), key K
(`KB_SKY_EVENTS`), window id 13, `skChrome`; harness `ui open skyevents`, `skyevents ...` (docs/HARNESS.md).
- **It is the shell later features plug into** (the user's plan: Moon-base solar-array glints, a scripted Betelgeuse
  supernova on 2037-12-25, meteor showers with their own particles). **A kind is one row of `SkyEventsFriend::kinds`**
  (`SkyKindDef`): id (harness / json name), card title, icon, accent, SEARCH (`void (SatelliteSim::*)(const
  sky::SearchCtx &, std::vector<sky::Event> &) const`: append every event in [ctx.t0, ctx.t1], a pure function of the
  range, run on a worker: read only ephemeris functions, constants and the context — never members the main thread
  writes; `ctx.cancelled()` between steps), WATCH (`bool (SatelliteSim::*)(const sky::Event &, std::string &msg)`), the
  card's two text lines, the "none" status and the Watch tooltip. **To add a kind:** append a `sky::Kind` before
  `kKindCount`, declare its search + Watch in SatelliteSim.h's block, add the row (the static_assert counts them), an
  icon in make_icons.py + the icon list in `loadUiIcons`. The window, worker, upcoming list, json and harness need no
  change. `sky::Event` carries kind, the moment `t`, the window `[t0, t1)` a Watch may land in, bodies `a` / `b`
  (0-5 PlanetId, `kBodyMoon`, `kBodySun`), `sepDeg`, magnitudes, `score`, `total` / `annular`, a `site` (Earth-fixed
  unit) and `watchT` / `tAux` (kind-specific). A fixed scripted event (the supernova) is a search that pushes one
  record when its date is in range.
- **The searches** (all over [base - 4 h, base + 2 y], one kind after another on `skScanJob` (std::async), the next
  started by `skyEventsUpdate` when one comes in; restarted (the running one cancelled and its result dropped) when sim
  time leaves [base - 1 h, base + 30 d]): **solar / lunar eclipses** = `findEclipse` from the range's start, one after
  another (+20 days), the window from `findEclipse`'s own separation / limit (`eclipseSepLim`, keep in step); a solar
  eclipse's place is the central line (`axisPoint`, the harness `eclipse solar` placement) while the Sun is >= 15 deg up
  there, else the place with the Sun >= 15 deg up that sees the most of it (`solarSite`, a grid); a lunar eclipse's a
  grid site with the Moon ~40 deg up and the Sun <= -12 deg (`pickSite`). **Conjunctions**: Mercury .. Saturn pairs
  (6-h steps) within 2.5 deg, the Moon and a planet brighter than mag 1.5 (hourly) within 1.5 deg but outside an
  occultation's limit, both >= 12 deg from the Sun, minima refined by golden section; a site where both are >= 10 deg up
  and the Sun <= -6 deg somewhere in the window, else dropped; score = the fainter body's light / separation.
  **Occultations**: Moon-planet minima under the true disc's horizontal parallax + radius; `occultationSites`: where the
  line from the planet through the Moon's centre meets the Earth (every 2 min over +-4 h), the contacts there with the
  DRAWN disc (x "Moon size", 1 near the Sun as updatePositions), kept only if at the Watch moment (a minute before the
  planet reappears at the limb) the place is dark (Sun <= -9 deg, -4 for Mercury / Venus) and the Moon >= 10 deg up — the
  drawn disc made an occultation 2-3 h long at one place at the old 2.5x default (1x since 2026-10-27), and the first cut
  (judged at the central moment) landed with the Moon setting. Searches take ~5 / 8 / 70-90 / 50-65 ms (solar, lunar, conjunctions, occultations) on the worker.
- **Positions are the sim's own**: the Moon of date (satphot::moonGeoEciAt), the planets J2000 (planetStates), so the
  events are the ones the sim draws even where those frames' ~0.5 deg of precession makes them differ from the real sky.
- **ONE Watch per event** (the user rule): `skyJumpTo` (the moment: inside the window, never before t0; 1x, unpaused,
  histories reset), `skyPlaceAt` (the observer on the ground there), `skyAimAt` (the body or the pair's midpoint; FOV:
  solar 16, lunar 12, conjunction 4 x the separation (4-40), occultation 5), the planet selected; the toast and the
  window's status line say what you see. The aim is kept on the bodies each frame (`skyEventsTick`, buildUI after
  `updateTrack`) until the camera is moved (a 0.05-deg difference from what it set), follow / Track / a cinematic, or the
  event ends. The Watches that pick a place (lunar, conjunction, an occultation under cloud) jump the clock FIRST so the
  CPU cloud map's drift (`cloudCoverAtEcef`) is that moment's: a clear sky scores first, then land, then near the player.
- **The window**: a status line, NEXT OF EACH KIND (one fixed-height card per kind: icon, title + "in ..." /
  "HAPPENING NOW", two short lines — a card's text is clipped, and at 1024 x 768 a line holds ~40 characters), UPCOMING
  (always 10 fixed rows: the next events of every kind within 2 years by time, a row under way highlighted). Nothing runs
  while it is closed. Watches are applied after the layout (`skPendingKind` / `skPendingRow`).
- **Measured** (RTX 3070 Ti, 1600x900, harness `hitches threshold 1`, 180 frames): build_ui 0.06 -> 0.13 ms a frame with
  the window open; a Watch 4 ms on its frame (the jump's own scene cost on top); `ui_lowres.py --only skyevents` passes at
  every size. Captures: `harness_runs/sky/r1`..`r5` (untracked).
- Not included (the user's call): ISS / station transits of the Sun or Moon. Open: a partial solar eclipse is lost in the
  Sun's glare (the renderer's); a planet emerging beside a bright Moon is faint (the point photometry's Moon
  suppression); a Watch after the CME (2037-07-15) holds at the Kessler frontier while the cascade computes; clouds can
  still hide an event (solar eclipses take the central line whatever the weather).

- **Round 2 (2026-10-30, issue 20261029_205641): tabs, one-time events, Ongoing, space weather.** The window has two tabs
  (`skTab`: Events, Space weather; harness `ui open skyevents tab=spaceweather`). **Events**: ONE-TIME EVENTS on top, always
  (`skyEventsRefreshSpecial`, `skSpecial[4]` by time: the Great Comet, its meteor storm, the Sun's Fury, the Betelgeuse
  supernova — built once from each one's own search over a window round it, not from the 2-year search, so they show at any
  date), each with its own colour (`Pal::spComet / spStorm / furyBorder / spNova`: a 2-px frame and the title); ONGOING (events
  under way, the one-time ones left out, <= 6); NEXT OF EACH KIND (folded, `uiFold`); UPCOMING (10 rows, one line each). An
  event under way has **Start / End** under its Watch (the clock to its start / to 2 min before its end, 1x: `skyJumpTo`);
  Watch stays the one place-and-aim button. Rows are one helper (`skyEventRow`, `SkRowSpec`), compact by default ("Details"
  adds each row's second line). **Kind `cme`** is now both the Sun's Fury (`Event::a == 1`) and every procedural Earth-directed
  CME storm of G3+ (Kp >= 7: `spaceWeatherCmeArrivals`, the same hashes as the aurora's Kp; window = arrival + 1 day, Watch =
  arrival + 4 h); **kind `cometstorm`** is the Great Comet's meteor storm (below). New icons 44 `sky-cometstorm`, 45 `sky-cme`.
  **Space weather tab** (`SatelliteSimSpaceWeather.cpp`): a 400 x 200 CPU raster in a UI external image slot (the 7th; the
  Kessler tracker's pattern: `spaceWeatherEnsureImage / Raster / RecordUpload`, re-made when time moved 60 s, at most twice a
  second) of both hemispheres' auroral oval as polar plots from above each magnetic pole (colatitude 0-55 deg, magnetic noon at
  the top, midnight at the bottom, rings every 10 deg), land / sea shaded by the Sun (the night side dark), the oval =
  `auroraOvalWeightCpu(auroraGpu_, dir)` — the shader's band without the noise, from this frame's `spaceWeather_` (Kp, the
  substorm); your place as a cyan dot. Below: Kp / storm scale / oval edge / solar cycle tiles, the drivers (quiet, streams,
  CME), the CME storm and substorm in progress, "Watch the aurora" (`spaceWeatherWatch(t)`: a grid of places with the Sun <= -12
  deg (then -6, then any), score = the oval's weight - city light - cloud + land, the camera toward the magnetic pole, 28 deg
  up, FOV 90) and NEXT STORMS (the next five `cme` events, each one Watch). Harness `skyevents aurora [t=]`, `skyevents special
  <0-3> [watch=1]`; `skyevents state` -> `special`, `ongoing`, `space_weather`. Script `scripts/sky_space_weather.satcmd`.

## Subsystem: Moon-base glints (2026-10-27)

`LunarBases.h/.cpp` (app-independent: the bases, the Moon's IAU body frame as a pure function of t, the array's
photometry, the best place on Earth and the windows search), `SatelliteSimMoonBases.cpp` (the point in the sky, harness
`moonbase`), the sky-event kind `moonglint` in `SatelliteSimSkyEvents.cpp`, `data/lunar_bases.json` (copied next to the
exe, packaged), `tools/lunar_glint_tool` (`LunarGlintTool`, EXCLUDE_FROM_ALL: the single-panel check + the windows).
- **The scenario (every value a labelled assumption in the data file):** China's ILRS on the Shackleton south-east ridge
  (88.8 S 123.4 E, the Chang'e-7 target), a generous array of 1 km^2 in 500 rows, built 2032-01 .. 2035-06; NASA's
  Artemis Base Camp where the Shackleton - de Gerlache connecting ridge meets Shackleton's rim (89.684 S 196.739 E, LROC /
  Glaser et al. 2014), 0.1 km^2 in 100 rows, 2032-06 .. 2033-12 (Fission Surface Power carries the rest). Elevations
  1.6 / 1.9 km, horizon dip 1.5 deg, row tilt sigma 0.8 / 0.6 deg, row azimuth sigma 0.3 / 0.25 deg, panel flatness
  0.05 deg, lean 0, glass F0 0.04. Rows appear in order between `online` and `complete` (deployment-era style).
- **Physics:** vertical panels on vertical-axis trackers facing the Sun's azimuth (+ each row's hashed errors). A panel
  facing the Sun mirrors it back toward the Sun's AZIMUTH at elevation 2 lean - e_sun, so the beam reaches the Earth only
  with the Earth at about the Sun's azimuth in the base's sky (the Moon within a few degrees of full) and at that
  elevation (the Moon near a node: the eclipse seasons). Per row: Schlick F(cos i) x A_row x cos i x p(Delta) / D^2 in
  units of the Sun's irradiance (p = the mirrored Sun's disc smoothed by 2 x the flatness: 0.5 erfc((Delta - r_s) /
  (sqrt2 sigma)) / (pi (r_s^2 + sigma^2)), normalised so one perfect panel is the hand formula
  m = -26.74 - 2.5 log10(R A cos i / (Omega_sun D^2))); summed, x the Sun's visible share at the base: its disc over the
  local horizon (-dip) AND past the Earth (a lunar eclipse hides the glint: the first cut had glints during the
  2037-01-31 totality). `glintAggregate` takes the rows' scatter as one Gaussian beam (sigma 2 x the errors, + the disc)
  for the search and the per-frame pre-test; `glintRows` is the measured value (its far tail can hold no row: the windows
  keep only peaks the rows make brighter than the limit).
- **Checked:** `moonbase check` / LunarGlintTool: one flat panel at sun elevations -1 .. 1.2 deg and leans 0 .. 5 deg,
  beam elevation = 2 lean - e_sun exactly, magnitude = the hand formula to 1e-6 mag; 0.5 deg off the mirrored Sun's
  centre nothing.
- **Measured (2036-11-22 .. 2038-11-22, limit mag 3 above the air, from the best place with the Moon >= 10 deg up and
  the Sun <= -6 deg):** ILRS 12 windows, Artemis 7 (jitter x0.5: 8 / 6; x1.5: 15 / 8). All at a Moon phase angle of
  0.4-5 deg, in two seasons a year (January-March and May-July in 2037-38, round the eclipse seasons), 2-11 h each;
  peaks ILRS -1.2 .. -2.4, Artemis 0 .. +1.9 (seen through the air ~0.8 mag fainter at 12 deg up). The best place is
  usually where the Moon is low (the beam grazes the Earth's limb; the base sees the Earth at its horizon).
- **Drawn:** `moonBasesFill` (kesslerFillDust's finish, after the wildfires): per built base with its Sun up, the
  aggregate first (rows only when brighter than mag 14), then a point in kFireBuf as a city light (point, bloom, glare;
  meshPx -1: no trail, manual terrain test only, so the Moon's disc does not hide it): at the base's place on the DRAWN
  disc (moonDir + moonAngR x the body vector: the disc's own sphere, x "Moon size"), clamped to 0.98 of the drawn radius
  (the glints come with the base at the limb, and a point a hair past the per-pixel-refracted edge read as floating),
  refracted (`refractEnu`), extinguished along the line of sight, x (1 - 0.9 dayness), capped at flare 60, the base's
  tint. CPU 0.004 ms a frame with no glint, ~0.09 ms during one (both bases' rows); nothing on the GPU per pixel; no
  shader changed (SKY_LITE untouched).
- **The sky event `moonglint`** (MOON BASE GLINT, icon 37 `sky-moonglint`): `skySearchMoonGlints` = `lunar::findWindows`
  over the search range (30-min pre-test on the mean beam's angle from the Earth, 5-min steps inside, a 5/1/0.25-deg grid
  for the best place; ~3.5 s for 2 years on the worker); `sky::Event::label` carries the base's short name, `a` =
  `sky::kBodyMoonBase0` + its index, `tAux` the Moon's phase angle. Watch: the clock into the window (the peak, or now if
  under way), 1x; the brightest place by the rows, or the clearest place within 1 mag of it when that is under cloud
  (> 0.3 on the CPU cloud map); the camera on the base's drawn place at a 1-deg FOV (`skyBodyEnu` knows the base ids, so
  the kept aim follows it); the toast names the base, the magnitude, the place, the Moon's height and phase angle.
- **Selectable** (2026-10-28, issue 20261028_192740): a click within 8 px of a glint drawn this frame
  (`pickMoonBaseAt`) selects the base (`selectedMoonBase`); the star-like panel (`formatSelectedMoonBaseInfo`, each
  frame: magnitude seen / above the air, arrays, the Moon's phase angle, the glint window from `lunar::findWindows` over
  -1 .. +3 days, cached; rows lit) has Track (`updateTrack` aims at `moonBaseDrawnEnu`). It gives way to any other
  selection. The pick chain (comet, glint, planet, star, satellite) is `pickAndSelectAt` (mouse, the select key,
  harness `select pick`); harness `select moonbase [<i>|<name>]`.
- Settings (no UI): `moon_bases.enabled` (true), `moon_bases.tilt_jitter_scale` (1, x the rows' errors),
  `moon_bases.event_mag_limit` (3); a change of the last two re-runs the sky-event search.
- Harness: `moonbase [state] | windows [days=] [limit=] [jitter=] | check` (docs/HARNESS.md);
  `tools/harness/scripts/moon_glints.satcmd`. Captures: `harness_runs/mg1`, `harness_runs/mg2run` (untracked).
- Open: the local terrain horizon is a single dip angle (no LOLA horizon mask: a ridge toward the Earth would hide it);
  the base sits on the smooth sphere + its elevation (the drawn limb's LOLA relief is not consulted); no daytime contrast
  model beyond the 0.9 day dimming; the 10-row upcoming list fills with glints in a season (two bases glint together);
  no UI for the settings; panels never lean (lean_deg is in the data for a later design); the Watch's clear-sky choice
  reads the cloud map at the jumped time only.

## Subsystem: Meteors (2026-10-27)

`Meteors.h/.cpp` (app-independent: the shower table, the flux a ZHR stands for, the generator, light curve, fragments,
train wind, colour), `SatelliteSimMeteors.cpp` (the frame's fill, the Watch, the harness `meteors`), the sky-event kind
`meteors` (METEOR SHOWER, icon 38 `sky-meteors`, `skySearchMeteorShowers` / `skyWatchMeteorShower` in
SatelliteSimSkyEvents.cpp). Settings `meteors.enabled` (on), `meteors.rate` (1, x the flux), `meteors.fireball_rate`
(1, x the rate of M <= -4); no UI row yet.
- **The showers** (`kShowers`, the IMO working list, J2000): Quadrantids, Lyrids, Eta Aquariids, Southern Delta
  Aquariids, Perseids, Draconids, Orionids, Southern and Northern Taurids, Leonids, Geminids, Ursids + the sporadic
  background (row `kSporadic`, ZHR 10). Each: peak solar longitude, ZHR, the profile's slopes ZHR x 10^(-B |dLam|)
  (B- / B+, the Geminids' asymmetric; the Perseids add a broad ZHR-22 component, B 0.05), population index r (mag 2 ..
  6.5) and a bright-end index rb (brighter than mag 2; the Taurids' 1.35 makes them fireball-rich), entry speed, radiant
  RA / Dec + drift per degree of solar longitude, the radiant's spread. The solar longitude is the Sun's ecliptic
  longitude from `sunDirEciAt`. **APPEND rows before `kSporadic` only by moving it** (it is the last row; the generator's
  seeds take the row index, so a reorder reshuffles every meteor).
- **Rates**: the flux brighter than absolute mag +6.5 per km^2 per hour = ZHR (13.1 r - 16.5)(r - 1.3)^0.748 / 37200
  / 1000 (Koschack & Rendtel 1990), x sin(the radiant's elevation) at each cell (the horizontal surface's share), x the
  cumulative magnitude distribution `magFrac` (r^(M - 6.5) above mag 2, x rb^(M - 2) below). Sporadic radiants are drawn
  per meteor from the apex (Sun-centred ecliptic longitude 270, ~58 km/s), helion (340) and antihelion (200) sources
  (~28 km/s), accepted by their elevation (the cell's rate x 4 to compensate): the dawn-heavy diurnal cycle falls out.
- **Generation is a pure function of sim time** (`meteors::generate`): six absolute-magnitude bands (edges -14, -8,
  -4, -1, 2, 4.5, 6.5), each with its own time bin and longest age (bolides 120 s / 240 s, fireballs 30 / 60, -4..-1
  8 / 16, fainter 2 / 8); Earth-fixed 1-deg cells (equal-ish area: 360 cos(lat) per latitude band) within the eye's
  reach (a meteor 120 km up on its horizon) and the band's visibility distance (100 km x 10^((cull + 1 - M_bright) / 5),
  cull = the point model's effective limit `pointEffLimitMag()`); a Poisson count per (shower, band, cell, bin) from a
  splitmix hash, each meteor's start, place, magnitude (the band's piecewise power law), speed (+-3%), radiant (the
  shower's + its spread) hashed. The distance cull only drops meteors that could not be seen, and the bands are nested,
  so a meteor exists or not regardless of the observer. Time warp, reverse and bookmarks see the same meteors.
- **A meteor**: begins at 80 + 0.55 v km (v in km/s: Leonids ~119, Geminids ~99, Draconids ~91), ends at 65 + 0.35 v +
  2.5 M km (clamped 20 .. begin - 8: bright and slow ones go deeper, a -10 Taurid bolide ~55 km), straight from the
  radiant, path <= 250 km, duration = path / v (0.2-1 s for most). Light curve 6.75 x^2 (1 - x) (peak at 2/3) + 1-3
  flares for M <= -1 (and some down to 1.5), a terminal burst for bolides (M <= -8). Fireballs (M <= -4) shed 2-5
  fragments after the first flare (sideways 40-260 m/s, decelerating 1.5-7.5 km/s^2, 12-47% of the body's light). A
  persistent train for M <= -5.5: decay 2.5 x 10^(0.2 (-5.5 - M)) s, x2 above 45 km/s (a -10 Leonid ~40 s).
  Colour: white; fast and bright toward the green of Mg / Fe / O, slow toward sodium's orange-yellow; flares whiter.
- **Drawn** (`meteorsFill`, kesslerFillDust's finish after the Kessler fireballs, before the launches): the apparent
  magnitude = M - 2.5 log10(light curve) + 5 log10(range / 100 km) + `atmExtinctionMag` along the line of sight, then x
  `meteorSkySuppress` (a hand-kept copy of updateStars' chain: the night gate with a 9-mag daylight floor, the
  light-pollution dome with its isotropic floor, the Moon) -> effectFlare. So a city or a bright Moon removes the faint
  ones through the point model's own limit, as it does the stars. Heads are kFireBuf points (kessler_fireballs.comp:
  point, bloom, glare); the STREAK is the path swept over the eye's persistence (0.1 s): three emissive Gaussians in the
  dust list, front brightest (0.5 / 0.3 / 0.2 of 0.6 x the flare; the head keeps 0.4) once it is longer than 1.5 px —
  the motion within one frame, not a stepping point. Fragments: heads + a short streak each. The train: ten Gaussians
  along the 78-108 km part of the path already passed, drifting with a hashed wind (25-75 m/s, turning 8-22 deg per km
  of height), widening 12 m/s, green for its first seconds then dim orange, ~7 mag under the meteor + a +2-mag wake over
  its first second. **Budget**: at most 96 meteor Gaussians and never past kMaxDustClouds - 200 (the launches,
  tornadoes, haboobs, wildfire smoke come after), the brightest meteors first; at most 600 heads. **Time warp = TIME-LAPSE**
  (2026-10-28, issue 20261028_220640: they stopped past 1 min/s; `meteors.timelapse`, default on, false = the old rule:
  bands 0-1 only past 2x, none past 1 min/s): past 2x every meteor that burned during the frame's sim-time interval
  [t - dt, t] (dt = this frame's sim step; the first frame or a jump: warp / 60) is drawn as the streak it drew in that
  interval — 1-4 Gaussians by its length on screen, each carrying its piece's light (`meteors::lightIntegral`: the light
  curve integrated in closed form, flares and terminal burst included) / the exposure E = clamp(dt, 0.1 s,
  `meteors.timelapse_exposure_s` 0.1 s = the 1x streak's persistence, so a streak keeps the 1x streak's surface brightness at any rate; 0.5 s and 1 s read too faint: a 4-h run at 1 h/s showed ~6 streaks); 0.7 of it in the streak, 0.3 a point at its brightest piece (bloom, glare; a piece not drawn gives its light to the point; at least half the brightest moment's flare, as the 1x head, so each meteor reads as a real-time one — the streak alone left a Perseid peak at ~1 visible meteor a frame). At 1 h/s each frame shows the minute's meteors as whole
  streaks from the radiant. Trains: each segment's light integrated over the part of its life inside the interval / E,
  drawn at that part's middle (a 20-s train is a fraction of a frame at 1 h/s). No fragments in time-lapse. Bins are
  generated for [t - dt - the band's longest age, t], the missing ones in parallel (`stormCatalog->parallelFor`, from 8);
  the faintest bands are left out while the estimated Poisson draws of the frame's new bins pass 350k (band 0 always;
  harness `meteors state` -> `timelapse`, `timelapse_interval_s`, `timelapse_exposure_s`, `draws_estimate`,
  `band_max`), 192 Gaussians in time-lapse (96 at 1x; a streak with none left is dropped, never drawn as a lone point: hundreds of them read as a star cluster), candidates below 2e-4 of flare not drawn. A bolide replayed at 10x
  shows as before (issue 20261028_191529). Measured (RTX 3070 Ti, 1600x900, the report's place -10 148 2037-07-29 and the
  Perseid Watch, on / off interleaved, Kessler off so time runs): meteors' CPU 0.05 ms (1x), 0.2 (1 min/s), 0.55 (5 min/s),
  2.7-3.9 ms at 1 h/s (~300k draws, 6-10k candidates; generation 2.2-2.8, ranking 0.4-0.6 on the pool), frame time and GPU
  within noise. `generate()` builds its cell list once per call and `meteorsFill` asks runs of up to 16 consecutive bins
  per call (`Meteor::bin` splits them back); a sporadic meteor's source longitude is its bin's (`meteors count` at the
  report's place unchanged: 906 / h). Videos: `screenshots/meteorwarp/` (rep_1h, rep_5m, rep_1m, per_1h, per_5m, stacks). **Train duration** `meteors.train_duration` (0.3,
  x the decay time, no UI; 1 = the old): drawn to 4 decay times (was 5 at x1: a -12 Taurid's train ~270 s; now ~65 s).
  **Determinism vs the eye**: the bin cache (`meteorBinCache_`) serves every eye within `kMeteorCacheRadiusM` (30 km)
  of where it was made, so generation takes `GenParams::eyeMarginM` = that radius (reach from the eye's height + it,
  widened by it): which meteors a cached bin holds no longer depends on where the eye stood (climbing 2 -> 11 km under
  one cache shortened the reach by ~200 km). The report's streak was the -12.2 N Taurid's train 36 s after it; it
  reproduced after rewinds, `time set`, eye moves and 10x replays (`harness_runs/*a38_ab3`).
- **The Watch** (`meteorWatchShower`, the sky event's and harness `meteors go`): hourly times across the peak +-14 h
  (or the next 24 h when the window is under way), a 3 x 4 deg grid of sites with the Sun <= -15 deg and the radiant >=
  25 deg up, scored sin(radiant elevation) x the profile x (1 - 0.75 Moon up x lit) - the night map's lights; then at
  that moment (the cloud map's drift) the 60 best sites re-ranked by the CPU cloud map and land. 1x, unpaused, the camera
  45 deg from the radiant at ~50 deg elevation, FOV 75. Card: "Perseids peak, ZHR 102, <UTC>" / "Moon 23% lit: dark
  skies" (some moonlight from 25%, bright Moon from 60%).
- **Measured** (RTX 3070 Ti, 1600x900, live harness; `harness_runs/meteors_*`, untracked). Rates (`meteors count
  minutes=60`, all sky, at each meteor's peak): the Perseid peak's Watch site (38 N 80 W, radiant 59 deg up, Moon down)
  1784 / h brighter than 6.5, 322 / h brighter than 4, 56 / h in a 75-deg cone round the view; 37 brighter than 0, 7
  brighter than -4, no bolide in the hour; the same moment from Manhattan (the dome dims the zenith 5 mag) 190 / h,
  none brighter than -4; 35 N 110 W on 2037-01-31 (full Moon up, sporadics only) 0 / h, on 01-17 (no Moon) 13 / h; the
  Quadrantid Watch 2776 / h. Cost: the fill 0.06-0.07 ms a frame on the main thread (`kessler` bucket 0.26 -> 0.33 ms),
  0.34 ms on the frame a new 2-s bin is generated (the cache: each (shower, band, bin) once while the eye stays within
  30 km); GPU within noise (8.81 vs 8.80 ms, a few Gaussians); 15 s at 1x: no frame over 50 ms.
- **The Great Comet's meteor storm (2026-10-30, `Meteors.h` `kGreatComet`, `cometStorm()`, `zhrAtTime` / `radiantEciAtTime` /
  `speedKmsAtTime`).** Row 12 of the shower table (id `comet_2037`, "C/2036 X1 meteors", r 2.2, rb 1.5: a large sungrazer's big
  grains, fireball-rich); **`kSporadic` moved 12 -> 13** and stays last, but its generator seed index is pinned at 12 so no
  sporadic meteor changed (the shower rows before it keep their seeds). Activity is a function of TIME, not solar longitude (a
  one-time event): wherever a time is at hand callers use the `...AtTime` forms (the lam forms return 0 for this row). ASSUMPTIONS
  (ours; there is no real comet): the meteoroids are the big grains shed round the sungrazing perihelion. Flown from the comet's
  own orbit (a Monte Carlo of grains emitted from 3 d before perihelion to the encounter, beta 1e-5..3e-3, the tails' ejection law,
  Kepler under mu (1 - beta)), they are still a compact cloud a few million km round the nucleus a month later, and the Earth meets
  it only round its closest approach to the comet (2037-03-17 03:30 UTC, 560,000 km, which is also the least distance between the
  Earth and the comet's orbit): grains within 3e6 km from 03-16 ~06:00 to 03-17 ~19:00, within 1e6 km from 03-16 ~21:00 to 03-17
  ~08:00. Before that the Earth is tens of millions of km from any of it (23 million km from the orbit on 03-08). So the peak is
  the closest approach: ZHR 8000 with 10^(-5 |d|) before (half-maximum 1.4 h before) and 10^(-4 d) after, over a residual of 400
  at the peak rising with the spike and decaying with an e-fold of 75 d after (artistic, kept from the first design's "spikes up
  then gradually slows through 2037": physically the Earth leaves the cloud at ~1.2 million km a day): total 8400 at the peak,
  ~1870 at 00:21 (3 h before; 6500 before the change), ~10 at 13:27 the day before (`t0`, 14 h before; it was 9 days before until 2026-10-30, issue
  20261030_141206: the storm ran while the Earth was still far from the dust), ~265 on 2037-04-17, ~52 on 08-17, passing 10 on
  2037-12-22 (`t1`). The radiant is where the stream comes FROM in the Earth's frame: -(v_comet(at its closest approach, the
  stream's velocity where it crosses the Earth's path) - v_Earth(t)), Earth's velocity from the sim's own Sun; no zenith
  attraction. Result: RA 322 Dec -17.5 at the peak (38 deg from the Sun: a morning radiant), entry 53 km/s (64 on
  04-17, 62 on 08-17), drifting ~1 deg a day. Measured (`meteors count`, 31 S 24 E at the peak): 21,000 meteors/h in view, 180 <=
  -4 in 10 min, 20 <= -8 (bolides). The sky event `cometstorm` (a Watch via `meteorWatchShower`, which for this row accepts a radiant
  10 deg up and a Sun 12 deg down: it is never high in a dark sky) lists it as a one-time event; it is also the way to test the
  Ongoing section (under way for 280 days). `meteors state` gives `radiant_ra_deg / dec / sun_elongation`.
- **Open**: the sporadic rate is probably low in the evening (r 3.0 and the apex below the horizon: 13 / h all sky at
  23:00 local); the train is ten straight segments (kinks show when it twists); no sonic boom; a bolide's light
  (`engineLightAdd`) follows "Rocket light on clouds"; satellites are not refracted and neither are meteors; the
  persistence streak is a fixed 0.1 s.
- Harness `meteors [state]|count|force|go` (docs/HARNESS.md).

## Subsystem: Comets (2026-10-27)

`Comets.h/.cpp` (app-independent: orbits, magnitudes, the catalog, the tail particles), `SatelliteSimComets.cpp` (per
frame, the splat pass and composite, selection, the Watch, harness `comets`), `shaders/comet_splat.*`, `comet_composite.*`, `tools/comet_design.py` (solves
the Great Comet's elements against the sim's own Earth). Sky-event kind `comet` (icon 38, `pixel--sky-comet.png`).
- **Orbits**: a perihelion state (q, e, i, node, argument of perihelion, perihelion time; J2000 ECLIPTIC, AU and days, the
  planets' frame) carried by a universal-variable Kepler propagator (Stumpff functions, Laguerre-Conway iteration), so
  parabolas, hyperbolas and long ellipses alike, a pure function of sim time. The observer is `skyEarthHelioAt(t)`
  (SatelliteSim.cpp: `keplerEclipticPos(kEarthElements)`, the planets' Earth) + `obsECI` turned into the ecliptic, so a
  close comet shows its parallax (~0.7 deg at 560,000 km).
- **The Great Comet of 2037** (`C/2036 X1`, `comet::greatComet()`): a retrograde Kreutz-like sungrazer, q 0.0062 AU
  (1.33 solar radii), i 142 deg, perihelion 2037-02-17 16:49 UTC (behind the Sun at that moment; near the Feb 15 new
  Moon, clear of the 2037-01-31 lunar eclipse; visible in daylight / bright twilight a few degrees from the Sun the days
  round it), then out along the Sun-Earth line: a morning object 20-35 deg from the Sun for weeks (phase ~140 deg: the
  forward-scattering surge, as McNaught), passing **560,000 km from the Earth on 2037-03-17 03:30 UTC** (outside the
  Moon's orbit, over the northern hemisphere, at the Mar 16 new Moon, far from the 2037-07-13 solar eclipse; it sweeps
  ~90 deg of sky in hours, mag ~-6, the coma ~15-20 deg across, the tail ~120 deg), then an evening object fading over
  a month. To move it: edit and re-run `tools/comet_design.py` (it mirrors the Earth elements and checks the closest
  approach by brute force) and paste the elements into `makeGreat()`.
- **Minor comets** (`minorCometsOfYear`): 6 a year hashed from (year, index); 60% long-period (q 0.35-2.75 AU, e
  0.995-1.002, H 4.5-11), 40% Jupiter-family (q 0.9-2.3, e 0.3-0.65, i < 26 deg, H 8-13), named from the half-month of
  perihelion ("C/2036 N2 (Iwamoto)"). Most peak at mag 9-13; about one a year reaches naked eye (2036: Iwamoto 5.7). A
  JFC is a single apparition (no return). The catalog near now (`catalogNear`, +-1.2 years) refreshes every 20 sim-days.
- **Brightness** (`comet::magnitude`): m = H + 5 log Delta + 2.5 n log r_eff, r_eff = sqrt(r^2 + 0.04^2) (a sungrazer
  saturates near the Sun: the Great Comet peaks ~-8 to -9 at perihelion), minus Marcus' (2007) forward-scattering surge
  with the compound Henyey-Greenstein dust phase normalised at 90 deg (dust-to-gas 0.6 x `dust`).
- **The head** is the inner coma within ~1 arcmin (a 1/rho coma: share = 1' / the coma's radius, 0.003..1) as a point:
  appended to kFireBuf by `cometsFillHeads` (kesslerFillDust's finish) at range 2e9 m (infinity), so point, bloom and
  glare draw it; dimmed by day / Moon like a satellite (`dayBright x daySuppression`: the Great Comet shows near the Sun
  in daylight), by the light-pollution dome like a planet, extinction along its line of sight, refracted. **Below the
  horizon** = under the Earth's limb seen from the eye (z < -sqrt(1 - (R / r)^2)), then the refraction table: a fixed
  z < -0.3 hid the head and the coma from orbit (23,000 km up the limb dips 77 deg) and they popped back as the view
  turned (issue 20261030_140112). **Not on Potato** (no kesslerFillDust there).
- **The tails are 3D PARTICLES** (2026-10-28, issue 20261028_192308 "Comet Graphics": the old sheet of sky directions drew
  stair-step bands, a hard-edged fan and an oval coma at the closest approach). `comet::buildParticles` (Comets.cpp):
  - **Dust**: an absolute grid of emission slots uniform in u = asinh((te - tPeri) / tau0) (`du` 0.01; tau0 0.05 day for
    the Great Comet, 2 days for minor ones): fine round perihelion, coarse far from it, stable as time runs. A slot holds
    8 x cosh(u)^0.33 grains (12-128 Great, 6-40 minor: wide slots get more; ~50,000 for the Great Comet). A grain (hashed
    by slot and index): te jittered in its slot, beta stratified in ln beta over 0.002-0.95 (order shuffled per slot),
    ejection ~80% into the sunward hemisphere at 0.5 km/s x sqrt(beta) x r^-0.5, flown by `propagate` under mu (1 - beta)
    from the nucleus at te (Taylor from the slot's centre state). Weight = `dustFluxK` 0.025 x dust x Fhead0 x the dust
    rate at te x the slot's days / n x the size weight, faded over the youngest 0.08 day (the coma), the oldest 45-100%
    and beta 0.45-0.95 (the sampled ranges' ends drew hard lines); never at the large-grain end (the anti-tail). Spread
    (the Gaussian a grain stands for) = 0.35 v_ej age (+ the slot's width x its speed relative to the nucleus).
  - **Ions**: 12 parcels a pick-up (5 streamers, to 2.5 days), picked up at the nucleus, then the comet's
    velocity decays over 0.03 day while the solar wind carries them radially, s = 250 km/s (a - 0.15 (1 - e^(-a/0.15)))
    (a in days): the tail is anti-sunward aberrated by the comet's motion and reorients round perihelion; streamer
    angles wander, kinks travel with the plasma. **Pick-ups at every whole w = t / 0.01 day + nu / `ionDnuRad` (0.02)**
    (nu the true anomaly, unwrapped by the orbit count; Newton for each slot's time), each standing for 1 / (dw/dt) days,
    its spread widened by the radial's swing and the wind's travel over that time (2026-10-28, issue 20261028_221813: on
    a time grid alone the radial turned ~0.5 rad a slot at perihelion and the swing drew the streamers as separate blue
    puffs). 3000 parcels far from perihelion, ~6000 through the swing. **Brightness by the gas rate**: pick-ups follow
    the production Q ~ r_eff^-(n - 2) (`ionFluxK` 0.3 x gas x Fhead0), each ion fluoresces by the sunlight where it is
    NOW (comet_splat.vert: / (r^2 + rSat^2), rSat^2 in `F.comet[2s+1].w`), e-folding 1 day. Colour CO+ deep indigo
    (0.16, 0.26, 0.72), darker and greyer than the electric (0.05, 0.17, 2.1) of 2026-10-30 and the pastel cornflower
    (0.16, 0.42, 1.95) before it (issues 20261030_140112, 20261028_221813: the composite times the colour by the
    brightest channel, so a blue far above its luminance was lifted to a pale one). The composite divides by that
    channel and keeps the hue: the ratios are the hue on screen, its luminance (0.96 of the old) the photometry, and
    the brightest channel over it (2.65, was 7.4; the dust's 1.06) how hard the shoulder lifts the tail: that ratio is
    its display brightness. **Streamers**: each 0.008 of its distance from the head wide
    (0.025 merged the five, 0.032 rad apart, into one band), its brightness varying along it with the emission time
    (knots and gaps ~0.3 day apart that ride out with the plasma) and between streamers, 0.25-1.75 x, mean 1. Through the
    swing the old perihelion parcels fade with their distance from the Sun, and after it the ion tail is faint: its
    production has fallen as r^-2 since perihelion and its ions last a day, while the dust tail is the perihelion's.
  - **Builds** are a pure function of their time on a grid: `particleBuildStepS(r)` = 0.003 of the nucleus's dynamical
    time, a power of two, 4 s .. 4 h (8192 s at 1 AU). A build (~15-20 ms on half the cores) runs on a worker
    (`std::async`); on the frame (all cores but two) only after a JUMP (sim time moved > 4x the last frame's step) or
    once the worker's build lags > 8 frame steps, or always with `comets.particles_sync` (2026-10-28: it rebuilt on the
    frame whenever 3 grid steps behind, i.e. every frame at 1 d/s through perihelion: 145 frames over 50 ms in 240, 0
    now). Each particle (`comet::GpuGrain`, 64 B) stores its state at tBuild relative to an anchor (the nucleus then); the GPU
    carries it to the frame by p + v dt + a dt^2 / 2 (a from its own heliocentric position x (1 - beta)): any warp
    shows the same tail. At 1 d/s: ~6 builds a second, a frame build (~20 ms) only near perihelion / after jumps.
  - **Drawn** in two passes: `cometsRecordSplat` (recordCompute's end, its own render pass) adds every particle as a
    Gaussian quad into a HALF-RES RGBA16F ATLAS (`comet_splat.vert/.frag`; header `GpuCometFrame` 512 B at the buffer's
    head, `include/comet_frame.glsl`; regions of 132k / 24k / 24k particles for the three tail slots): light = weight /
    Delta^2, x the Halley-Marcus phase at the grain's own scattering angle / r^2 for dust (warm white; ions blue, no
    phase); size = sqrt((spread / Delta)^2 + (0.7 px)^2). **The atlas has 6 levels** (level 0 the half-res screen, level
    k W >> k by H >> k to its right, `F.pad[1]`): a particle wider than the 22-px quad cap draws into the coarsest level
    holding it within the cap at its TRUE surface brightness (fill per particle bounded), past level 5 faded out as before;
    the composite adds the levels back (bilinear). Until 2026-10-28 every particle past 4x the cap was culled: zoomed to a
    20-deg field at the closest approach (a grain's spread is degrees there) the tail vanished. Culled under lum 1e-6. `cometsRecordDraw` composites (`comet_composite.vert/.frag`, a full-screen
    triangle at kDepthFar) the target + an analytic Haser coma per tail (Lc e^(-rho/rho_s) / sqrt(rho^2 + rho0^2),
    rho_s = the visible coma / 2.5, rho0 the point's 1 arcmin: round on the sky, faded out over x = rho / rho_s 5 -> 12
    (a cut at 12 drew a hard circle round a bright head); white inside, a faint C2 green outward (0.3 x the gas share,
    from x 0.3 -> 2)), then per pixel the extinction, `cometSkyMag` / `darkSkyVis`, exposure, cloud occlusion and the
    shoulder K (1 - (1 + x / (K p))^-p), K 0.92, p 0.5, on the brightest channel (hue kept): slope 1 at 0, a decade of
    brightness still a visible step near white (the x / (1 + x / K) it replaced drew the bright ion tail as a flat band).
    The splat target is recreated in onResize.
  - **Zoom detail** (2026-10-28, issue 20261028_221703; `comets.tail_detail` 1, 0 = off, no UI; `comets.tail_debug` 1 / 2
    the field's te / its weight and spread): each dust grain carries its local (emission time te, ln beta) field
    (GpuGrain `d`: dP/dte and dP/dlnbeta as half floats, the field's te at the grain with its own ejection offset
    decomposed out, from 10 zero-ejection reference grains per slot at the slot's centre and dTe later: 20 extra
    propagations a slot, build ~15-20 ms as before). A FIELD PASS (same vertex shader, push constant `screen.w` 1,
    `comet_field.frag`) splats (L te, L lnb, L, L te^2) into an RGBA32F atlas at a quarter of the splat resolution
    (`F.pad[2]`; the field is linear per Gaussian, and 32-bit blending at full splat size cost ~1 ms), and into a second
    target (RGBA16F, same layout, `gradTex` binding 8) the light-weighted magnitudes of the field's screen gradients per
    full-res pixel (|d te|, |d lnb|, |d (te + 0.7 lnb)|, clamped at 10, the weight in alpha): the octaves' LOD. Each
    grain's Gaussian in the field atlas is at least `comets.field_floor_px` (0.5) field pixels (its linear field
    extrapolated). Until 2026-10-30 the LOD was dFdx / dFdy of the bilinear field, constant across each field texel (4-128
    screen px by level), and small grains under-sampled it: blocky "pixel noise" and black notches along the dust trail
    near the head (issue 20261030_140112); the detail now never takes away more than the light the colour atlas holds
    there (min(field weight, luminance)), and the composite reads the field only where the tails hold light. The composite
    divides it back and multiplies the dust light by synchronic bands in te (periods 1.7, 0.42, 0.105, 0.026, 0.0065
    days: time-variable emission), oblique striae in te + 0.7 lnb (0.9, 0.22 days, dust older than 2-8 days:
    fragmentation) and filaments in lnb (0.4, 0.1), mean 1; an octave shows once its bands are 4-10 screen px apart and
    the field's spread there (sqrt of the te variance) is under 0.12-0.3 of its period, so zooming in resolves finer ones
    and overlapping sheets show none. Young dust (< 2-6 days) also spreads over its size stratum along the syndyne (the
    swing's speckle). Open: closed te contours (rings) where the field has an extremum; the field is undefined where
    synchrones and syndynes run together on screen (faded by their angle).
  - Framing for the Watch (`tailSpanDeg`, `tailAimEnu`): from each build, the angle from the head holding 92% of the dust
    light and the light-weighted direction of the 55-100% band.
- **Selection**: `pickCometAt` (before planets in both click paths), `selectedCometIndex` (dropped whenever a satellite
  or planet is selected), the planet-style panel from `cometInfoLine` (magnitude, distances, elongation, tail and coma,
  perihelion). Harness `state` -> `selection.comet`.
- **Sky events** (`skySearchComets` / `skyWatchComet` in SatelliteSimSkyEvents.cpp): each comet's best display (6-h
  steps refined to 30 min, >= 20 deg from the Sun, score = -mag + 2.5 log10(1 + tail / 5 deg); the window while within
  ~2.5 mag of it), kept when brighter than mag 8; the card shows the BRIGHTEST under way or within a year
  (`skyEventNext`'s comet rule). Watch (`cometWatchAt`): the moment (inside the window, never before it), a place where
  the head is >= 8 deg up (less near the Sun) and the Sun <= -12 deg, clearer sky first, nearest the player, framing the
  head and the visible tail's far end (`tailAimEnu`, lum > 0.004) at 1x; `skyEventsTick` keeps the aim (sky bodies
  `kBodyComet0` / `kBodyCometTail0`).
- Measured (2026-10-28, RTX 3070 Ti, 1920x1009, tails on - off, interleaved x3, splat + field + composite): closest approach
  (issue 20261028_221703's view) 0.92 -> 0.9-1.3 ms, zoomed to 20 deg 0.6 (the tail was culled) -> 0.5-1.1, the swing
  (Feb 19, 3140 km) 0.7-1.4 -> 0.4-1.5 (run noise +-0.3); particle buffer 8.1 -> 11.5 MB (64 B x 180k), splat atlas
  3.9 -> 5.8 MB + field atlas 1.5 MB; 1 d/s through perihelion: 145 -> 0 frames over 50 ms in 240. Earlier: the splat pass ~0.5 ms at the closest approach from the ground at a 120-deg field
  (the report's view; 0.97 ms with a 40-px cap), 0.2 ms at +20 d; the composite < 0.1 ms; CPU ~0.02 ms a frame between
  builds. Videos and captures in `harness_runs/comet/` (untracked): r8/videos (inertial Feb 1 - Apr 1 at 1 d/s, the
  perihelion swing at 28 deg, the ground at the closest approach at 5 min/s). 2026-10-30 (issue 20261030_140112's five
  views from 23,575 km, 1920x1009, detail on - off in one app): the field pass with its gradient target 0.4-1.1 ms (it
  was ~0.1-0.8 with one target; a second RGBA32F target cost twice that, a second instanced pass 2-3x).
- Settings `comets.enabled` / `minor` / `tails` / `tail_gain` (x the tails' physical brightness) / `display_gain` /
  `particles_sync` / `tail_detail` / `tail_debug` / `field_floor_px` (no UI).
- Open: the tail is not in reflections (sea, mirrors) nor env probes; no ion-tail disconnection events; the striae are a
  pattern in the (te, beta) field, not flown fragments; the coma's size is a fixed 2e5 km x r^-0.5; Potato draws no head; minor comets have no return
  apparitions; no Settings UI rows yet; at the closest approach the Earth's own passage through the dust is not drawn
  (grains within ~0.001 AU are faded out by the splat cap).

## Inertial camera lock (2026-10-28)

`SatelliteSimInertial.cpp`. The view held fixed relative to the stars (geocentric ECI): `inertialTick` runs in
recordCompute right after the time advance (after updateFollow, before updatePositions), turns the stored ECI view into
this moment's ENU and sets az / el (`aimCameraAzEl`). With `inertialHold` (the default above 100 km) the eye is held in
ECI too, so the Earth turns beneath (the ENU frame at an ECI-fixed eye is itself ECI-fixed: no roll drift); on the
ground only the direction tracks (field rotation). Mouse look and WASD are adopted (in the previous frame's ECI). Ended
by follow, Track, a cinematic, any sky-event Watch or the comet Watch, a bookmark's Go and any recorded view (harness `report load` / `snapshot`, an issue shot's Go: `harnessApplyViewRecord`). **Occulter** (`inertialOcculter`, "Occult the
Sun"): sunCloudT forced to 0 (the Sun's disc, glare, corona, lens flare, bloom seed), no sky glare (stars stay), the
comet composite's Sun glare off, and from above 100 km the night exposure (`skyExposure` dayness 0). UI: the Sky events
window's "Lock view to the stars" / "Occult the Sun" buttons. Harness `inertial [state] | on [hold=] | off | look
ecl_lon= ecl_lat= [fov=] | place ecl_lon= ecl_lat= alt=` (`occult=0|1` on any). Validation views: eye at ecliptic
(318, +40) 80,000 km, looking at (316, +3) FOV 70 or (325, +2) FOV 28, Feb 1 - Apr 1 2037 at 1 d/s.
Also fixed with it: `updateStars` counted the twilight sky (and moonlight) from ABOVE the air (x atmFrac now), so from
orbit with the Sun up every star was gated out.
