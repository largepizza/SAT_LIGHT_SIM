---
pages: [rendering/clouds/lighting.md]
code: [shaders/cloud_v2_lightning.comp, shaders/include/cloud_lightning.glsl]
---
## Now
The cloud-over-ground map (`cv2Vt`, 5 nested 32x32 levels, cells 1 km x 4^level) reads the cloud field at the same footprint on every level: the cell / 8, clamped to 100-500 m. Levels 2-4 (16-256 km cells) take four jittered columns a visit, levels 0-1 two. The ground's sky light under cloud and the deck's ambient on clouds below it read this map, so every level agrees on the mean cloud optical depth.

## History
Issue 20261029_174506 (700-1000 km up): a dark square followed the viewer, and dark splotches moved when flying. The footprint had been cell / 8 without a cap, so a 16 km cell read the field at 2 km (up to 32 km at the top level) and lost the cumulus and erosion optical depth that the 500 m read of level 1 keeps. Level 1's window (+-64 km) was visibly darker than the level-2 ground around it, and the window moved with the eye. `ground_overcast 0` removed the square. After the change the square is gone; GPU cost within noise (7.98 / 8.64 / 7.93 ms old vs 8.43 / 7.83 / 7.97 new at the report's view).
