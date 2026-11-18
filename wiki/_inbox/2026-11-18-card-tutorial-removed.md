---
pages: [using/getting-started.md, using/controls.md, using/features.md, rendering/hardware-tiers.md, development/controls-and-settings.md, development/harness.md, development/architecture.md, history/design-notes.md]
code: [src/simulations/SatelliteSimHints.cpp, src/simulations/SatelliteSim.cpp (finishIntro), src/simulations/SatelliteSimUI.cpp (buildGraphicsAutoNotice), src/text/ui_text_en.inc]
---
## Now
The first run's hints (SatelliteSimHints.cpp) are the only teaching path: there is no card tutorial, no
Settings > Display "Replay Tutorial" button and no harness `tutorial` command. Replay Intro runs the hints again.
`display.tutorial_done` keeps its name in settings.json (it is the hints' "first run is over" flag, C++
`firstRunDone`), so existing settings files keep working.
When a first run's opening ends (cut short or not) on Planetarium or Potato, the top-centre graphics notice
(`GraphicsAutoNotice`) holds for 20 s, once a session: "Graphics are on <preset>, a light preset: there are no
clouds, and the terrain, sky and sea are simpler. If it runs smoothly, try Low in Settings > Display > Preset: it
brings back the clouds and every effect, at half resolution." When the opening's benchmark chose the preset it reads
"Graphics set to <preset> for this computer, a light preset: ..." instead. It also shows after a startup-chooser pick
or a crash recovery that left a light preset. Harness: `intro play first=1` ends the opening as a first run's
(no benchmark); `intro state` -> `notice`.

## History
The card tutorial (2026-10-03, SatelliteSimTutorial.cpp, 13 steps + a Graphics card on the light presets) stopped
starting on its own on 2026-11-17 when the hints arrived, and was deleted on 2026-11-18 (the user: "Delete the old
card tutorial"). Its Graphics card's explanation survives as the light-preset notice above.
