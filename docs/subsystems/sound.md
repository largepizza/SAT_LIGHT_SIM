# Ambient sound and music

The ambience layer table, drivers, synth voices, tonality, music player, offline audio.

Part of the subsystem notes (`docs/subsystems/`, map in `AGENTS.md` §4). Moved verbatim from
`CLAUDE.md` on 2026-11-03. Do not read this file whole: `python tools/outline.py docs/subsystems/sound.md`
lists its sections and topics with line numbers. New notes go next to the section they belong to.

## Subsystem: Ambient sound (2026-09-25)

Location- and context-aware ambience on its own bus under the music (Settings → Sound →
"Ambience", `audio.ambience_vol`). Three layers, app-independent → sim:

| File | Role |
|---|---|
| `src/AmbientSynth.h/.cpp` | procedural voices as miniaudio data sources: `wind`, `surf`, `hum` (noise through resonances), `drone` (harmonic wavetable + phaser + whine + compressor cycle + soft status motifs), `pad` (the glare chorus: up to twelve hollow odd-harmonic voices placed on a ladder of chord/scale/tension roles from pitch-class masks, per-voice pitch warp, a bloom when a voice joins, an FDN reverb), `bass` (the beam-site pedal: a beating pair on 2× the root, its fifth, a sub, drive, a slow downward sag and throb), and the unused-by-default `saw` (the first beam sound, which read as an FPV drone), `chorus` (procedural VLF chorus / whistlers / sferics, flanged — replaced by the recorded `vlf_earth` loop), `beeps` (FSK bursts) and `disk` (seek clicks). `rain` and `thunder` (rolls triggered by the lightning flashes, see *Clouds v2*). Params are atomics set by the main thread, eased per 64-frame block on the audio thread (a `snap` param — a mask — is not eased); seeded xorshift, so a render is deterministic |
| `src/MusicAnalysis.h/.cpp` | the soundtrack's key: per track, the tuning and a tuning CURVE over time, chroma, key (Krumhansl-Kessler), pitch set, a chord timeline; analysed on a worker thread at startup and cached per track in `<user data>/music_analysis/` (see *Tonality* below) |
| `src/AudioSystem.h/.cpp` | the ambience group, voices (sample loop or synth) with per-voice gain, one-shots with pan, the music PLAYER (see below), and **offline mode** |
| `src/simulations/Ambience.h/.cpp` | the layer table: `assets/sound/ambience/ambience.json` → per-layer target gain from named drivers, eased over `fade_s`, voices created lazily, events scheduled |
| `src/simulations/SatelliteSimAmbience.cpp` | the sim's context drivers, computed each frame at the end of the planets block in `recordCompute` |
| `tools/make_ambience.py` | builds the CC0 sample FLACs (fetch, high-pass, crossfaded loop, loudness-normalise) and `CREDITS.txt` |
| `tools/sound_tool/` | `SoundTool` (EXCLUDE_FROM_ALL): `--analyze <tracks>` runs MusicAnalysis and prints key / tuning curve / chords; `--render <synth> --out x.wav name=v\|a:b\|t/v,...` renders one voice with keyframed params — levels and timbre without launching the app |

- **A layer is ramps, never switches.** `gain × group gain × Π when-ramps × max(any-alternatives)`,
  a ramp being `[a, b]` (smooth 0→1, reversed for a > b) or `[a, b, c, d]`. Every transition is a
  crossfade by construction. An unknown driver/param/synth kind is a LOAD error (logged to
  `satlight_log.txt`), not a silently muted layer.
- **Fades are LINEAR slews** (`fade_in_s` / `fade_out_s` per layer, × the Sound tab's global
  multipliers): silent to full in fade_in_s, back in fade_out_s, whatever the distance. The first cut
  eased exponentially — it never finished (a layer 4 s behind was still at −20 dB nine seconds
  later), so a jump from the ground to orbit dragged the crickets and birds up with it. Harness check:
  1.2 s after a jump to 400 km, crickets and ground wind are below 1%.
