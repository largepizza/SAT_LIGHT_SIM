# Music and tonality

This page covers the soundtrack side of the audio: the music player and its playlist, the *upwell stems*
that answer the glare on screen while a track plays, the analysis that finds each track's key, and how the
ambience's pitched voices follow that key. The ambience layers themselves are on [Ambience](ambience.md).

## The player

`AudioSystem` (in `src/AudioSystem.cpp`) streams one track at a time on the music bus.

### Playlist order

`SatelliteSim::setAudio()` builds the playlist from `assets/sound/music/`:

1. `gravity_wave` always first: the intro cinematic is cut to it, and *Replay Intro* restarts it.
2. Then `fuse`, `leo_motif`, `BIOS`, in that order (names compared case-insensitively).
3. Then any other `.mp3`, `.flac` or `.wav` in the folder, in case-insensitive name order.

A file whose name ends in `_upwell` is never a track: it is the stem of the track with the same name (see
[Upwell stems](#upwell-stems)). Dropping a new audio file into the folder adds it to the end of the playlist;
it is analysed on its first launch.

The track name shown in the UI is made from the file name: underscores become spaces, words are capitalised,
and orbit acronyms stay upper case (`leo_motif` is shown as "LEO Motif").

### Gaps, looping and controls

Tracks play in order and loop back to the first. Between two tracks there is a silent **gap**, 30 s by
default, so the ambience has room of its own. Pausing freezes the gap's countdown too.

The Sound tab's music row shows the current track with its position and length (`Name  1:23 / 4:56`), or
`Next: <name> in N s` during a gap, and `+upwell N%` while the upwell stem is audible. Its buttons:

| Button | Action |
|---|---|
| `<<` | restart the current track if more than 3 s in, else go to the previous track |
| `\|\|` / `>` | pause or resume (pausing keeps the position) |
| `>>` | next track now, with no gap |

### The altitude fade

The music fades with the camera's altitude, under the user's music volume: full up to 1500 km, half by
5000 km, silent from 35 786 km (geostationary altitude), linear in log altitude between. The fade moves at
most a quarter of full scale per second. High orbits belong to two ambience beds written for them
(`vlf_earth` and `firmament`). The intro tops out at 300 km, so it is never affected.

## Upwell stems

While a track plays, a bright sky (many satellites glaring on screen at once) is answered by the **music**,
not by a synth. A synth that tries to follow a recorded track in real time never fits its rhythm and harmony,
so each track has an **upwell stem**: `assets/sound/music/<track>_upwell.mp3` (or `.flac`, `.wav`), a separate
layer the composer wrote to sit over that track. It is not a remix of the track.

How it is played:

- **Sample-locked.** `AudioSystem::loadTrack()` opens the track and its stem together, and `startSynced()`
  starts both on the same engine frame: a start time 50 ms ahead of the mixer in the live app, so the audio
  thread cannot run between the two calls, or the current engine time offline. Pause stops both; resume
  seeks the stem to the track's cursor and starts both on one frame again. If the two read cursors ever drift
  more than 50 ms apart, the stem is re-seeked (a seek can click, so it is reserved for real drift). Both are
  assumed to have the same sample rate, which is how the composer exports them.
- **A tail.** A stem longer than its track rings out after the track ends, into the gap. Skipping cuts both.
- **Gain from the glare.** The stem's gain is the table's `music_upwell` block, a piecewise-linear map of a
  driver, clamped to 0..1:

```json
"music_upwell": {
  "driver": "glare_n",
  "in":  [1, 4, 20, 100],
  "out": [0, 0.4, 0.8, 1.0],
  "fade_in_s": 2.0,
  "fade_out_s": 6.0
}
```

`glare_n` is the number of flares glaring on screen (see [Ambience](ambience.md#beams-and-glare)). The gain
slides linearly toward its target: up to full in `fade_in_s`, down in `fade_out_s`. Between tracks it keeps
its value, so a glare that outlasts a track carries into the next track's stem. The stem plays on the music
bus, so the music volume and the altitude fade apply to it.

During the intro the stem stays silent, and replaying the intro takes it away within a second. The ambience's
own beam layers are gated on the `music_gap` driver and speak only while no track plays.

## Soundtrack analysis

`src/MusicAnalysis.cpp` reads each track once to find its key and pitch. The ambience's tonal root follows the
result, so every pitched ambience voice is in the music's key.

### What it measures

Each track is decoded to mono at 11 025 Hz and analysed with an 8192-point Hann-windowed FFT (1.35 Hz bins,
so semitones are resolved down to about 40 Hz) at a hop of 2048 samples. From the spectral peaks:

| Result | How |
|---|---|
| **tuning** | the track's deviation from A440 in cents: the circular mean of every peak's offset from the semitone grid, weighted by amplitude. The length of the mean vector is the confidence |
| **tuning curve** | the same over 8 s windows at 1 s steps. A piano whose pitch bends shows here as a slow swing of tens of cents |
| **chroma** and **bass chroma** | energy per pitch class from the peaks between 55 Hz and 2.5 kHz, with the local tuning removed first; the bass chroma is the same below 160 Hz |
| **key** | the Krumhansl-Kessler major and minor key profiles correlated against chroma plus half the bass chroma; the best correlation is the key and its confidence |
| **pitch set** | the 3 to 7 strongest pitch classes (the tonic always, others while at least 12% of the strongest): what the track actually plays, which is what a voice must stay inside to sit with it. A nearest named scale is kept for display only |
| **chords** | per 3 s segment, a cosine match against triad, sus and power-chord templates, merged while unchanged; the *home chord* is the one held longest |

Chroma-template harmony is rough by nature: sus4 and sus2 on different roots contain the same notes, and a
sustained pad blurs a progression. That is acceptable here because a chord is only ever used as a **set of
pitch classes**, and ambient material (drones, pedals, slow pads) only needs the set.

### The cache

Analysis takes roughly a tenth of a second per minute of music on one core, plus the MP3 decode. Results are
cached per track as `<user data>/music_analysis/<track>.analysis.json`, keyed by the file size, a 64-bit
FNV-1a hash of the file's contents, and the analysis version (`music::kAnalysisVersion`). A content hash is
used rather than the file's modification time because the build copies the music next to the executable on
every build, which would change the time and force a re-analysis.

`music::Library` analyses the playlist on a worker thread at startup; until a track is analysed the ambience
uses the fallback key. A [harness](../development/harness.md) run analyses synchronously, so a script hears
the same key every time. The first cache directory is where new results are written; the executable's own
`music_analysis/` folder is a read-only fallback, which is what a harness run (whose user data folder is its
run folder) finds warm.

`SoundTool --analyze <tracks>` runs the same analysis from the command line and prints key, tuning curve,
pitch set and chord timeline, writing the JSON to `--out <dir>` (default: the current directory):

```bash
cmake --build build --target SoundTool
build/Debug/SoundTool.exe --analyze assets/sound/music/gravity_wave.mp3
```

## Tonality: the ambience in the music's key

Every pitched ambience parameter is a multiple of one frequency, the **tonal root**, and the glare pad also
receives three pitch-class masks. `SatelliteSim::updateTonality()` decides them each frame:

| Situation | Root | Scale mask | Chord mask |
|---|---|---|---|
| A track is playing | the track's tonic, at the tuning curve's pitch at the current playback position (or the whole-track tuning if `follow_tuning_curve` is off) | the track's pitch set | the chord at the playback position (the home chord between chords) |
| In the gap between tracks | a glide from the ending track's tonic (at its final tuning) to the next track's, over the middle of the gap (`gap_glide`) | switches from the old track's to the new track's halfway through the glide | the respective home chord |
| Key following off, or no analysis yet | the Sound tab's *Tonal root* | `fallback_scale` | root and fifth |

Masks are **relative to the root**: bit \(k\) means \(k\) semitones above it.

The root is kept inside `root_range_hz` by octaves. The octave is chosen when the *source* changes (a new
track, entering a gap): it is the octave nearest the root as it currently is, then kept inside the range.
A new track is therefore a glide of at most a tritone, never an octave. In a gap, the next tonic is also
moved by octaves to within a tritone of the last. Finally, the root never moves faster than
`slew_semitones_per_s` (it eases toward its target with a 0.35 s time constant under that cap), so even a
skipped track glides.

With a track playing, the root therefore bends with the music: a track whose piano drifts in pitch carries
the ambience's drones along with it.

### The tonality block

```json
"tonality": {
  "fallback_scale": [0, 2, 3, 5, 7, 8, 10],
  "tension": [1, 6],
  "root_range_hz": [36, 96],
  "gap_glide": [0.15, 0.85],
  "slew_semitones_per_s": 1.5,
  "follow_tuning_curve": true
}
```

| Field | Meaning | Default |
|---|---|---|
| `fallback_scale` | intervals (semitones above the root) used when no analysis applies; the root is always included | Aeolian, `[0, 2, 3, 5, 7, 8, 10]` |
| `tension` | the tones only the glare pad's last voices reach; against the music's own notes they rub | minor second and tritone, `[1, 6]` |
| `root_range_hz` | the root is kept in this band by octaves; must span at least an octave | `[36, 96]` |
| `gap_glide` | the fraction of the gap over which the root glides (start, end) | `[0.15, 0.85]` |
| `slew_semitones_per_s` | the fastest the root may move | 1.5 |
| `follow_tuning_curve` | ride each track's tuning curve rather than its single tuning value | true |

The Sound tab shows the result on one line: `Key: <key>, <chord> | root <Hz> | <source>`.

## Settings

| UI label (Sound tab) | `settings.json` key | Default |
|---|---|---|
| Master vol | `audio.master_vol` | 0.4 |
| Music vol | `audio.music_vol` | 0.7 |
| SFX vol | `audio.sfx_vol` | 1.0 |
| Music gap (s) (advanced) | `audio.music_gap_s` | 30 |
| Key follows the music (advanced) | `audio.ambience_root_follow_music` | on |
| Tonal root (Hz) (advanced; used only with key following off) | `audio.ambience_root_hz` | 41 |

The ambience's own settings (its volume, fade multipliers and group gains) are listed on
[Ambience](ambience.md#settings).

A muted harness run starts the music as usual (the engine has no device, so nothing is heard). The harness's
`audio music [next|prev|pause|play|end|state]` drives the player; offline a track never ends by itself, so
`end` ends it as if it had played out, which makes the gap and the key's glide through it reachable.
`audio tonality [wait]` reports the root (as played and its target), the source, key, chord, scale and the
tuning cents.

## Where in the code

| File | Main functions |
|---|---|
| `src/AudioSystem.h/.cpp` | `addTrack()`, `update()` (gap countdown, upwell drift check, tail reaping), `loadTrack()`, `startSynced()`, `setMusicPaused()`, `setUpwellGain()`, `setMusicFade()` |
| `src/MusicAnalysis.h/.cpp` | `music::analyzeFile()`, `music::Library`, `Analysis::tuningAt()`, `chordAt()`, `tonicHz()` |
| `src/simulations/SatelliteSim.cpp` | `setAudio()` (playlist and stems) |
| `src/simulations/SatelliteSimAmbience.cpp` | `startMusicAnalysis()`, `updateTonality()`, `updateAmbience()` (upwell gain, intro and altitude fades), `tonalityJson()`, `tonalityLine()` |
| `src/simulations/Ambience.h/.cpp` | `TonalityConfig`, `upwellTarget()` |
| `tools/sound_tool/main.cpp` | `SoundTool --analyze` |
