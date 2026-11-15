---
pages: [rendering/clouds/march.md]
code: [shaders/cloud_v2_march.comp]
---
## Now
The march measures each lit sample's view depth from where the ray entered the cloud it is in (`Tent`), and a ray whose
first dense sample lies within a few steps of the eye is treated as starting inside a cloud (the diffuse field's face
leak ~1, the Sun volume's own-cloud share diffused). Rain alone below a base is not that cloud: an entry set by rain is
kept apart (stored as `-2 - T`), and the first cloud-body sample after it is entered at its face. Rain samples are lit as
before.

## History
Until 2026-11-15 the rain at the eye under a raining deck set the "inside" state for the whole ray, so every part of the
deck seen through unbroken rain was lit as the cloud's interior and every part seen through a gap as its underside: at
sunset a huge lit area with straight edges and steps followed the rain region's outline (issue_20261114_234423, cluster
c02). Found by elimination (Sun volume, terrain shadow map, clipmap, base height all ruled out by knockouts). Cost: the
deck's first optical depths now take the light march like any deck face, cloud march +0.8 ms (19.7 -> 20.4) at the
report's view; 128 registers (a separate flag took the march to 255).
