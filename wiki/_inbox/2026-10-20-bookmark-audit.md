---
pages: [using/features.md, development/harness.md]
code: [data/bookmarks/bookmarks.json, tools/harness/scripts/default_bookmarks.satcmd, src/simulations/SatelliteSimLaunches.cpp, src/simulations/SatelliteSimHarness.cpp]
---
## Now
**Nine default bookmarks**, audited with default settings on 2026-10-20: Great Australian Eclipse, Blood Moon from
Anchorage, Solar Storm, Beam-lit Snow over Antarctica, Rainbow over Venezuela, Starmind Formation Rings, The Sun's
Fury, Twilight Starship Launch, Starmind Deployment. Each shows what its name says.

- **Rainbow over Venezuela**: sunrise at the edge of a raining storm on the Falcon coast (11.02 N 68.58 W), looking
  at the antisolar point: a full double bow, the drops falling around the eye. Paused.
- **Beam-lit Snow over Antarctica**: McMurdo in mid-April twilight at the edge of a snow shower, the camera on the
  line of Reflect Orbital mirrors, the flakes glittering in the beams. Paused.

The weather bookmarks depend on the cloud defaults as well as on the cloud map's drift phase they store: a change
of coverage, flow, weather evolution or storm shape can move or dissolve the storm they frame. After any such
change, re-check them (`bookmark go N; wait settle; wait 30; capture`) and re-frame from
`tools/harness/scripts/default_bookmarks.satcmd`.

Harness: `launch select kind=rocket` (and `booster` / `upper`) after a jump that framed a launch (`launch next`,
`deploy`, `fairing`, `deorbit`, `landing`, `droneship`, a Missions Go) selects that launch's stage, drawn or not, so a
rocket on its pad before lift-off is selected without `nearest=1`. Otherwise it takes the brightest or nearest, and
never a booster that has already landed.

## History
- 2026-10-20: with the 2026-10-11 cloud defaults (the user's tuned coverage remap, flow warp and evolution window)
  the rainbow and the Antarctic snow showed a clear sky; their storms were re-framed (same moment, new place / phase),
  not the defaults re-tuned. Before, `launch select kind=rocket` right after `launch next` found nothing (the stack
  on the pad is not drawn), and with `nearest=1` could take another launch's landed booster.
