---
pages: [rendering/clouds/march.md, development/profiling.md]
code: [shaders/cloud_v2_march.comp]
---
## Now
A cloud-march sample below a cloud base with any rain share (a rain or snow shaft, and the fractus / scud band hanging in
it: `f.rain > 0 && f.hf <= 0`) stands for four coarse steps and, under the Sun, runs no light march: its key light is the
Sun transmittance volume (`cv2StvT`) read at the sample itself (the deck over it and the storm beyond), its optical depth
toward the Sun `sigma x light length` and away from it `sigma x 2 light length` (rain taken as a uniform medium); it still
reads the sky probe and the beams. Moonlit rain keeps the light march (there is no Moon volume). Shader switch
`CV2_RAIN_LIT` (`-D CV2_RAIN_LIT_OFF` = the old march, for A/Bs). Measured inside a snowing deck at 4.4 km (81.5 S
26.2 E, RTX 3070 Ti, 1600x900, the reporter's settings, rain_amount 3): cloud march 24 -> 14.5 ms, 19.5 -> 14.5 ms at
4.1 km; no change from orbit or in rain-free views. The rain reads ~1-1.5% darker on average.

## History
Before, every rain sample ran the full light march (7 steps + far sample + sky probe + 2 reads away from the light, ~11
field reads), and scud with under 20% rain share took fine steps and full light marches. Eval counters at the report's
view showed the light march's optical depth for rain was the deck's (8-38 km of the rain's own density), i.e. the shadow
the transmittance volume already holds. Shafts alone without the light march: only -12..-14% (warps wait on the scud
lanes); with the scud as well: -25..-42%. The 2026-10-04 objection to skipping the rain light march ("sunlight would
reach shafts under thick cloud") predates the transmittance volume (2026-10-22).
