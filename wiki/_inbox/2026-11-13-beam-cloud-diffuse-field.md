---
pages: [simulation/reflectors.md]
code: [shaders/cloud_v2_march.comp]
---
## Now
A reflector beam lights cloud with the same multiple-scattering model as the Sun and Moon: the delta-Eddington
slab's diffuse field (`cv2DiffuseField`, used when `cv2.ms.x` is 1), with the depth on the far side taken as the
path to the column's base along the beam (no extra field reads). A cloud deck under a beam, seen from above,
returns white light from its tops with their relief. The beam's level stays physical irradiance x "Beam light on
cloud" (30); the Moon's display level is ~2400x physical, so a beam still reads dimmer than a moonlit deck.

## History
2026-11-13 (issue_20261113_183043 "Reflect cloud lighting"): the beams kept the older Wrenninge octaves after the
Sun and Moon moved to the diffuse field, so a beam-lit deck was a flat blue wash, brightest toward the satellites.
Measured at the report's two shots (RTX 3070 Ti, 1600x900): lit tiles +4-6 luminance levels looking down; cloud
march within the A-vs-A spread; 128 registers before and after.
