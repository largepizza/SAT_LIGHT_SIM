---
pages: ["new: simulation/kessler.md"]
code: [src/simulations/SatelliteSimKessler.cpp]
---
## Now
The Sun's Fury dust clouds (each breakup of the last 3 h, drawn as a tube of Gaussians along the parent's orbit)
share a budget of 384 Gaussians a frame. Clouds are ranked by surface brightness: amp / (along-track sigma x
half-width + (nearest part's range x 0.02 rad)^2), so fresh, dense clouds win over old ones spread across the sky.
A cloud small on screen gets fewer Gaussians (pieces at least 3 half-res pixels apart), so ~40 clouds fit instead of
25. Clouds near the budget's cut fade by priority, but never the first 60% of those kept. "Dust cloud brightness"
(`kessler.dust_gain`, x1 .. x1000, default x100) scales them; at x10 a late cascade's dust is barely visible.
Harness `kessler state` -> `dust.clouds` lists each drawn cloud (event, kind, dust kg, age, rank size, amp, fade).

## History
Issue 20261020_221525 ("Dust doesn't seem to appear anymore", day 48 of the cascade): since the tube rework
(2026-10-19) clouds were ranked by amp / (nearest part's range, floored at 100 km)^2. A late cascade has dozens of
identical breakups (Starmind on Starmind) strung past the eye: all floored, their priorities tied at the budget's cut,
and the cut's fade drew every one at ~0 — no dust at any gain (x1000: no pixel changed). After the fix the issue's
view at x1000 changes 10% of pixels (99th percentile +70 levels); the day-81 view of issue 20261018_211013 shows a
field of compact young clouds.
