---
pages: [rendering/clouds/storms.md, rendering/clouds/weather.md]
code: [shaders/cloud_v2_weather.comp]
---
## Now
A cyclone's central-dense-overcast deck is forced overcast by a coverage floor in the weather bake
(`deckFloor` in `cloud_v2_weather.comp`'s `main()`). The floor is no longer the full threshold everywhere the
imagery is dark: it keeps the map's own brightness in a band under full,
`gCycDeck * (0.80 + 0.20 * clamp(cov, 0, 1)) * full / Coverage`, so the deck's coverage under the CDO carries
the imagery's structure (5-10 km) and the storm's own bands instead of erasing it. The mean stays near
overcast, so the 2026-10-27 fix that made the deck overcast under a weak CDO (issue_20261027_003307) holds.

Why: at full-threshold floor the forced-overcast deck saturated the low cloud's cell field, every column
reached the same top (a mesa) and the underside was one smooth, slowly varying surface. Flown through at
1.8 km inside a Cat 3 cyclone it read as a smooth-walled "carved cavity", with the rain hanging ~1 km below the
deck's mass (issue_20261027_213254 shot 1). Measured at that view: sky sight lines running > 4 km before
hitting opaque cloud 81,834 -> 24,602 px (-70%), > 8 km 7,410 -> 868 px (-88%); the deck's local contrast
(21x21 luminance std over the sky's upper third) 0.526 -> 0.769 (+46%); clipped-white fraction 0.00% on all
three of the report's shots. Harness `debugview cloudalpha` (raw r = the opaque-hit distance / 100 km,
g = the translucent mean distance / 100 km) is how the hollow was measured.

Open: the *cell* scale (1-2 km) is not there - the render's cell fields are 4-16 km blobs and "Cumulus base
flatness" damps the base lobes to 7.7% of their strength at the base (0.91 in the reporter's settings), so the
deck's structure is at the map's scale, not the eye's. And the rain-to-base gap at that view is unchanged:
raising the rain a third of the way into the deck closes it but fills the void with uniform rain haze
(sky local contrast 1.13 -> 0.59), so it was reverted - it needs a deck-thickness fix, not a haze.

## History
2026-10-27: the deck under a CDO was made overcast outright (issue_20261027_003307): an additive brightness
left a weak cyclone's deck broken and dry. That floor was the full threshold; 2026-10-30 (issue_20261027_213254)
it became a band under full so the deck keeps the map's structure.
