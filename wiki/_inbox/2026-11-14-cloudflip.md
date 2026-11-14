---
pages: [rendering/clouds/field.md]
code: [shaders/include/clouds_v2.glsl, src/simulations/SatelliteSimCloudsV2.cpp]
---
## Now
The low cloud field grows from nothing as the coverage grows from zero. Its threshold (cloud where field > 1 - coverage)
is raised by 0.75 and the cloud's margin (which the lobes, bubbles and turrets stand on) lowered by 2.7 x that at zero
coverage, both back to their own over the first 0.02 of coverage (`cv2ZeroCoverLift`; the threshold also in the far
column `cv2FarColumn`; settings.json `clouds_v2.zero_cover_fade` 0.02, no UI, 0 = off). A clear region of the cloud
map, whose coverage is the imagery's noise floor (0-10/255 under "Clear below" 0), is clear sky.

## History
2026-11-14 (issue_20261114_112011 "Clouds flickering on and off?"): any coverage above zero drew a whole fair-weather
cumulus field (the cells, morphology and small-cumulus lift take the field past 1), while zero drew nothing. Over a
clear map region (the JPEG's 0-10/255 under "Clear below" 0, plus the evolution's growth offset) the ~20-km coverage
fraction crossed zero back and forth as the weather evolution's two advected copies slid through their 6-minute
window, so the whole sky switched between cumulus and clear every 2-3 sim minutes, paused or at 5 min/s.
