---
pages: [development/ui.md, development/harness.md]
code: [src/simulations/SatelliteSim.h (kIntroKeyframes, kIntroArriveT, kIntroTitleFovDeg), src/simulations/SatelliteSim.cpp (updateIntroCinematic), src/simulations/SatelliteSimUI.cpp (buildIntroOverlay), docs/subsystems/ui.md "Subsystem: Intro Cinematic (UC3)"]
---
## Now
The intro keeps its eight keys one songbeat apart (53.27 s; the "SAT LIGHT SIM" title at beat 6, 45.66 s). The
camera's progress along the path is remapped so it ARRIVES on the last key (the Sun centred on the limb, 638 km) at
`kIntroArriveT` (default beat 6, the title): over the songbeat before it, the camera covers the path from beat 5 to the
end (a cubic Hermite in path time, from the path's own speed to a stop: no velocity jump, an ease-out arrival, peak
2.8x the path's speed). Beats 0-5 frame what they did. From the arrival to the hand-off the camera is still and only
the FOV widens, 89.35 -> 105 deg (smoothstep in log FOV; the hand-off keeps 105). Not 120, the old intro's end: past
~105 deg the Sun's halo fades in this view (sky veiling-glare sampling, docs/rendering/FINDINGS.md #10). The sim clock
runs at the intro's rate throughout and is handed back at 1x. The title is one line. `kIntroArriveT = songbeat * 7`
gives back the old ending. The harness's `intro state` reports `phase` (move / still / off), `arrive_s` and the
applied camera.

## History
2026-11-14 (issue_20261114_162721, "Intro final beat"): the reporter wanted the camera still on the Sun-centred
framing before the title, and the old intro's title zoom-out. Before, the title showed at 45.66 s while the camera was
still flying in, and it stopped only on the hand-off frame (53.27 s); the title wrapped to "SAT LIGHT / SIM". A first
answer (136a85b) added a still songbeat after the last key and a title beat after it (68.5 s); the user: "the timing of
the key frames was fine, we just needed them to be accelerated in position so that the last beat would end on that
final shot". The second keeps the timing and accelerates the camera's travel instead.
