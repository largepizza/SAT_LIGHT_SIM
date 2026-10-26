---
pages: [using/features.md, using/controls.md, development/harness.md]
code: [src/VideoRecorder.cpp, src/VideoEncoder.cpp, src/FrameCapture.cpp, src/simulations/SatelliteSimVideo.cpp, src/simulations/SatelliteSimHarness.cpp (video, path play video=, wait settle auto, capture), tools/harness/tstab.py]
---
## Now
**Video export.** F6 ("Record Video", rebindable) or the video-camera button at the end of the bottom-left panel starts
and stops a recording: an H.264 .mp4 in `<user data>/videos/satlight_<date>_<time>.mp4`, with a REC badge and the
elapsed time at the top left while it runs and a toast when the file is written. Settings > Display > VIDEO: "Videos
include the UI" (off: the scene alone, while the player still sees the UI), frame rate 30 / 60, resolution 100 / 75 /
50% of the window. The video keeps the wall clock: a frame the app did not draw in time repeats the last one. On
Windows the encoder is the one that ships with Windows (Media Foundation; a hardware encoder where the driver has one),
elsewhere an ffmpeg on PATH; nothing is bundled. Recording costs ~nothing on the frame (the copy is read back two frames
later and encoded on a worker thread): at 1920x1009 the frame time and the GPU moved by 0-0.5 ms (within run-to-run
noise). ~105 MB a minute of motion
at 1080p60, ~40-50 MB of a still view, ~15 MB at 50% / 30 fps.

**Harness.** `video start <name> [fps=] [scale=] [ui=on]` / `video stop` (videos/<name>.mp4 in the run folder; frames,
drops, repeats, encode cost, size) and `path play ... video=<name>` (every path frame, lossless, no PNGs: ~4 s for a
2.5-s 1080p60 path against 30-40 s with `record=`). `wait settle auto [max=160]` stops a settle when the image stops
changing (a 1/4-size probe compared 8 and 32 frames back: a drift is waited out, the view's own flicker is not): ~50
frames in a cumulus, overcast or clear view; storm views and orbit still run to max. `capture` returns once the frame is
read back (the PNG encodes while the script goes on; `wait=1` = the old wait), and `state` / capture sidecars no longer
read the Sun transmittance volume back (`state sun_volume=1` does): a capture 0.7-1 s -> ~0.1 s. tstab.py writes only the
5 scored frames per scenario and settles with `auto` (`--settle N` = fixed, `--video` = an mp4 per path): a storm view's
two scenarios 134 s -> 57 s of commands, 546 MB -> 39 MB, same scores.

## History
Until 2026-10-25 the only way to see motion was `path play record=` (a PNG per frame, the app waiting on each copy) and
frames2video.py; one tstab view took ~39 min and 11 GB. Every `capture` waited for the PNG encode and built a state
sidecar that drained the GPU and read ~12 MB of device memory across the bus (~0.5 s); the screenshot readback copied
out of uncached memory (~25 ms of the frame thread per capture). `wait settle` was a fixed 40 / N frames.
