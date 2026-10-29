---
pages: [using/controls.md, development/controls-and-settings.md, rendering/satellite-meshes.md]
code: [src/simulations/SatelliteSim.cpp, src/simulations/SatelliteSim.h, src/simulations/SatelliteSimUI.cpp, src/simulations/SatelliteSimTutorial.cpp, src/simulations/SatelliteSimHarness.cpp]
---
## Now
Go to moves the observer to the satellite. While following, the observer is the camera: the selection panel's
magnitude, the Info window's sky position and brightness, the 3D view's "you" marker and Observer preset, and the
"highest in your sky" picks all describe what is seen from the camera. Only the last ground position is remembered,
for Exit and Return (the flight home) and for settings.json.

With a satellite selected, I opens or closes its Info window, G goes to it (and G again leaves Go to where the camera
is), and T turns Track on or off. "Select Satellite" (the centre pick) has no key; a click selects, and A on a
controller still picks at the centre. The selection buttons' tooltips name their keys ("Info (I)", "Go to (G)",
"Track (T)"), and the tutorial's cards do too. Saved bindings are migrated once (`controls.key_layout` 2): a saved T on
"Select Satellite" is dropped, the renamed "Go To / Leave Here" keeps the key saved under "Leave Go To Here", and I / T
keep their defaults only where no saved binding uses them. Harness `ui key` takes any letter.

## History
2026-10-28 (issues 20261028_223234 "Goto mag", 20261028_223530 "Hotkeys"): until then the readouts in Go to used the
parked ground observer (a "ghost" observer left on the ground), so a satellite 34 m from the camera read "Mag: below
your horizon"; and T was "Select Satellite", G only left Go to.
