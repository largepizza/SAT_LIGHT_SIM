---
pages: [rendering/clouds/weather.md]
code: [shaders/rain_particles.frag, src/simulations/SatelliteSimRain.cpp]
---
## Now
A precipitation particle's light is no longer allowed to be darker than the scene it is drawn over. The
particles are drawn after the sky TAA resolve, so the resolved background is in the frame: `rain_particles.frag`
reads it with one `texelFetch` per precipitation pixel (the sky TAA's two history colour images, bound as
bindings 4/5 of the rain descriptor set, which image is current in push-constant `obsECEFDir.w` bit 3, bit 2 =
"the background is available") and floors its contribution at `kCv2PrecipFloor` (1.3) x the background's
radiance x its alpha, held under a white ceiling (`y + bg (1 - a) <= 1`), so the composite becomes
`bg x (1 + 0.3 alpha)` wherever the particle used to subtract light. Where the particle's own light is already
the brighter one nothing changes: the rain control view (issue_20261022_174757 shot 1) is bit-identical before
and after. The floor is skipped when the sky TAA did not run this frame (`sky_taa` off, or 100 % render scale
with it off) and for diamond dust (alpha 0), so those paths keep their pre-fix look.

## History
2026-10-31 (issue 20261030_204744 "Snow in shaded conditions is black"). Root cause is the single analytic
light for the whole field: at the report's view `cv2RainKey` was 0 (the Sun's disc transmittance through
tauUp = 44) and `cv2RainAmb` was 0.046-0.060, while the cloud base the same pass draws is ~0.7 -- under that
inflated contrast the premultiplied over-blend SUBTRACTED light, so at 83 % of the precipitation pixels the
flake was below 0.8x its background (median ratio 0.74, 0.72 against bright sunlit cloud). Measured after:
0.0 % below 0.8x in all four views (report, sky, shaded terrain, rain), median ratio 1.11-1.13, clipped-white
fraction unchanged at 0.00000. Additive compositing (blend `dstAlpha` -> ONE) was the smaller alternative and
was rejected: the flakes' own light is 0.05 against a 0.2-0.7 background in this scene, so adding it is a
1-3 % lift, invisible where the over-blend's subtraction was worst. Evidence:
qa/waves/20261030b/evidence/c11/ (numbers.md, sheet_views.png, sheet_crops.png).
