# Ambience

The ambience is a set of sound *layers*, each a looping sample, a stream of one-shots or a procedural
synthesiser voice, whose volumes follow the camera's context: altitude, the Sun, the ground underneath, the rain and wind,
nearby satellites, the flares on screen. This page documents the layer table so it can be edited, lists
every context value a layer can depend on, and describes the voices, samples and level conventions.

The music player and the key-following system are on [Music and tonality](music.md); the overview of how the
two relate is on [Sound](index.md).

## How a layer's volume is decided

The table lives at `assets/sound/ambience/ambience.json`. It is moddable: edit it, rebuild (see the warning
below) and the next launch uses it.

Each frame the sim computes a set of named numbers called **drivers** (altitude in metres, Sun elevation in
degrees, the ocean fraction nearby, and so on). Every layer turns those numbers into a target gain:

\[
g_\text{target} = \text{gain} \times g_\text{group} \times \prod_{\text{when}} r_i \times \max_{\text{any}} \Big( \prod r_j \Big)
\]

where each \( r \) is a **ramp**, a smooth step from 0 to 1 over a range of one driver. The layer's actual
gain then slides toward the target at a fixed rate. Because every condition is a ramp and every change is a
slide, nothing in the table ever switches a sound on or off abruptly: every transition is a crossfade.

