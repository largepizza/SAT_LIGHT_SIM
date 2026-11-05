---
pages: [rendering/atmosphere-and-sky.md, using/features.md]
code: [shaders/sat_sky.frag, shaders/planet_disc.frag, shaders/cloud_v2_lightning.comp, src/simulations/SatelliteSim.cpp, src/simulations/SatelliteSimCloudsV2.cpp]
---
## Now
The sky pass's AgX tonemap has "AgX range" (Settings > Photometry > Exposure, `clouds_v2.tonemap_range`, 1.4): the
scene's log contrast about mid grey (0.18) is multiplied by it before the curve; below mid grey the extra contrast
saturates 1.5 EV down (1.5 tanh(d / 1.5)), so dark ground keeps its detail. The planet discs take the same range.
AgX saturation defaults to 1.35, contrast stays 1.15. Daytime auto exposure meters the displayed frame toward
"Auto exposure target" (`clouds_v2.auto_exposure_target`, 0.40 linear mean; the orbit spot meter keeps 0.47).
"White balance" divides by the Sun's colour as the clouds receive it: "Cloud sunlight Rayleigh" plus the ozone layer
(`clouds_v2.white_balance_cloud_sun` 1; 0 = the sky's Rayleigh and no ozone).

## History
2026-11-05 (light3, user: "shading is still a bit too grey ... anything in direct sunlight should be significantly
lit"): the grey was mostly the auto exposure (it stopped sky-and-cloud frames down 1-2.4 EV toward a 0.32 mean, so lit
cloud sat ~1 EV over mid grey in AgX's flat middle) and AgX's 16.5-EV span. Darwin cumulus, Sun 5 deg, from the
ground: lit display luminance 0.67 -> 0.84, lit/shaded 4.0 -> 5.2-5.6 (photos 5-8). AgX "punchy" (contrast 1.35,
saturation 1.4) was tried: more contrast but lit faces darker (0.71 -> 0.61). The white balance had used the sky's
Rayleigh (0.98) against the clouds' 0.65 and no ozone: 18-km view of the Congo anvils at Sun 10 deg, lit blue/red
1.21 -> 1.12. Evidence: qa/waves/20261105/evidence/light3/.
