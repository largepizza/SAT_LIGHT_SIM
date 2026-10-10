---
pages: [sound/ambience.md, rendering/clouds/weather.md, development/harness.md]
code: [src/simulations/SatelliteSimAmbience.cpp, src/simulations/SatelliteSimLightning.cpp, src/AmbientSynth.cpp, assets/sound/ambience/ambience.json, shaders/sat_sky.frag]
---
## Now
**Thunder is streamed from sound shells.** Every lightning flash's channel is kept as a sound source (its pieces'
positions, directions and lengths in Earth-fixed axes; up to 48 flashes, 150 s of sim time each). Its sound leaves at
the return stroke and travels at 343 m/s in sim time. Each frame `thunderStep` finds the pieces whose shell crossed
the listener between the last frame and this one — whether time or the listener moved — and sends the synth one
level per sounding flash: sqrt(channel length x weight / distance^2 per second), with the mean distance and the
bearing (pan). So the roll is the channel's geometry as heard from where the listener is now: flying toward a storm
it arrives sooner and compressed (at most 3x), outrunning the sound it never arrives, paused nothing arrives, a
teleport or a jump of more than 20 s of sim time arrives silently. Levels fade out over 20-40 km. The synth
(`thunder`, 8 streamed voices) is pink + brown noise under that level with a ~10 Hz flicker and 0.3-1.2 s peals,
low-passed by distance (about 3 kHz at 1 km, 1.1 kHz at 5 km, 650 Hz at 10 km, 350 Hz at 20 km), a crackle within
4 km, and a shared low rumble that decays over ~2 s. The ambience driver `thunder` is 1 while a roll is arriving,
about to (~2 s) or ended within 4 s; the layer is gated on it, so "Ambience now" lists thunder only when there is
some. Rain and thunder are both procedural: there are no sample files for them.

Harness: `lightning` lists `thunder_sources` (age, distance, extent, `arrives_in_s`, pieces, heard),
`thunder_pending`, `thunder_level`, `thunder_rolls`. `audio record` carries the shells on its own clock, so the
recipe is `audio force thunder 1`, `lightning spawn ...`, `wait 3`, `audio record <name> seconds=<arrival + roll>
solo=thunder` (no `time add`); put `time add 200` between tests to retire the previous source.

Measured (solo, layer gain 1, loudest second, dBFS RMS): a ground stroke 1.5 km off -16, 5 km -19, 12 km -25;
a 3 km stroke in the full mix is about 10 dB over the music.

**City lights under cloud.** The procedural city lights (glitter, street posts, road dots) give way to their own
local mean as the cloud and fog in front of the ground thicken: all mean at a transmittance of 0.02 or less, all
points from 0.35 (`gCityFlat` in `sat_sky.frag`). The total light is the same; the composite then attenuates it.

## History
Thunder was a whole envelope computed at the flash for where the listener stood then, played at that distance /
343 m/s: a player moving at many times the speed of sound heard it at the wrong time from the wrong side. Its noise
was brown (a ~30 Hz corner) low-passed at 360 Hz from 5 km and 100 Hz from 20 km: sub-bass only, close to inaudible
on small speakers, and the layer's gain was always up, so the Sound tab listed thunder as playing everywhere below
12 km. At the user's snapshot (Anchorage, 3.1 km, 2026-10-10) the nearest of 13 flashes in 40 s was 27 km away.

A glitter point carries a cell's whole light in one pixel, hundreds of times the map's value, so points and road
dots stayed crisp through a deck whose transmittance was well under 1% once the under-cloud blur of the night MAP
was removed (2026-10-06, the grey wash from orbit). A cap on the points' peak was tried first: dots at the map's
value on a dark ground still read as points.
