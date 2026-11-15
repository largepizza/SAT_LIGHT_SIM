---
pages: [rendering/points-bloom-glare.md, development/gpu-conventions.md]
code: [src/simulations/SatelliteSim.cpp, shaders/flare_composite.frag, shaders/glare.frag, shaders/glare_mesh.frag]
---
## Now
The glare sprites (glare.frag for satellites, glare_mesh.frag for mesh glints) are drawn into their own
full-resolution RGBA16F target, `glareAccumImg`, in recordCompute right after the glare clusters. The flare
composite (flare_composite.frag) adds that sum to the frame in one write, after the bloom's soft knee, so the
glare is still not under the bloom's ceiling. The target is cleared every frame that satellites are on.

## History
Until 2026-11-15 each glare sprite was blended straight into the 8-bit sRGB swapchain, and every add was rounded
on its own. Where tens of sprites overlapped (a fresh launch train flaring at dusk), a halo tail under half a
level was lost and one over half counted as a whole level, so the pile summed into stepped rings with a grey rim
(issue_20261115_131830 "Flare glare banding"). At the report's view the seams went from 707 to 3, the area's
light rose 5.5% (the light the rounding had dropped), the whole frame 0.6%, and GPU time stayed within noise.
