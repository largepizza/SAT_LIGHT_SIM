---
pages: [rendering/clouds/march.md]
code: [shaders/cloud_v2_march.comp, tools/cloud_ms_reference.py]
---
## Now
A lit cloud sample's multiple scattering is the diffuse field of a conservative delta-Eddington slab along the light
(`cv2DiffuseField`): its depth toward the light is the light march's, its depth away from the light a short march the
other way (`ms_back_m`, 1200 m; the clipmap's mean extinction where it is baked; from a 150-m pixel up, the path to the
column's base). Toward the eye it is I0 + 0.3 g mu I1 plus the forward peak's extra light, attenuated for light escaping
sideways and for the leak through the face the eye looks at (the view's optical depth into the current cloud, averaged
over the sample's step). Thin wisps and cloud edges are dark, thick cores bright; the phase isotropizes with depth.
Settings (Clouds tab): "Multi-scatter model (0 octaves / 1 delta-Eddington)" `clouds_v2.ms_model` 1, "Multi-scatter
droplet g" `ms_diffusion_g` 0.85, "Multi-scatter diffuse strength" `ms_diffuse_gain` 1 (~1.6 = the path-traced level),
"Multi-scatter reach away from light (m)" `ms_back_m` 1200. "Powder" applies only to the octaves. Thin ice keeps the
octaves. Cost: +0.1..+0.8 ms of cloud march (RTX 3070 Ti, 1920x1009), less from orbit; noise and motion equal or better.

## History
Replaced Wrenninge-style octaves (depth toward the light only): against a path-traced reference
(`tools/cloud_ms_reference.py`, blobs lit from four directions) they lit thin cloud 4-13x too brightly and drew a
front-lit cloud as a flat disc to its rim (correlation 0.78-0.92; the new model 0.89-0.96). Prompted by a cloud-plugin
developer's notes (Martins: "90% of the cloud look is multiple scattering"; dark wispy outlines, bright cores; base it on
delta-Eddington 1D). The reference also showed the old crisp lobe shading was too strong in the multiple scattering:
real multiple scattering is smooth at lobe scale. Dropped on the way: the slab alone at physical strength (flat: a slab
along the light ignores the surface's incidence angle and holds the light in), and the octaves x a neighbourhood
build-up factor (saturated in dense cumulus).

## Now (sky light, same day)
The clouds' sky light is the mean radiance of the upper and lower hemispheres at the sample's height and Sun angle,
single plus multiple scattering (Hillaire's Psi function), baked into the Sun / sky table (cloud_v2_sunlut.comp layers
2-4) when the scattering gains change. Settings: "Sky light model (0 zenith / 1 hemispheres + multi-scatter)"
`clouds_v2.sky_light_model` 1, "Sky light on clouds (x physical)" `sky_light_gain` 1, "Ground albedo (sky light)"
`sky_ground_albedo` 0.25. It replaced the zenith radiance x "Sky ambient" with twilight terms.
