---
pages: [sound/music.md, sound/ambience.md]
code: [assets/sound/ambience/ambience.json, src/simulations/SatelliteSim.cpp, src/simulations/SatelliteSimAmbience.cpp, src/simulations/SatelliteSimLaunches.cpp, src/simulations/SatelliteSim.h]
---
## Now
The upwell stem has one more source in `music_upwell`: `beam_near`, the converged Reflect beams whose target site
is within 5 km of the eye (3-D distance; full to 5 km, smoothstep to none at 12 km; each beam weighted
1 / (1 + (aim error / 10 deg)^2)), eased 1.5 s. Map 0.5 -> 0, 2 -> 0.6, 6 -> 0.9, 15 -> 1. Standing in a lit beam
site therefore swells the music whichever way the camera looks; it fades as you leave (the stem's 6-s fade-out).

Settings > Sound "Sounds arrive instantly" (`audio.sound_instant`, default on): thunder, a rocket's roar and its
ignition / staging thumps start 0.15 s after their event at any distance (the speed of sound is ignored; a thunder
roll still lasts as long as its channel's path differences). Off: they travel at 343 m/s, capped at 3 s by
"Distant sounds arrive within 3 s" (`audio.sound_quick`, shown only while instant is off), or uncapped.

## History
2026-11-24 (issue_20261124_190033): in a beam site the upwell never rose, because its only beam source was the
mirrors glaring on screen (`glare_n`), and in a site the mirrors are overhead while you look at the lit ground.
2026-11-24 (issue_20261124_192804): instant sound became the default, ahead of the 2026-11-21 3-s cap.
