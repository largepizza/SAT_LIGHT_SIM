---
pages: [sound/ambience.md]
code: [src/simulations/SatelliteSimAmbience.cpp, src/simulations/SatelliteSimLightning.cpp, src/simulations/SatelliteSimLaunches.cpp]
---
## Now
Each lightning flash's channel is a sound source whose front travels at 343 m/s of sim time. A playhead per flash
plays the channel's pieces in the order of their path difference behind the nearest piece, starting when the front
reaches the listener: it never runs back and never faster than 3x sound, so a roll is heard once and whole at any
frame rate, while flying (compressed at most 3x when flying in faster than sound) and across a pause. The delay is
capped at 3 s by default: Settings > Sound "Distant sounds arrive within 3 s" (`audio.sound_quick`, on); off gives the
full 3 s per km. The cap also applies to rocket noise. Up to 96 flashes are kept (the least relevant leaves first:
heard, else the farthest) on 8 synth voices; a flash with no free voice shares the voice nearest in distance.
Harness: `lightning thunder log|state|save|load|sim` keeps the flashes of a session and replays them on a virtual
clock (frame time, a moving listener, a hold) into a WAV.

## History
Until 2026-11-21 (issue_20261121_163317, "choppy", "does not play at all") thunder was the pieces whose arrival
distance changed sign between two frames, on a 48-source list that dropped its oldest and 8 voices given to the flash
"silent longest". Measured on the same kept flashes: 80 flashes in 24 s around a still listener: 50 of 80 heard
before (19 evicted by the cap, 11 without a voice), 80 of 80 after. In the report's own storm a still listener missed
nothing; the flashes there were ~20 km off (thunder 60-85 s later) and the reporter flew at 1.1-2.1 km/s with time
often held, which is why the delay is capped by default. A listener at 1 km/s heard 0.0 s of thunder in 60 s before,
42.6 s after.
