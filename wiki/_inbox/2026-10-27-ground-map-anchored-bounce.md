---
pages: [rendering/clouds/march.md]
code: [shaders/cloud_v2_lightning.comp, shaders/include/cloud_lightning.glsl, shaders/cloud_v2_march.comp]
---
## Now
The cloud-over-the-ground map (cv2Vt / cv2VtUp: 5 nested levels of 32 x 32 columns, cells 1 km x 4^level) keeps its
cells FIXED on an anchor plane: the 32 x 32 window round the eye's cell is stored toroidally (slot = cell & 31), each
slot carries a stamp (its cell, the anchor's epoch, a visit count in cv2VtStamp), so flying moves the window by whole
cells and only cells entering it restart their running mean (1, 1/2, 1/3 of the first visits, then 0.15). The anchor
follows the eye only past 500 km (level 0) .. ~1200-2000 km (levels 1-3), half a cell for level 4. Lookups of the
deck above a cloud (cv2VtUpCell) blend into the next level over a level's outer 3 cells, as the ground lookup does.
With "ground_overcast" on, a cloud's light from below is the map's mean direct + diffuse Sun on the ground (cv2Vt.w =
the columns' direct transmission) at the point where the Sun's ray from the ground crosses the sample's height
(h / tan(elevation) sunward, at most 30 km), x the old albedo (0.15 low .. 0.3 high) x (1 - hf)^2.

## History
2026-10-27, issues 20261027_002401 ("Lighting persistence") and 20261027_002051 ("Storm Cell Lighting"). The map was
centred on the eye and restarted a level whenever the eye had moved half a cell since its last visit; at a boost's
8.5 km/s levels 0-1 restarted from two jittered columns every visit (every 5 frames: frame-to-frame spikes of 5-11
levels mean on the ground of a fast flight over the Sahara, none after). cv2VtUpCell switched levels at the window's
edge, a hard wall 16 km from the eye (a lit/dark boundary across a storm). The bounce took 0.6 of the UNSHADOWED Sun
for samples 4-9 km up (kBounceOpen, the 2026-10-24 "Anvil darkness" fix): an anvil over a storm complex was lit light
grey from below while the Cb walls a few km lower, taking almost none, were dark blue. Now both take the light under
the storm; ordinary cumulus scenes unchanged, cloud march within noise (13.34 -> 13.43 ms at the report's view).
Open: the mid layer's altostratus sheet seen edge-on from near its height still draws a straight translucent band
across a storm (mid_amount 0 removes it).
