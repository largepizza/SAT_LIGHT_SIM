---
pages: [development/harness.md, development/ui.md]
code: [src/simulations/SatelliteSimHarness.cpp, src/simulations/SatelliteSimCinematic.cpp]
---
## Now
The harness command `cine preview <file> [shot=N] [scale=0.5] [fps=30]` loads a saved cinematic and records its preview
(rendered in motion at a fixed 1/fps, no settling, no motion blur) into one .mp4 at a fraction of the window size, the
whole cinematic or one shot (`<name>_shotN.mp4`). It is for checking framing and motion before an HQ export.

The v1.2 tour cinematic is `tools/cinematics/tour_v1_2.json` with its shot list in `tools/cinematics/tour_v1_2_shots.md`.

## History
2026-10-29: added for the v1.2 tour (workstream E of the v1.2 release plan).
