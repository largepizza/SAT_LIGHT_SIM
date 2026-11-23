---
pages: [rendering/clouds/march.md, rendering/clouds/storms.md]
code: [shaders/cloud_v2_march.comp, shaders/cloud_v2_lightning.comp]
---
## Now
Inside a cloud (and under a storm with rain at the eye), the part of the Sun's shadow that is the sample's own cloud
reaches it as diffuse light, 1 / (1 + 0.11 tau) (Eddington's thick-slab transmission at g = 0.85, mu0 = 2/3), weighted
by the Sun's flux on the cloud's top: fMu = min(mu0 (2 + 3 mu0) 0.375, 1), so 1 with the Sun above ~42 deg and 0.013 at
+1 deg. A storm's underside at sunrise is dark (sky light only) instead of lit orange. The Sun transmittance volume
counts rain at its full extinction (it counted 15%): rain is ~0.36 /km at 1 mm/h, 1.6 at 10, 4.3 at 50 (Marshall-Palmer,
visibility 11 / 2.5 / 0.9 km).

## History
issue_20261122_214014 (2026-11-22, wave c20): at 3.6 N 61.2 E with the Sun +0.95 deg the whole underside of a storm was
flat orange. The Sun's path from the base had optical depth 63-72 in the Sun volume (> 190 marched finely), so the direct
Sun was already black; the light was the diffused own-cloud share at 12% of the full Sun with no flux factor. View
luminance 76 -> 41 at the shot, 107 -> 71 at +15 min. The 15% rain share dated from review 8's ground shadow (coarse reads
smeared the curtains over the map's rain area); along the Sun from that base the 4-km read carries less rain optical
depth than the fine one, and from 180 km the change is 1.1 levels.