- **One key for every pitched voice.** A synth param written `{"root": k}` is k × the tonal root
  (which follows the music — see *Tonality*): the comms drone on 2× (110/220/330 Hz), the datacenter hum
  on 1× with its whine on 16× (880), their status motifs on just-intonation ratios (1, 9/8, 5/4, 3/2,
  5/3, 2) of 4× and 3×, the cabin and aurora hums on 1× and 2× — they overlap in LEO and stay
  consonant. The first cut's FSK beeps (2.4 kHz, squared-off) and disk clicks + fan noise read as
  brash and as audio glitches; they were replaced by the two drones.
- **Groups** (`"group"`: wind water nature city space machines) each have a gain on the Sound tab.
- **Rain (2026-09-28):** driver `rain` = the cloud field's rain rate at the eye (cloud_v2_march.comp ->
  `terrainFrame.w` -> `terrainFrameMapped[3]`, eased 2 s) — the value the on-screen streaks use; layer
  `rain` (group water) plays synth `rain`: a band-passed hiss, a low rumble for heavy rain and a Poisson
  stream of drop ticks (each its own resonance, level and pan), all scaled by `intensity`. Not yet
  loudness-calibrated against the -24 LUFS convention.
  **Gusts (2026-10-23):** drivers `gust` (the gust fronts' envelope the rain and snow particles are blown by,
  SatelliteSimRain.cpp `surfaceWindAt`: 0 calm .. ~1.5), `surface_wind` (its speed / 15 m/s) and `rain_driven` (the
  rain rate pushed up to +60% by a gust; the rain layer's `intensity` reads it). Layer `wind_gust` (synth `wind`,
  gain 0.25, near the ground) swells with `gust` (level, and its band 340 -> 620 Hz), so the roar rises with the gust
  the rain is seen to lean in. Solo ~-47 dBFS against a ~-32 dBFS ambience mix in a storm (measured at gain 0.16: -51): modest, the user's to judge.
