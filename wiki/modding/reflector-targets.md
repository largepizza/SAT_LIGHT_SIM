# Reflector targets

`reflector_targets.json` lists the places on the ground that orbital mirrors can aim sunlight at: solar power
plants, farmland, towns. Every satellite type whose attitude reflects sunlight onto a ground site (the
`sun_reflect_ground_site` target, or the legacy `TargetedReflector` mode) picks its site from this list. Solar
plants in the list are also drawn on the terrain as photovoltaic parks. This page documents the file. How a mirror
chooses a site and what its beam looks like is on [Reflectors and beams](../simulation/reflectors.md); how the
parks are drawn is on [Cities, farms and solar parks](../rendering/cities.md).

The file sits next to the executable (`data/reflector_targets.json` in the source tree) and is read once at
start-up.

## Format

```json
{
  "_comment": "anything; top-level keys other than \"targets\" are ignored",
  "targets": [
    { "name": "Observer Spawn", "kind": "none", "lat": -67.0, "lon": -67.0,
      "capacity_mw": 0, "observer_spawn": true },
    { "name": "Bhadla Solar Park", "kind": "solar", "lat": 27.535, "lon": 71.915, "capacity_mw": 2245 },
    { "name": "Ivanpah Solar Complex", "kind": "solar", "lat": 35.56, "lon": -115.47, "capacity_mw": 392,
      "tech": "csp" }
  ]
}
```

Every element of `targets` is read as a target. There is no comment syntax inside the list: an extra object
added as a note becomes a target at latitude 0, longitude 0. Put notes in top-level keys instead.

| Field | Type | Default | Meaning |
|---|---|---|---|
| `name` | string | `""` | Shown in messages; when `kind` is missing, it is inferred from the name |
| `lat` | degrees | 0 | Latitude, north positive |
| `lon` | degrees | 0 | Longitude, east positive |
| `capacity_mw` | number | 0 | The plant's capacity. Sets a solar park's area; also helps infer a missing `kind` |
| `kind` | `solar`, `agriculture`, `daylight`, `none` | inferred | What the site is (below) |
| `area_km2` | number | from capacity | Solar parks: the park's area, overriding the estimate from capacity |
| `mount` | `tracker` or `fixed` | by region | Solar parks: how the panels are mounted |
| `tech` | string | none | A note such as `csp` for concentrated solar plants. Not read; such plants are drawn as photovoltaic parks |
| `observer_spawn` | boolean | false | Marks the site at the fixed fallback spawn point (below) |

### Kinds

| Kind | Effect |
|---|---|
| `solar` | A solar power plant. Mirrors may aim at it, and it is drawn on the terrain as a park of panel rows |
| `agriculture` | Farmland (agrivoltaics). A mirror target; no ground drawing |
| `daylight` | A town receiving extended daylight. A mirror target; no ground drawing |
| `none` | A plain target with no meaning attached |

Every kind is an equally valid aim point for a mirror; the kind changes only what is drawn on the ground. An
unrecognised kind is reported on standard error and treated as `none`.

When `kind` is missing it is inferred, in this order: `none` if `observer_spawn` is true; `daylight` if the name
contains "polar illumination"; `agriculture` if the name contains "agrivoltaic"; `solar` if `capacity_mw` is above
0; otherwise `none`. Older or hand-made lists therefore keep working.

### Solar parks

For a `solar` site the program draws a park of panel rows on the terrain, centred on `lat`/`lon`, when you are
within view of it.

- **Area**: `area_km2` if given, else 2.2 hectares per MW of `capacity_mw` (0.022 km² per MW), at least 1 km². A
  2245 MW plant covers about 49 km².
- **Mount**: single-axis trackers (rows running north-south that turn east-west) in the Americas (west of 30° W),
  in India and its neighbours (68° E to 90° E, south of 30° N) and in Australia (east of 110° E, south of 10° S);
  fixed tilt facing the equator everywhere else. `mount` overrides the region.
- **Trackers** turn toward the Sun while it is up at the site; at night they turn toward the strongest mirror beam
  landing on the park, and otherwise lie flat. They swing smoothly, over several seconds. Fixed rows are tilted
  toward the equator at about 0.8 times the latitude.
- At most the eight nearest solar sites within 2500 km of the observer are drawn.
- Where a park overlaps a city, part of it becomes rooftop panels on the city's buildings.

The panels reflect the sky, the Sun and the mirror beams: seen from the right angle, a park lit by orbital mirrors
glitters with their reflections.

| Setting | `settings.json` key | Default |
|---|---|---|
| Draw solar parks (no slider) | `clouds.solar_arrays` | true |

## How the list is used

- **Ground height.** Each site's height is read from the elevation map (the highest of the nearby map cells, plus
  a 75 m margin), so a beam lands on the ground rather than inside a hill.
- **Choice of site.** A mirror considers every site that is on the night side and sees the satellite above a
  minimum elevation, and prefers each site by a fixed per-satellite, per-site preference, so the mirrors spread
  over the available sites. Its choice is held for a lock window and the mirror slews between sites at a limited
  rate. All of it is a pure function of time, so running time backwards reproduces the same beams.

| Setting (Beams tab) | `settings.json` key | Default |
|---|---|---|
| Target lock window (s) | `clouds.reflector_lock_window_s` | 90 |
| Min beam elevation (deg) | `clouds.reflector_min_elev_deg` | 10 |
| Mirror max slew rate (deg/s) | `clouds.mirror_max_rate_deg_per_sec` | 0.11 |

A mirror type casts its beams only when its geometry has a mirror face along the site-aimed axis; see
[Constellations](constellations.md#model-types).

## The spawn point entry

The first target in the shipped list, *Observer Spawn* at 67° S 67° W, sits at the program's built-in fallback
observer position, so that at least one site is always near the observer when no other position is set. The
program records which entry carries `observer_spawn: true` and logs whether one was found; the flag has no other
effect.

## Limits and fallback

| Case | Behaviour |
|---|---|
| More than 201 targets | Only the first 201 are used; a message goes to standard error |
| File missing, not valid JSON, or no entries in `targets` | 200 sites are generated: one at 67° S 67° W and 199 at random points over the globe (oceans included), all of kind `none`, so no parks are drawn |

Messages about this file go to standard error, not to `satlight_log.txt`; the summary line reads
`Loaded N reflector targets from reflector_targets.json (observer-spawn pin found)`.

## Example: adding a site

```json
{ "name": "My Solar Farm", "kind": "solar", "lat": 34.10, "lon": -117.30,
  "capacity_mw": 150, "mount": "fixed" }
```

Add it to `targets` and restart. Orbital mirrors start using it as soon as it is on the night side and in view of
them, and from within a few hundred kilometres the park appears on the ground: about 3.3 km² of fixed-tilt rows
facing south.

## Where in the code

- `src/simulations/SatelliteSim.cpp`: `loadReflectorTargets()` (this file),
  `generateReflectorTargetsRandomFallback()`, `computeReflectorTargetElevationRadius()`, `fillSolarSites()` (the
  parks drawn each frame).
- `shaders/sat_orbit.comp`: the per-satellite choice of site.
- `shaders/sat_sky.frag`: drawing the parks.