!!! warning "The table only reaches the app through a build"
    `ambience.json` and the samples are copied next to the executable by the build's runtime-file sync
    (see [Building](../development/building.md#runtime-files-next-to-the-executable)). Edit, build, then run;
    otherwise the old table plays. A broken table is a load error, written to `satlight_log.txt` as an
    `ambience:` line, and the ambience is then silent.

## The table format

The top level has five keys:

| Key | Purpose |
|---|---|
| `version` | 1 |
| `tonality` | how the key is derived from the music ([Music and tonality](music.md#the-tonality-block)) |
| `music_upwell` | the upwell stem's gain from one or more driver maps ([Music and tonality](music.md#upwell-stems)) |
| `shell_groups` | groups of constellations whose proximity becomes drivers ([below](#satellite-shell-drivers)) |
| `layers` | the list of layers |

Keys starting with `_` (`_about`, `_note`) are comments and are ignored.

### A layer

| Field | Type | Meaning |
|---|---|---|
| `id` | string, required | unique name; used by the harness (`audio force`, `audio expect`) and the Sound tab readout |
| `synth` / `loop` / `events` | exactly one | the voice: a synth kind, one sample file to loop, or a list of files to fire as one-shots |
| `group` | string | one of `wind`, `water`, `nature`, `city`, `space`, `machines`; each has its own gain on the Sound tab |
| `gain` | number, default 1 | the layer's full-scale gain |
| `fade_in_s` | number, default 1.5 | seconds from silent to full gain (minimum 0.05) |
| `fade_out_s` | number, default 0.8 | seconds from full gain to silent (minimum 0.05) |
| `when` | object `{driver: ramp}` | every ramp multiplies the gain |
| `any` | list of `{driver: ramp}` objects | each object is the product of its ramps; the largest one multiplies the gain (an OR of alternatives) |
| `params` | object | synth parameters (synth layers only): a number, `{"root": k}` or `{"tonal": "scale" \| "chord" \| "tension"}` |
| `mod` | object `{param: mod}` | a parameter driven by a driver every frame (below) |
| `interval_s` | `[min, max]`, default `[6, 18]` | events only: random delay between one-shots |
| `pan` | number, default 0.8 | events only: random pan spread, -pan .. +pan |
| `gain_jitter` | number, default 0.3 | events only: each one-shot is quieter by up to this fraction |

File paths are relative to the table's folder. miniaudio decodes WAV, FLAC and MP3; use FLAC for loops
(see [Samples](#samples)).

### Ramps

A ramp is a list of two or four numbers:

- `[a, b]` rises smoothly from 0 at `a` to 1 at `b` (a smoothstep). If `a > b` it falls instead: 1 at and
  below `b`, 0 at and above `a`.
- `[a, b, c, d]` rises from `a` to `b` and falls from `c` to `d`: a band.

Example, from the cricket layer:

```json
"when": { "agl_m": [300, 30], "sun_el_deg": [-2, -10], "lat_deg": [-58, -50, 50, 58] }
```

Full volume below 30 m above the ground, gone by 300 m; full once the Sun is 10 degrees below the horizon,
silent while it is above -2 degrees; only between about 50 S and 50 N.

### Mods

A mod sets a synth parameter (or, for loop and events layers, `rate`) from a driver, every frame:

```json
"mod": { "level": { "driver": "cloud", "in": [0.2, 0.75], "out": [0.4, 1.0] } }
```

`in` and `out` are breakpoints of a piecewise-linear map, clamped at both ends. They must have the same
length, two or more points, and `in` must be monotonic (a falling list is reversed at load). Use more than
two points for drivers that span orders of magnitude, such as `glare_n`, which runs from 0 to several
hundred.

For a loop or events layer the only modulatable parameter is `rate`: it divides the events' interval (faster
one-shots). A parameter that is both in `params` and a mod target is overwritten by the mod every frame.

### Pitched parameters: `root` and `tonal`

A synth parameter written `{"root": k}` is set to \( k \times \) the **tonal root**, a frequency between
36 and 96 Hz that follows the music's key (see [Music and tonality](music.md)). Every pitched voice in the
table is a multiple of the same root, so voices whose zones overlap stay consonant: the comms drone sits on
2x the root, the datacenter hum on 1x with its whine on 16x, the LEO cabin hum on 1x, the aurora hum on 2x.

A parameter written `{"tonal": "scale"}`, `"chord"` or `"tension"` receives a 12-bit pitch-class mask
*relative to the root* (bit \(k\) is \(k\) semitones above it): the track's pitch set, the chord it is on
now, or the tension tones from the `tonality` block. Only the glare pad uses them.

### Validation

The table is checked when it loads, and any of these is an error that stops the load:

- a driver name nobody registered (a typo in a driver is never a silently muted layer);
- an unknown synth kind, or a parameter or mod that the synth does not have;
- a ramp that is not two or four numbers; a mod with mismatched or non-monotonic breakpoints;
- a duplicate `id`; a layer with none of `synth`, `loop`, `events`;
- an `events` list with no files; a `root_range_hz` that spans less than an octave.

A missing sample file is not a load error. That layer is marked failed (the harness's `audio state` shows
the reason) and the rest play.

## Context drivers

The drivers are computed each frame in `SatelliteSim::computeAmbienceContext()`. They describe the
**listener, which is the camera**: in follow mode the camera rides with a satellite and the drivers follow
it, not the parked ground observer.

### Position and environment

| Driver | Unit | Meaning |
|---|---|---|
| `alt_m` | m | the eye's height above sea level (never below the ground) |
| `agl_m` | m | height above the ground |
| `ground_m` | m | ground height under the camera: the GPU's detailed terrain when the depth pass runs, else the CPU's coarse DEM copy |
| `lat_deg`, `lon_deg` | deg | geocentric latitude and longitude of the camera |
| `sun_el_deg` | deg | the Sun's elevation at the camera |
| `ocean_near` | 0..1 | ocean fraction within about 3 km (a full-resolution land/sea mask built from the DEM) |
| `ocean_wide` | 0..1 | ocean fraction within about 25 km |
| `veg`, `forest`, `desert`, `ice` | 0..1 | land cover under the camera, classified from the colour of the day map averaged over about 60 km |
| `urban` | 0..1 | city brightness from the night-lights map, through the light-pollution dome's response curve |
| `wind` | 0..1 | a smooth pseudo-random wind strength over position (about 500 km cells) and sim time (hours); deterministic for a place and time |
| `cloud` | 0..1 | the 2D cloud coverage map directly overhead, with the same drift the clouds use; eased over 1.5 s; 1 if the map failed to load |
| `rain` | 0..1 | the rain rate around the eye (below), times the liquid share; eased over 2 s |
| `rain_driven` | 0..1.2 | `rain` pushed up by up to 60% in a gust: wind-driven rain lands harder |
| `gust` | 0..2 | the envelope of the gust fronts the rain and snow particles are blown by: 0 in calm air, about 1.5 at a strong gust's peak, rising over 1 to 2 s and decaying over 5 to 20 s ([Weather](../rendering/clouds/weather.md)) |
| `surface_wind` | 0..2 | the surface wind's speed at the eye over 15 m/s: the mean wind, storm outflow, gust fronts and a tropical cyclone's wind field |
| `tornado` | 0..2 | the funnels on the ground near the listener, each \( \sqrt{w / 150\,\text{m}} \times \ell / (1 + (d / 2\,\text{km})^2) \) for a funnel of width \(w\), lifecycle \(\ell\) (rising as it touches down, gone as it ropes out) and distance \(d\) to its nearest point between the ground and its cloud base; a waterspout counts half |
| `thunder` | 0/1 | 1 while thunder or a rocket's roar is arriving, about to arrive (within about 2 s) or ended less than 4 s ago; 0 otherwise ([below](#rain-and-thunder)) |
| `aurora` | 0..1 | inside the auroral oval (the band only, no curtain noise), 0 when the aurora gain is 0 |

The land-cover classification uses the map as drawn, so the sound agrees with the ground the player sees.
The day map is a dry-season mosaic, which makes grassland such as the Great Plains read tan; "not desert and
not ice" is the better test for open grassland.

### Beams and glare

| Driver | Unit | Meaning |
|---|---|---|
| `beam` | arbitrary | Reflect Orbital light on the ground at the camera: the same Gaussian spots the sky shader draws, summed at the camera's ground point. About 1.8e6 at the centre of a lit spot, 0 outside |
| `glare_n` | count | how many flares are glaring on screen; each counts fully once it is 0.4 past the glare threshold. Eased up in 0.1 s, down in 0.6 s |
| `glare_sum` | arbitrary | their combined strength past the threshold, eased the same way |
| `beam_site` | count | Reflect beams converged on ground sites near the listener: each counts 1 within 15 km, \( 1/(1 + (e/25\,\text{km})^2) \) beyond, where \(e\) is the distance past 15 km. Eased over 1.5 s |
| `rocket_mag`, `stage_mag`, `fireball_mag`, `dust_mag` | mag | the upwell stem's other sources: burning engines, sunlit coasting stages close by, re-entry fireballs, collision dust clouds, each summed over the sources on screen or near the camera as an equivalent apparent magnitude (30 = none). Eased up in 0.15 s, down in 0.8 s ([Music and tonality](music.md#upwell-stems)) |
| `music_gap` | 0..1 | 0 while a track plays; a triangle over the silent gap between tracks (0, 1 halfway, 0); 1 when no music is audible (player off or paused, music volume or the altitude fade below 3%). Moves at most 1/4 per second |

`glare_n` and `glare_sum` apply the glare sprites' own test: a flare glares when the bloom's log response,
\( b = \tfrac12 \log_2 F \) for an effective flare \(F\), passes the glare threshold. They count only flares
inside the view, with a soft frame edge (full at 95% of the half-extent, none past 115%), so a flare leaving
the frame fades out. They do not test occlusion by terrain or cloud. The input is the flare compute pass's
list of bright flares, read back from the GPU every frame (the result of the frame two frames earlier). Facing the
Reflect Orbital ring from a lit site,
`glare_n` reaches about 470.

`beam_site` is about 180 standing on a busy beam site and about 10 from 120 km away.

### Time, state and motion

| Driver | Unit | Meaning |
|---|---|---|
| `time_scale` | s/s | sim seconds per wall second; time-of-day layers fade out under time warp |
| `following` | 0/1 | follow mode is active |
| `intro` | 0/1 | the intro cinematic is playing |
| `speed_mps` | m/s | the camera's speed in Earth-fixed coordinates (time warp alone moves nothing); eased over 0.3 s |
| `eas` | m/s | equivalent airspeed, \( v \sqrt{\rho / \rho_0} \) with an 8.5 km scale height: large low down, near 0 in orbit |

A jump of more than 20 km in one frame (teleporting the observer, Go to) counts as no motion, and a median of
three frames rejects a one-frame jump of the eye. No layer in the shipped table uses `speed_mps` or `eas`.

### Satellite shell drivers

Each entry in `shell_groups` registers two more drivers:

```json
"shell_groups": {
  "comms":      { "match": ["starlink*", "oneweb", "amazon_leo", "guowang"], "hearing_m": 500000 },
  "datacenter": { "match": ["spacex_ai_sat"], "hearing_m": 50000 }
}
```

| Driver | Unit | Meaning |
|---|---|---|
| `<group>_count` | count | expected number of the group's satellites within `hearing_m` of the camera |
| `<group>_near_m` | m | expected distance to the nearest one |

A pattern (only `*` is a wildcard, case-insensitive) matches a constellation whose satellite type has that
model id or type name. Disabled constellations are skipped. With the deployment era on, each constellation (and each
filed group of a Shells constellation) counts only its share deployed at the current date, so a sky of 2020 hums with
the first Starlink launches rather than the shells of 2036.

The values are computed in closed form from each constellation's orbit distribution, not by searching the
roster, so they cost nothing at millions of satellites. For a Walker shell of \(N\) satellites at radius
\(R\) and inclination \(i\), the surface density at latitude \(\varphi\) is

\[
\sigma = \frac{N}{2\pi^2 R^2 \sqrt{\sin^2 i - \sin^2 \varphi}}
\]

(satellites linger near the turning latitudes \(\pm i\)). A random shell spreads \(N\) over the band
\(|\varphi| < i\). With the camera a radial distance \(\Delta r\) from the shell, the count within hearing
range \(H\) is \( \sigma \pi (H^2 - \Delta r^2) \), and the nearest is about
\( \sqrt{\Delta r^2 + (0.5/\sqrt{\sigma})^2} \). A disk constellation (concentric rings in one orbital plane)
uses the plane of the ring nearest the camera's radius, because each sun-synchronous ring's inclination
follows its own altitude and across a deep disk the outer rings are tilted hundreds of kilometres from the
inner ones. A Shells constellation is taken one filed group at a time: a Walker group is one inclined shell
spanning its altitude range, and a sun-synchronous group contributes one band per plane (each node of the
X-ring), oriented by the shell nearest the camera's radius.

## Fades

A layer's gain moves linearly: from silent to its own full `gain` in `fade_in_s`, back to silent in
`fade_out_s`, however far the target is. A linear slide finishes; an exponential ease never does, and a layer
several seconds behind would still be audible long after the camera left its zone. Both times are multiplied
by the Sound tab's global *Fade in x* and *Fade out x* settings.

Voices are created lazily, the first time a layer becomes audible. A voice at gain 0 is stopped and costs
nothing.

## The procedural voices

The synth voices live in `src/AmbientSynth.cpp`. Each is a miniaudio data source rendering stereo float at
the engine's rate. The main thread writes parameters (atomics); the audio thread reads them once per 64-frame
block and eases toward them, so a parameter change never clicks. A few parameters are marked *snap* and jump
instead: pitch-class masks and the thunder trigger, which must never pass through intermediate values. All
randomness comes from a per-voice seeded generator, so an offline render of the same scene is the same
waveform every time.

There is no melody by design. Pitched material is filtered noise, short data blips, or sustained pitch
classes taken from the music's own key.

| Kind | Sound | Main parameters |
|---|---|---|
| `wind` | band-limited noise whose loudness and pitch rise together in a gust, a little low rumble, a hiss growing with the gust, an optional whistle at gust peaks; high-passed at 45 Hz | `level color center_hz q body body_hz gust gust_rate whistle whistle_hz width hiss hiss_hz flutter` |
| `hum` | pink noise through resonances at f0, 2f0, ... plus broadband air, with slow drift and swell; high-passed at 32 Hz | `level f0 harmonics q drift drift_rate air air_hz wobble wobble_rate width` |
| `drone` | a harmonic series on f0 with a detuned copy for slow beating, through a phaser; optional whine with vibrato, air, a compressor-like on/off cycle, and occasional soft status motifs on consonant ratios; high-passed at 30 Hz | `level f0 harmonics bright odd detune phaser phaser_rate phaser_depth phaser_fb whine whine_ratio whine_wobble air air_hz cycle_s cycle_depth tones tone_ratio tone_ms tone_level swell width` |
| `surf` | breaking waves: two wave trains at incommensurate periods (swell, break, roar, wash) over a distant bed; high-passed at 55 Hz. `distance` 0 is on the beach, 1 open ocean | `level period jitter roar_hz fizz fizz_hz distance bed width crash` |
| `pad` | the glare chorus ([below](#the-beam-sounds)) | `level f0_hz scale_mask chord_mask tension_mask voices hollow bright detune_cents warp_cents warp_rate glide_s attack_s release_s bloom bloom_s shimmer cutoff_hz reverb reverb_s spread hp_hz` |
| `bass` | the beam-site pedal ([below](#the-beam-sounds)) | `level f0_hz fifth sub bright hollow beat_hz throb throb_rate bend_cents bend_rate drive cutoff_hz width hp_hz` |
| `rain` | a band-passed hiss, a low rumble for heavy rain, a Poisson stream of drop ticks each with its own resonance, level and pan | `level intensity drops hiss_hz rumble width` |
| `thunder` | streamed voices for thunder and rockets, driven by events from the sim ([below](#rain-and-thunder)) | `level trigger distance_km energy pan` |
| `beeps` | FSK data bursts (unused by the shipped table) | `level rate carrier_hz spread tone_ms tones_min tones_max gap_ms doppler width bright_hz symbols` |
| `disk` | seek clicks and fan noise (unused) | `level activity whine whine_hz fan fan_hz click_hz seek_ms clicks width` |
| `chorus` | procedural VLF chorus, whistlers and sferics, flanged (unused; a recorded loop is used instead) | `level hiss chorus_rate chorus_hz chorus_rise chorus_ms cluster whistler_rate crackle width flange flange_rate flange_ms flange_fb` |
| `saw` | a detuned saw stack (unused) | `level f0_hz voices detune_cents fall_oct sub bright drive shimmer shimmer_rate lfo_cents lfo_rate_hz spread hp_hz` |

Every voice's output is high-passed: sub-bass under the music costs headroom and reads as mud. The wind's
gusts swing in decibels, not linearly, so they are audible as gusts.

## The shipped layers

| Group | Layers |
|---|---|
| wind | `wind_ground` (follows `cloud`), `wind_gust`, `tornado_roar`, `wind_desert`, `wind_alpine`, `wind_glacier`, `sea_wind`, `air_low`, `jetstream` (7 to 22 km), `stratosphere` (15 to 70 km) |
| water | `rain`, `thunder`, `beach_waves`, `sea_open` |
| nature | `gulls` (events), `crickets`, `tropical_night`, `birds_forest`, `birds_dawn`, `jungle_day`, `jungle_night` |
| city | `city_day`, `city_night` |
| space | `beam_glare`, `beam_site_hum`, `leo_cabin` (80 km to 6000 km), `aurora_hum`, `vlf_earth` (medium orbit), `firmament` (high orbit), `comms_drone` |
| machines | `datacenter_hum` (inside the AI datacenter disk) |

The ground wind follows the weather overhead (`cloud`): a full bed under cloud and a light breeze in clear
air, so the wind is loud in some places and quiet in others. The open-sea surf (`sea_open`) grows louder, fizzier
and quicker with `surface_wind`: a storm sea, or a hurricane's. The `thunder` layer is up only while `thunder` is (so
*Ambience now* lists it only when there is some) and below about 12 to 25 km above the ground; it carries the
rockets' roar as well as thunder ([Rockets](#rockets)). Crickets and tropical night sounds fall silent
in Reflect Orbital light (`beam` above 20 000). Most nature and city layers fade out under time warp faster
than 300x.

## The beam sounds

Two layers answer the Reflect Orbital mirrors. Both are gated on `music_gap`, so they speak only between
tracks; while a track plays, its upwell stem answers the glare instead (see [Music and
tonality](music.md#upwell-stems)).

**`beam_glare`** (synth `pad`) is screen-space: it follows `glare_n` and `glare_sum`, the flares actually
glaring in view. One flare is two hollow voices (odd harmonics through a low-pass and a long reverb tail), the
current chord's fifth and octave on 8x the root. More flares add voices down a fixed ladder of roles: the
root below, the third high up, the ninth against the octave, the fifth an octave down, the seventh, the root
two octaves down, the sixth, then the tension tones (by default a minor second and a tritone above the root).
The chord therefore widens downward and fills with seconds as flares gather, and only its last voices leave
the music's notes. A joining voice *blooms*: louder for `bloom_s` with a glassy octave partial. Each voice has
its own slow pitch warp, grown with `glare_sum` so a sky full of mirrors never quite sits in tune. `voices` is
fractional (the last voice crossfades in), and a chord change glides every voice over `glide_s`.

**`beam_site_hum`** (synth `bass`) is not directional: it follows `beam_site`, the beams converging on sites
near the listener. It is a pedal on 2x the root, beating slowly against a slightly sharp copy, with the fifth
above and a sine an octave below, driven into a soft clip and low-passed. As beams concentrate it grows
louder, gains drive and sub, throbs faster and sags further and more slowly in pitch.

## Rain and thunder

Rain, the storm wind, tornadoes, thunder and the rockets' roar are all procedural: there are no sample files for them.

**Rain.** The cloud renderer keeps a 32 x 32 map of the rain rate around the eye (40 m cells, about 640 m
either side). The `rain` driver is the mean of its central 6 x 6 cells, so what is heard is what falls on
screen and a passing shaft is heard as it arrives. It is multiplied by the liquid share of precipitation at
the eye's temperature (snow is silent) and is 0 above 6 km. The `rain` layer's `intensity` follows the rate:
a drizzle is a thin hiss with a few ticks, a cumulonimbus core a roar. The `intensity` reads `rain_driven`, so a gust
makes the rain land harder.

**Gusts.** The `wind_gust` layer (synth `wind`, gain 0.25, below about 1.5 km above the ground) swells with `gust`:
its level follows the gust's envelope and its band slides from 340 to 620 Hz, so the roar rises as the rain on
screen leans over in the same gust.

**Tornadoes.** The `tornado_roar` layer (synth `wind`, gain 0.5, below about 3 to 6 km above the ground) is a low,
churning rumble on the `tornado` driver: a broad band centred near 130 Hz far away, rising to 360 Hz and widening
close up, with fast gusts and flutter so it never settles into a steady hiss, and debris hiss joining within about a
kilometre. Recorded alone offline it measures about -36 dBFS RMS 1 km from a funnel (the whole ambience there about
-35), -39 at 3 km and -48 at 12 km.

**Thunder is streamed from sound shells.** When a lightning flash first appears, the host builds its channel as
geometry (see [Weather: lightning](../rendering/clouds/weather.md#lightning)), and every flash except a red sprite
becomes a sound source: the pieces of its main channel and branches (positions, directions and lengths, branches
at half weight) in Earth-fixed axes. Up to 48 sources are kept, each for 150 s of sim time. The sound leaves the
channel at the return stroke and spreads at 343 m/s in sim time, so a piece at distance \(d\) from the listener is
heard when

\[
g = d - 343\,(t - t_0)
\]

crosses zero. Each frame `thunderStep()` finds the pieces whose \(g\) changed sign between last frame's listener and
time and this frame's, whatever moved it: time passing, or the listener. Their power is each piece's length times
its weight over \(d^2\), more where the piece lies across the line of sight (a tortuous channel radiates mostly
sideways). The level sent to the synth is \(270 \sqrt{P / T}\) for the power \(P\) arriving over the frame's
duration \(T\), scaled by the flash's energy (1 for a ground stroke, 0.6 in cloud) and faded out between 20 and
40 km, with the arrivals' mean distance and their bearing against the camera's heading as the pan.

So the roll is the channel's geometry as heard from where the listener is now:

- standing still, near pieces arrive first: a channel lying across the sky claps, one running away rumbles long
  and low;
- flying toward a storm, the sound arrives sooner and compressed (at most 3x); outrunning it, it never arrives;
- paused, nothing arrives; under time warp it rolls faster;
- a teleport (more than 5 km in a frame) or a jump of more than 20 s of sim time arrives silently.

The `thunder` synth has 8 streamed voices, one per sounding flash (`AmbientSynth::pushEvent()`, a lock-free queue
carrying the voice, level, distance and pan). Each voice is pink and brown noise under that level, with a flicker
of about 10 Hz and peals of 0.3 to 1.2 s, low-passed by the distance (about 3 kHz at 1 km, 1.1 kHz at 5 km, 650 Hz
at 10 km, 350 Hz at 20 km), with a ripping crackle within about 4 km. All voices feed a shared low rumble that
decays over about 2 s and joins the peals into one roll. A voice that hears nothing for 2 s is free for another
flash. The synth's `trigger` parameter still starts a generic roll, for `SoundTool`.

Measured with the layer alone at gain 1 (loudest second, RMS): a ground stroke 1.5 km away about -16 dBFS, 5 km
-19, 12 km -25. A strike 3 km away is about 10 dB over the music in the full mix.

## Rockets

Each burning rocket within about 150 km is heard on a thunder voice of its own. Its sound is the one it
made at the **retarded time**: the latest moment \(\tau\) whose sound, travelling at 343 m/s through air that moves
with the Earth, reaches the listener now. `launchSounds()` scans back over 440 s (150 km of sound) for the latest
such moment, in either direction of approach, so a booster's supersonic descent is heard in reverse order; it stops
once the burns that made the sound have been heard. The level grows with the square root of the engines' mass flow,
falls with distance and thins with the air at the rocket. A rocket voice is a steady low roar instead of peals
(brown and pink noise low-passed by the distance, over a sub-bass layer around 62 Hz), with Mach-wave crackle that
fades out by 40 km, a punch when the roar first arrives (three times louder over about 1.4 s, with a burst of
crackle and a thump falling from about 70 to 30 Hz), and a send into an echo (a feedback delay network with a 3.4 s
decay) for the roar off the ground and the landscape. 300 m from a pad it measures about -23 dBFS while the roar
lasts, 25 km away about -31, with most of its energy in the sub-bass.

## Samples

Sample layers are FLAC files: FLAC loops are gapless, while MP3 encoder padding breaks a loop point. Loops are
32 kHz, 36 to 42 s long, written by `tools/make_ambience.py` from CC0 Freesound recordings declared in that
script:

```bash
tools/harness/.venv/Scripts/python tools/make_ambience.py [name ...] [--audition]
```

For each source it downloads the recording (cached in `build/ambience_cache/`, where a lossless `<id>.wav`
placed by hand is preferred over the 128 kbps preview), high-passes it, cuts the loop and folds its tail over
its start with an equal-power crossfade, and normalises it to -24 LUFS. An `events` source is split into its
individual calls, the loudest kept and each peak-normalised. The script rewrites
`assets/sound/ambience/CREDITS.txt` with every source and its licence.

Two files are original to the project and not made by the script: `vlf_earth.flac` (recorded whistlers, for
medium Earth orbit) and `firmament.flac` (a high-orbit pad). They are 44.1 kHz and 55.2 s long, and are not
at -24 LUFS; their layer gains compensate. Do not normalise or regenerate them.

In the live app, loops stream from disk; in offline mode (below) they are decoded up front.

## Levels

The level convention: at `level` 1, layer gain 1 and the default bus volume, every synth kind measures about
-24 LUFS, and every sample is normalised to -24 LUFS. The layer gains then set the mix so that every place
totals about 10 dB under the music (about -35 LUFS against the music's -25). The harness script
`tools/harness/scripts/ambience_tour.satcmd` visits a set of reference places and checks the expected layers
are playing; its last recording is the music reference for comparing loudness.

## Offline rendering for the harness

A muted [harness](../development/harness.md) run initialises the audio engine with **no device**. Nothing is
audible and nothing is mixed until a script asks for a recording: `audio record` then pulls the mix
synchronously into a 16-bit WAV (`AudioSystem::renderWav()`), after a 0.25 s pre-roll that lets volume ramps
and new voices settle. Event layers keep firing during the render through a per-chunk hook (1/60 s chunks).
The result is deterministic and independent of the frame rate. Offline renders outrun miniaudio's streaming,
so in offline mode streamed sounds are decoded instead: music on the first recording that asks for it, loops
when they are created.

Harness commands for sound:

| Command | |
|---|---|
| `audio state [name]` | every driver and every layer's target and current gain, the audible ones loudest first, and the tonality block |
| `audio record <name> [seconds=8] [bus=ambience\|music\|sfx\|all\|music+ambience] [solo=<layer>]` | render the mix to `captures/<name>.wav` and return RMS, peak and per-second RMS |
| `audio expect <layers> [absent=<layers>] [min=0.05]` | fail unless each listed layer is at least `min` and each absent one below it |
| `audio force <layer> <gain\|off>`, `audio force off` | pin a layer's gain anywhere, or release all |

`tools/harness/imgtools.py audio <wav ...> -o spec.png` reports RMS, peak, ungated LUFS, energy per band,
stereo correlation, transients and tonal peaks, and draws a log-frequency spectrogram. Useful scripts:
`ambience_tour.satcmd` (self-checking location tour), `ambience_solos.satcmd` (every layer alone at gain 1,
the calibration run), `ambience_beams.satcmd` (the glare and site drivers and the beam voices).

## SoundTool

`SoundTool` is an offline workbench that needs no window, GPU or audio device:

```bash
cmake --build build --target SoundTool
build/Debug/SoundTool.exe --render pad --out pad.wav --seconds 12 voices=0/0,1/2,6/8,9/0
build/Debug/SoundTool.exe --analyze assets/sound/music/gravity_wave.mp3
```

`--render <kind>` renders one synth voice to a WAV and prints its RMS and peak (`--seconds`, `--rate`,
`--seed`). Each `name=spec` sets a parameter: a constant (`voices=4`), a linear ramp over the render
(`voices=0:12`) or keyframes as `time/value` pairs (`voices=0/0,1/2,4/2,5/0`, linear between, held past the
ends). `--analyze` runs the soundtrack analysis described on [Music and tonality](music.md).

## Settings

| UI label (Sound tab) | `settings.json` key | Default |
|---|---|---|
| Ambience | `audio.ambience_vol` | 0.7 |
| Fade in x (advanced) | `audio.ambience_fade_in_scale` | 0.27 |
| Fade out x (advanced) | `audio.ambience_fade_out_scale` | 0.32 |
| Wind & air, Water, Nature, City, Space, Machines (advanced) | `audio.ambience_groups.<group>` | 0.93, 1.0, 1.0, 1.6, 1.2, 2.0 |

The Sound tab also shows *Ambience now*, the audible layers loudest first. It is the quickest way to tell a
wrong layer from a wrong mix while flying around.

## Where in the code

| File | Main functions |
|---|---|
| `src/simulations/Ambience.h/.cpp` | `Ambience::load()`, `evalTarget()`, `update()`, `tickEvents()`, `stateJson()` |
| `src/simulations/SatelliteSimAmbience.cpp` | `initAmbience()` (registers the drivers), `computeAmbienceContext()` (including the rain, wind and tornado drivers), `updateAmbience()`, `updateThunder()`, `thunderStep()` (the sound shells and the rocket voices) |
| `src/simulations/SatelliteSimLightning.cpp` | `addThunderSource()` (a flash's channel as a sound source) |
| `src/simulations/SatelliteSimRain.cpp` | `surfaceWindAt()`: the surface wind and gust fronts behind `gust`, `surface_wind` and `rain_driven` |
| `src/simulations/SatelliteSimLaunches.cpp` | `launchSounds()` (each rocket's level, distance and pan at its retarded time) |
| `src/AmbientSynth.h/.cpp` | `AmbientSynth::create()`, one class per kind (`ThunderSynth` also plays the rockets) |
| `src/AudioSystem.h/.cpp` | `addAmbienceLoop()`, `addAmbienceSynth()`, `playAmbienceOneShot()`, `renderWav()` |
| `tools/make_ambience.py` | the samples and `CREDITS.txt` |
| `tools/sound_tool/main.cpp` | `SoundTool` |
