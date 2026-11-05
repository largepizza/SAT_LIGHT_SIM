---
pages: [rendering/clouds/march.md, development/tools.md]
code: [tools/cloud_micro_reference.py, tools/cloud_ms_reference.py, shaders/cloud_v2_march.comp]
---
## Now
`tools/cloud_micro_reference.py --below` path-traces the reference blobs and flat decks (a new `slab` blob: vertical
optical depth 0.5 x sigma0) with the eye straight under them, the Sun 63 or 24 deg up, and prints the droplet model's level
against the reference for the whole blob and its centre. A cloud base's brightness in the march comes from its optical depth:
the droplet model is 0.65-1.0x the path tracer at cumulus bases and 0.2-0.5x at deck bases, never brighter. A tau-15
cloud transmits ~45% of the sunlight, so its base is a light grey 3-4x the zenith sky. A dark base needs tau 50+.

## History
Issue_20261104_180745 ("Cloud Bottom with multiscattering": the bases are too bright and the clouds too transparent) was
measured, and nothing changed. In the report's view the visible base samples have an optical depth toward the Sun of 13-19
(a forced overcast: 16-23). An adiabatic cumulus of the same ~1.2 km depth has tau ~30-100, so the cloud field is 3-6x
thinner than real cumulus. `clouds_v2.density` 2 / 3 darkens the bases 10-35% and leaves the tops within 4%, at no cost.
That changes the look in every view, so it is left at 1 for the user to choose.
