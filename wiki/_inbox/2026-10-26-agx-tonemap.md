---
pages: [rendering/sky.md]
code: [shaders/sat_sky.frag]
---
## Now
The sky pass tonemaps with AgX (Sobotka's, in Wrensch's polynomial fit): an inset gamut and a log curve, so highlights
roll off to white keeping their hue; mid grey 0.18 maps to ~0.18, so the exposure keeps its meaning. A look adds
contrast (power) and saturation. Settings (Photometry > EXPOSURE AND MOON): "Tonemap (0 exponential / 1 AgX)"
`clouds_v2.tonemap` 1, "AgX saturation" `tonemap_saturation` 1.15, "AgX contrast" `tonemap_contrast` 1.15. Not in the
Planetarium / Potato tiers or the environment probes.

## History
Replaced 1 - e^-x per channel (blended toward 1 - 1/(1 + x + x^2/2) by "Highlight roll-off", which now applies only to
the old curve): channels saturated on their own, bright orange skewed yellow and sunlit cloud went paper white.
Prompted by a cloud-plugin developer's notes ("white balance and a good tonemap cannot be overlooked").
