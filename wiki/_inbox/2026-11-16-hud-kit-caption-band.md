---
pages: [development/controls-and-settings.md, development/ui.md, development/harness.md]
code: [src/simulations/SatelliteSimUI.cpp, src/simulations/SatelliteSim.h, src/UIRenderer.cpp, tools/harness/ui_lowres.py]
---
## Now
The UI kit has four HUD pieces: `uiHudPanel(fl, childGap)` (the black rounded HUD panel: Pal::panelBg,
Style::panelCornerRadius, padding 10/10/8/8; LeftPanel, SelSatPanel and the Movement / View popups use it),
`uiKeycap` (the tutorial's keycap and pad button: amber pulsing = the input to use, green = held), `actionLabel(kb[,
gamepad])` (a KB_ action's key or pad-button label returned by value, "-" when unbound, by the last input used when
no flag is given) and `uiBottomClearance(screenH)` (how high a bottom-centred card sits to clear both corner panels).
The intro's caption container is `CineCaption`, at most min(W - 24, fs(19) x 40) wide, its text wrapping by words; the
captions are UI text entries (`NoteIntro*`). `ui dump` lists layout-only elements a check needs (CineCaption) as kind
`box`, and `ui_lowres.py` adds an `intro_caption` view per size and checks CineCaption with the HUD ids.

## History
The panel style, the keycap and the clearance were hand-copied or private to the tutorial; factored out ahead of the
hint popup and the cinematic caption band.
