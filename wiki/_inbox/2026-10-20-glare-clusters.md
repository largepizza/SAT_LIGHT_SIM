---
pages: [rendering/points-bloom-glare.md, rendering/satellite-meshes.md, using/features.md]
code: [shaders/glare_cluster.comp, shaders/include/glare.glsl, shaders/glare.vert, shaders/include/glare_mesh_vert.glsl, shaders/sat_mesh.frag, shaders/cloud_march.comp, src/simulations/SatelliteSimLaunches.cpp]
---
## Now
Glare clusters: every source that glares this frame (satellite sprites, launch pad lights, rockets, fireballs, mesh
glints) is summed into a coarse screen grid (`glare_cluster.comp`; tile ~1/68 of the view height), weighted
(flare / threshold flare)^2 x visibility. Each glare keeps w^2 of its spikes, w = its weight over everything glaring
within 0.8 x its glare radius; its rays shorten and its halo grows a little as w falls. A lone source is unchanged
(measured <= 2 levels), the brightest of a group keeps most of its star, N equal neighbours each keep 1/N^2: a tight
cluster reads as a soft glow with at most a few spikes. Core and halo are never touched. Setting "Glare cluster merge"
(Photometry, `photometry.glare_cluster_merge`, default 1, 0 = every light its own star). Cost within noise.

Launches close up: a burning stage's nozzles glow (the inside of every bell, the rocket models' `soot` material); the
flame's Gaussians fade when the eye is inside one, keep their side-on surface brightness when seen end-on (and thin to
0.15 within ~40 deg of the axis from the exhaust side, so the 33 lit Raptors show from under a Super Heavy); the flame
core is x0.18 at night; the exhaust glow is 12 exit diameters low down (was 30), 0.12 of the flame (was 0.25) and fades
within 2-6 of its lengths; the flame's point flare blends to 14 once the flame is resolved. Emissive light is hidden by
a cloud only when the cloud is nearer, and a flame composites in front of its own mesh. Pad floods -3 mag at 1 km
(was -4), flare cap 14 (was 25); floodlit apron 0.2 x albedo (was 0.35).

## History
Issue 20261019_222938 ("Glares": clusters "spidery, sea-urchin-y"; Super Heavy from below a black disc with spikes over
a screen-filling white haze) and 20261019_222829 ("Plumes Again"). Causes found: the white haze was the flame tail's
Gaussian with the eye inside it (lit every direction at ~1/3 of its peak), not bloom, glare or the sky glow bins
(each knocked out in turn); the black disc was the stage mesh hiding its own flame (the cloud composite carried the
no-cloud distance, 60000 km, so sat_sky.frag skipped it on mesh pixels); the "black blob" was a cumulus ~2 km below the
launch's glow, which cloud_march.comp's cloud suppression (transmittance along the whole ray, cubed) cut out of the
glow although the cloud was behind it. Not built: clouds lit by a burning rocket.
