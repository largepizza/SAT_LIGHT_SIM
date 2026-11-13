---
pages: [using/getting-started.md]
code: [src/AudioSystem.cpp, src/AudioSystem.h, src/VideoEncoder.cpp, src/VideoRecorder.cpp, src/simulations/SatelliteSimVideo.cpp, src/simulations/SatelliteSimUI.cpp, tools/harness/run.py]
---
## Now
Recorded videos (F6) carry a stereo 48 kHz AAC track. Settings > Display > VIDEO > Sound (`display.video_sound`):
Everything (default: music, ambience and UI sounds at their own volumes), Ambience only (no music and no UI sounds,
for laying your own music over the ambience), Off. The master volume is not applied, so a muted app still records
its sound. Cinematic exports and harness `path play video=` are silent in a run with a real sound device (they run
slower than real time). The sound comes from a tap node every bus passes through on its way to the speakers; live,
sound and video stay within one frame (8 s: 8.00 s video, 8.02 s sound). Harness: `video start ... sound=all|ambience|off`,
`run.py --sound` for a real device.

## History
Before 2026-11-12 videos were silent, and the live video clock ran a constant ~0.1 s behind the wall clock (frame 0
held for the time the encoder took to open).
