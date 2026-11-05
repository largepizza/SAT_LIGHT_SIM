---
pages: [rendering/clouds/storms.md]
code: [shaders/include/clouds_v2.glsl, shaders/include/cv2_storms.glsl, src/simulations/SatelliteSimCloudsV2.cpp]
---
## Now
With storms through the cumulus field (`clouds_v2.storm_unified` 1) the anvil plate is the union of the storms'
shields: the shield walk (`cv2ShieldAtR`) takes, at each point, the shield with the largest PLATE margin
((presence - 0.5) x its thickness and age factors inside the plate), so overlapping shields join without a seam and
the plate's thickness is continuous. The plate's distance to its outline is that margin x 12 km (a gentle slope): the
storm lobes fray its edge over a few percent of its radius, and toward the outline the lobes shrink to half, so the
edge thins rather than standing as a wall of billows. The veil (thin ice) fades out across that edge (plate presence
0.42-0.55) and the cirrus gives way with it. Over a tower's head the plate's lid keeps only ~5% of the lobes (40%
elsewhere), and in this mode an overshooting dome rises 600 m more than "Cb overshoot" sets and is the tower's dense
cloud down its last km: the domes read as dark dots from orbit again. The storm-scale lobes rise at 1.5 x the wind:
the storm noise anchor slides along the short lattice vector of the noise tile nearest the observer's up (CPU side, no
shader cost; one jump when the observer's up passes to another lattice vector).

## History
2026-11-04 (worktree storm-polish), from the storm-rework sheets: a dark channel across issue 111947's anvil from
14.5 km (the walk took the strongest shield by a wider fade than the plate's, so a smaller shield won inside a bigger
plate where it stood outside its own); a mesa-like plate edge; weak domes (140 m above a lid with +-600 m lobes); towers
without their rising lobes. Tried and dropped: fraying the plate's outline with the cluster noise as the veil does
(+-0.12 presence: the fringe reached 111947's eye at 11 km, which stood in fog, and it cost ~3 ms over a plate); a
flow-map cross-fade in the shader (two reads), a bool parameter on the walk, or a second running choice in it — each
put the march over NVIDIA's 128-register cliff (244-255 registers); the shipped forms stay at 128.