- **Glare is answered by the MUSIC while a track plays (2026-09-27, the user's design).** Tuning a
  synth to the soundtrack in real time never fit its rhythm and harmony, so the composer wrote an
  **upwell stem** per track (`assets/sound/music/<track>_upwell.mp3` — a separate layer, not a remix:
  its waveform correlates ~0 with the track). `setAudio` pairs each track with its stem (a
  `*_upwell.*` file is never a track itself, nor analysed); `AudioSystem::loadTrack` opens both and
  `startSynced()` starts them on ONE engine frame (`ma_sound_set_start_time_in_pcm_frames`, 50 ms
  ahead of the mixer live, the current engine time offline); pause/resume re-seeks the stem to the
  track's cursor and restarts both the same way, and `update()` re-seeks it if their read cursors ever
  drift past 50 ms. A stem longer than its track (BIOS and LEO Motif, ~5 s tails) rings out after the
  track ends (`upwellTail_`); a skip cuts both. Its gain is the table's `"music_upwell"` block: a
  piecewise map of `glare_n` (1 → 0, 4 → 0.4, 20 → 0.8, 100 → 1), slewed linearly up over 2 s and down
  over 6 s by `updateAmbience` → `AudioSystem::setUpwellGain`, under the music bus (so the music
  volume and altitude fade apply). Verified in `ambience_beams.satcmd`: 1.0 facing the Reflect ring,
  0 looking away, and in the offline render the track and its stem sit at the same sample offset.
  **The upwell has more sources than the glare (2026-10-21, issue 20261021_002542 "Upwells": launches, firing
  engines, collision dust).** `"music_upwell"` takes `"sources"` (a list of driver maps) + `"combine"` (`max`, or
  `soft_sum` = 1 - prod(1 - g)); the old single-map form still loads. The drivers are equivalent apparent
  MAGNITUDES at the camera (30 = nothing) of what the visuals draw, listed each frame by the drawing code
  (`ambUpwellSource`, SatelliteSim.h; read and cleared next frame in `computeAmbienceContext`): `rocket_mag` burning
  engines (the flame's magnitude at 100 km + distance + extinction, no daylight suppression), `stage_mag` sunlit
  coasting stages / fairing halves (the body before its mesh fade), `fireball_mag` every glowing re-entry fragment
  (`fbDrawReentry`) + the ship / capsule entry glows, `dust_mag` each dust tube piece (K amp / r^2, lit and not behind
  the Earth). Each source counts when on screen (the glare's soft frame edge) or NEAR (full within 8 / 1.5 / 15 / 20
  km for rocket / stage / fireball / dust, none past 6x), so a watched or followed launch rises whichever way the
  camera looks; summed per kind, eased up 0.15 s / down 0.8 s in log flux. Maps: rocket -10 → 1, -8 → 0.85, -5.5 →
  0.4, -3 → 0; stage -13 → 1, -11 → 0.6, -8 → 0; fireball -7 → 1, -5 → 0.8, -3 → 0.4, -1 → 0; dust -8 → 1, -6 → 0.8,
  -4 → 0.4, -2 → 0. Measured (harness_runs/upwell, `audio state` -> `upwell` {sources: driver/value/gain, target}):
  Starship from 12 km beside the pad 0 before ignition, -9.5 → 0.97 at lift-off (gain 0.87 two s later), 1.0 by
  T+40 and still 0.99 looking at the ground; gone ~6 s after T+290; from 150 km (below the horizon, then -1 mag) 0;
  following a Falcon 9 -18 → 1.0; a retiree re-entry from 120 km -3 → -6.4: 0.4 → 0.94; a Starlink breakup seen
  overhead from the ground -6.4 → 0.84 for minutes, followed -16 → 1; a day-21 cascade night sky -1 .. -2 → 0. The
  intro rule and the 2 s / 6 s slews are unchanged. Open: dust and fireballs are not tested against cloud; a caught
  ship in its tower reads stage_mag ~-5 from 12 km (harmless at that map).
- **The beam layers speak only BETWEEN tracks**, under the `music_gap` driver: 0 while a track plays,
  a triangle over the silent gap (0 at its start, 1 halfway, 0 at its end — a generous swell, never a
  cut when the song ends), 1 when no music is audible (player off or paused, music volume or the
  altitude fade below 3%), slewed 1/4 per second. Both beam layers carry `"music_gap": [0, 1]`.
- **The beam sound is two layers (2026-09-27 rework).** The first cut (`beam_swell`, a saw stack on a
  `beam_view` driver — beams near the camera's line of sight) read as an FPV drone, and flying through
  the atmosphere put you next to beams you were not looking at, so it sounded the same almost
  everywhere. Now:
  - **`beam_glare`** (synth `pad`) is SCREEN-SPACE: its drivers `glare_n` / `glare_sum` count the flares
    actually glaring on screen, with the glare sprites' own test (`b = log2(effectFlare)/2` past
    `glareThreshold`, in the view with a soft frame edge). Source: `sat_flare.comp`'s bright-flare list
    (`oceanGlintBuf`, every satellite at effectFlare ≥ 1 with its ENU direction), copied each frame into
    host-visible `glareReadBuf` at the end of `recordCompute`; `computeAmbienceContext` projects it and
    zeroes the count after reading, so a frame whose flare pass was skipped reads an empty list. Not
    occlusion-tested (the sprite tests that per pixel). Range: **0 to ~470** — facing the Reflect ring
    from a lit site hundreds of mirrors glare at once (0.06 facing away, 0 from 120 km). One flare = two
    hollow voices (the chord's fifth and octave on 8× the root) that bloom and fade; more flares add
    voices down a fixed ladder (root, third, the ninth against the octave, fifth below, seventh, root two
    octaves down, sixth, then the tension tones), so the chord deepens and crowds with seconds; the
    combined strength grows each voice's pitch warp and opens the filter.
  - **`beam_site_hum`** (synth `bass`) is NOT directional: `beam_site` sums converged beams (aim error
    against the same 10° the cloud clustering uses) with a soft proximity to their SITE — 1 each within
    15 km of the camera, 1/(1+(excess/25 km)²) beyond. ~180 on the Topaz site, ~10 from 120 km. It is the
    bottom of the sound: a pedal on the tonic that grows drive, sub, throb and a downward sag with it.
  Both are mapped with multi-point mods (`"in": [0, 1, 3, 10, ...]`), since a linear ramp saturated on
  the first hundred flares. A param that is BOTH a `params` entry and a `mod` target is overwritten by
  the root write every frame (`Ambience::update` applies mods, then root and tonal params).
- **Tonality: the ambience is in the music's key (2026-09-27, `MusicAnalysis`, `updateTonality`).**
  Kept for the pitched beds (LEO cabin, comms and datacenter drones) and the between-track beam pad;
  the glare during a track is the upwell stem, not a synth.
  Every playlist track is analysed once (`music::Library`, worker thread; SYNCHRONOUS in a harness run so
  a script hears the same key every time) and cached as `<user data>/music_analysis/<track>.analysis.json`
  keyed by file size + write time + `kAnalysisVersion` (the exe folder's cache is a read-only fallback,
  which is what a harness run finds warm) — a track dropped into `assets/sound/music` is analysed on its
  first launch. Mono 11025 Hz, 8192-point FFT (1.35 Hz bins), spectral peaks → **tuning** (the circular
  mean of each peak's offset from the semitone grid) and a **tuning curve** (8 s windows, 1 s steps),
  **chroma** with each frame's local tuning removed, **key** (Krumhansl-Kessler on chroma + bass/2), the
  **pitch set** (3-7 strongest pitch classes) and a **chord timeline** (3 s segments, triad / sus / power
  templates). ~1 s for all three tracks. gravity_wave: Ab (1 2 3 4 5, Absus4) with its piano's bend
  visible as a ±30-cent swing of ~50 s period in the curve; fuse: G minor (+6 cents, steady); leo_motif:
  C major (−19 cents). Chroma-template harmony is rough (sus4 and sus2 on different roots are the same
  notes), which is why a chord is only ever used as a SET of pitch classes.
  The runtime: while a track plays the root is its tonic at the curve's pitch where the music is (the
  ambience bends with gravity_wave), the scale its pitch set, the chord its chord at the playback
  position; in the gap between tracks the root glides from the last track's to the next one's tonic over
  `tonality.gap_glide` of the gap (masks switch halfway); the octave is chosen when the SOURCE changes
  (nearest the current root, then kept in `root_range_hz`), so a new track is at most a tritone's glide,
  and nothing moves faster than `slew_semitones_per_s`. Synth params `{"tonal": "scale"|"chord"|
  "tension"}` receive the masks, RELATIVE to the root; `tension` (b2 + #4 by default) is reached only by
  the pad's last voices — the "beautiful but deeply uncomfortable" part, over the music's own notes.
  Off (Sound tab "Key follows the music", `audio.ambience_root_follow_music`), or before the analysis is
  ready, the root is the Tonal root slider and the scale `tonality.fallback_scale` (Aeolian).
- **Drivers are the listener's, i.e. the CAMERA's** (follow mode: the camera, not the parked
  telescope): `alt_m agl_m ground_m` (GPU ground, like harness `state`), `lat_deg lon_deg`,
  `sun_el_deg`, `ocean_near/_wide` (`oceanMaskCpu`: a full-resolution 1-bit mask from the DEM —
  the 18 km/px `earthElevCpu` made every coastal town half ocean), `veg forest desert ice`
  (classified from a 2048×1024 copy of the DAY MAP, so the sound agrees with the ground as drawn;
  the map is a dry-season mosaic, the Great Plains read tan — "not desert, not ice" is the grassland
  test), `urban` (night lights, the dome's response curve), `beam` (the `groundBeams` Gaussians at
  the origin: ~1.8e6 in a lit spot's core), `glare_n` / `glare_sum` / `beam_site` (the beam sound —
  see above), `aurora` (oval band only), `wind` (value noise over
  ECEF direction and sim time — the jet stream and the alpine/desert/ice/sea winds), `cloud` (the 2D
  coverage map's value overhead: bilinear over the 1024×512 `earthCloudsCpu`, drifted by the same
  `cloudPhase` the surface overlay uses and eased over 1.5 s so a Go to cannot step it — `wind_ground`
  follows it, a full bed under cloud and a light breeze in clear air, so the wind is loud in some
  places and quiet in others), `time_scale`, `following`, `intro`, and per shell group
  `<g>_count` / `<g>_near_m`, and `speed_mps` / `eas`: the camera's ECEF speed and its
  equivalent airspeed `v·√(ρ/ρ0)` (8.5 km scale height), eased over 0.3 s. **No layer uses them:** a
  `wind_rush` layer on `eas` was removed at the user's request — it swamped every other layer
  whenever the camera moved and lingered — so don't re-add movement sound without asking. A jump of > 20 km in one
  frame (observer, Go to) counts as no motion, so a teleport never whooshes; time warp alone moves
  nothing in ECEF, and orbital speed in vacuum gives eas ≈ 0.
- **Shell proximity is closed form, not a roster search.** Walker density at latitude φ is
  `N / (2π²R² √(sin²i − sin²φ))`, RandomShell uniform over the band, count within hearing range
  `σ·π·(H² − dr²)`, nearest `√(dr² + (0.5/√σ)²)`. **A Disk is not one plane:** each SSO ring's
  inclination follows its own altitude, so across the AI disk's 1400 km the outer rings are tilted
  hundreds of km from the inner ones at that radius. The code takes the plane of the ring nearest
  the camera (its first satellite, in `initConstellation()`'s ring order). The first cut used one
  plane and read "0 satellites" from inside the disk; a second bug clamped the ring spacing to ≥ 1 km
  when the AI disk's is 0.7 km, which picked the wrong ring. **The deployment era (2026-10-21):** N is only the
  satellites there now — `rosterRangePresentFrac(i0, i1, t)` (each range's sorted born / gone seconds from
  rosterLife, cached until it changes; 1 with the era off) per constellation, and per filed group of a Shells entry
  (Starmind fills its shells one after another): in 2020 the Starlink drone is Gen1's first fills, not the 2036 shell.
- **Offline mode (harness).** A muted harness run initialises the engine with NO device
  (`App::run` → `AudioSystem::init(offline)`): nothing is audible and nothing is mixed until
  `audio record` pulls the graph synchronously (`renderWav`, with a 0.25 s discarded pre-roll and
  per-1/60 s event ticks). Offline renders outrun miniaudio's streamer — the music came out with a
  0.25 s hole every second — so offline, streamed sounds are DECODED instead (music on the first
  recording that asks for it, loops at creation). In the live app loops STREAM. A voice is created
  the first time its layer becomes audible, **on a worker thread** (2026-10-21, `AudioSystem::addAmbienceLoopAsync`
  / `addAmbienceSynthAsync`: the handle at once, the voice silent until made, then it takes the gain set meanwhile;
  `voiceSynth()` is null until then, and `Ambience` resolves the synth's parameter indices once it is up). Made on the
  main thread it was a 5-50 ms hitch each time (~50 ms of the first frame). `renderWav` and `clearAmbience` wait for
  voices still being made, so an offline recording hears every voice asked for before it.
- **Levels.** At level 1, gain 1 and the default bus volume every synth kind measures ≈ −24 LUFS
  (`ambience_solos.satcmd`); samples are normalised to −24 LUFS. The table's gains then set the mix
  so each tour stop totals ≈ 10 dB under the music (≈ −35 against its ≈ −25 LUFS). No tonal
  "melody" content: hums are noise through resonances, beeps are data bursts.
- **Every synth output is high-passed** (wind 45 Hz, surf 55 Hz, disk 60 Hz, hum 32 Hz, saw 40 Hz, pad
  110 Hz, bass 28 Hz) and the
  sample beds per source in `make_ambience.py`: the first wind was a sub-bass wall (brown noise
  through a gentle band-pass) and its gusts were a linear swing that measured flat — gusts are a
  swing in dB now.
- **Samples are FLAC** (gapless loops; MP3 encoder padding breaks a loop seam, and miniaudio has no
  Vorbis). Loops at 32 kHz, 36-42 s. Sources are Freesound CC0 previews (128 kbps); a lossless
  original dropped into `build/ambience_cache/<id>.wav` is preferred on the next run.
  **Two are the user's own (FL Studio), not generated — don't normalise or regenerate them:**
  `vlf_earth.flac` (real whistlers, MEO: in over 1500-3000 km, out over 25000-50000 km) and
  `firmament.flac` (a pad on one long ramp from 1000 km to full at 40000 km, 8 s fade-in). Both are
  44.1 kHz, 55.2 s — the length of `leo_motif.mp3`, the music track written alongside them. They are
  not at −24 LUFS (vlf_earth −20, firmament −29): the table's gains (0.16, 0.42) compensate.
  `make_ambience.py` credits them in `CREDITS.txt` (`ORIGINAL_NOTE`).
  The jungle pair is one recordist's Amazonian set — `jungle_day` (676675) and `jungle_night` (868746,
  a night frog and toad chorus), both by felix.blume (2026-09-26); the first jungle day recording
  carried people talking, which is why the day bed and a night twin were re-sourced together.
- **Music player** (`AudioSystem`): the playlist is `assets/sound/music/` in the composer's order —
  `gravity_wave` ALWAYS first (the intro is cut to it; Replay Intro restarts it), then `fuse`,
  `leo_motif`, `BIOS` (`kOrder` in `setAudio`, case-blind) — then any other mp3/flac/wav in
  case-blind name order (`*_upwell.*` excluded: each is its track's stem — see the glare bullet above).
  **The intro belongs to Gravity Wave:** no upwell while `showIntro` (a replay drops it within 1 s),
  and the whole ambience bus at half (`AudioSystem::setAmbienceFade`, `ambIntroFade`, back over 2 s
  when the intro ends). The Sound
  tab's track line shows `+upwell N%` while a stem is up. Between tracks a silent gap (`musicGapS`, 30 s, Sound tab advanced) gives the ambience
  room; pause freezes the gap too. Sound tab: current track + position (or "Next: … in N s"), `<<`
  (restart if > 3 s in, else previous), `||`/`>`, `>>`. `loadTrack` must not call `stopMusic()` — that
  also switches the player off. **Music fades with altitude** (`updateAmbience` →
  `AudioSystem::setMusicFade`, a multiplier under the user's music volume): full to 1500 km, half at
  5000 km, silent from 35786 km (GEO/HEO), linear in log altitude, slewed 1/4 per second — the high
  orbits belong to `vlf_earth` / `firmament`. The intro tops out at 300 km, so it is unaffected.
- **Sound tab**: under "Ambience now", the key line (`tonalityLine()`: key, chord, root, source).
  **Advanced**: "Key follows the music" (on), music gap, fade in/out multipliers, tonal root (used only
  with the key not following), six group gains —
  `buildCloudSliderRows(..., marksPreset=false)` at slot idx 99-108 (the slot arrays are 112 now;
  idx 110/111 are the satellite ocean-glint pair below, rendered from the Ocean tab).
- Verification: docs/HARNESS.md "Ambient sound" — `audio state/record/expect/force/music/tonality`,
  `ambience_tour.satcmd` (self-checking), `ambience_beams.satcmd` (the glare drivers read back per
  camera heading and the site hum by distance, then the beam voices recorded), `ambience_tonality.satcmd`
  (the key per track and the glide through a gap — `audio music end` ends a track offline),
  `SoundTool --render/--analyze`, `imgtools.py audio` (spectrograms, LUFS). How it SOUNDS is the user's
  to judge.

---
