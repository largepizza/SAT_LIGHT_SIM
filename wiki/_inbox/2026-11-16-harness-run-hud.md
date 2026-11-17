---
pages: [development/harness.md]
code: [src/simulations/SatelliteSimUI.cpp, src/Harness.h]
---
## Now
A harness launch draws a run HUD in the window's top left for the person watching: the run folder and script,
progress (statements run / known, errors, elapsed wall time, app frame), the statement executing now with its
`file:line`, and the last statement finished (red with its error when it failed). It is never captured: clean shots
skip the UI, and the HUD hides itself on `capture ui=on` frames, during `ui=on` video recording and on `ui dump`
frames. Hidden while the console is open; absent outside harness launches. No command or setting controls it.
