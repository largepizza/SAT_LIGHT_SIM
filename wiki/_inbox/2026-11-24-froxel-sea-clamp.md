---
pages: [rendering/atmosphere-and-sky.md, history/known-issues.md]
code: [shaders/cloud_v2_froxel.comp]
---
## Now
The god-ray froxel volume (cloud_v2_froxel.comp: 320 x 180 x 64 screen tiles x log-spaced depths, the clouds' and the
terrain's Sun shadow along the view, read trilinearly by the sky pass's atmosphere steps) never samples below the sea:
a froxel sample past its ray's sea-level-sphere hit takes the value at the surface. Land above sea level can still put
a slice's far samples inside a hill (the froxel pass has no surface of its own).

## History
2026-11-24, issue_20261124_185624 ("Concentric ring artifact", 53.7 km over the Gulf of Thailand, day): a slice is ~15%
of its distance deep; its samples past the sea read the terrain shadow map inside the Earth (T ~ 0), and the sky
pass's trilinear read at steps in front of the surface took a share of that darkness that grew across each slice and
dropped at the next: sawtooth rings of constant distance round the nadir, in the Mie haze (~10% of it, 2-6 levels).
Found by knockouts: in-scatter -> Mie -> god-ray shadow -> froxels -> terrain shadow. Ring measure (red, sea band
high-pass sd) 0.61 -> 0.23, the exact per-step direct path 0.24. The sky shaders are unchanged.
