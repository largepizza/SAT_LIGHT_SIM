---
pages: [rendering/clouds/storms.md, rendering/clouds/march.md]
code: [shaders/cloud_v2_march.comp]
---
## Now
Inside a cloud (a ray whose cloud starts at the eye), the share of the Sun's shadow that is the sample's own cloud
reaches it as diffuse transmission 1 / (1 + 0.11 tau) instead of Beer's law. Rain alone below a cloud base (the
shafts and the scud in them) is not part of that cloud: it never takes the diffused share, and the cloud over it
shadows it by Beer's law. Its diffuse light comes from the sky probe and the "deck above" term.

## History
2026-11-24 (issue_20261124_184831 "Odd highlights on bottom storm clouds"): with the eye in rain under a day storm,
dense rain and scud lumps took the diffused share (their density x the column to the storm top reached the storm's
whole optical depth), lit at ~13% of the Sun: sharp white cauliflower patches on a dark base. Found by knockout
(the brute-force Sun-path reference showed none). Patch minus base +22.7 -> -4.7 levels; other views at the noise
floor; no cost.
