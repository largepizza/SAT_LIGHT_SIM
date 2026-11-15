---
pages: [development/ui.md, development/harness.md]
code: [src/simulations/SatelliteSim.h (kIntroKeyframes, kIntroArriveIndex, kIntroTitleIndex, kIntroTitleFovDeg), src/simulations/SatelliteSim.cpp (updateIntroCinematic), src/simulations/SatelliteSimUI.cpp (buildIntroOverlay), docs/subsystems/ui.md "Subsystem: Intro Cinematic (UC3)"]
---
## Now
The intro ends in three parts. The camera path (keys 0-7, the Cinematics spline) arrives on the Sun centred on the
limb at 638 km at 7 songbeats (53.27 s) and stops. It holds there for one songbeat (to 60.88 s): pose and look exact,
the sim clock held so the Sun stays centred, no caption (the last narrative line, "We will come to miss the quiet
sky", fades over the last second of the move). Then the "SAT LIGHT SIM" title card (one line), while the FOV widens from 89.35
to 105 deg over one songbeat (smoothstep in log FOV: monotonic, still at both ends) and the clock's rate eases from 0
to 1x; the title fades over the last second and the intro hands over at 68.49 s with the FOV at 105 and the clock at
1x. Not 120 (the old intro's end): from this framing the Sun's halo fades past ~105 deg (the sky's veiling glare
samples the cloud composite across the Sun's disc, which at that scale reaches the limb's clouds). A skip in the hold or the zoom hands over the camera as it is, clock at 1x. The harness's `intro state` reports
`phase` (move / hold / title / off) and the applied camera.

## History
2026-11-14 (issue_20261114_162721, "Intro final beat"): the reporter asked for the camera to stay still on the
Sun-centred framing a beat before the title card, and for the title card to get the old intro's FOV zoom-out. Before,
the title appeared at beat 6 (45.66 s) while the camera was still flying in, and the intro ended the moment the camera
stopped (53.27 s). The title also wrapped to "SAT LIGHT / SIM" (the caption panel's fit width measured short at
its size); it is one line now (no wrap). The intro is 15.2 s (two songbeats) longer: 68.49 s.
