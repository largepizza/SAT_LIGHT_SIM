# UI, HUD, controls, settings window, intro, hints

HUD popups, UIRenderer / Clay rules and kit, icon + font atlases, controls and keybindings, the settings window's layout and search, the opening (the intro's captions), the first-run hints.

Part of the subsystem notes (`docs/subsystems/`, map in `AGENTS.md` §4). Moved verbatim from
`CLAUDE.md` on 2026-11-03. Do not read this file whole: `python tools/outline.py docs/subsystems/ui.md`
lists its sections and topics with line numbers. New notes go next to the section they belong to.

## Settings window: tabs, sections, search, slider slots (from "Subsystem: Clouds v2")

- **Settings layout (review 4, 2026-09-29):** the tabs are Constellations .. Photometry, then Clouds,
  **Weather** (storms, lightning, rain & snow, weather evolution, fog/dust/ice fog, god rays), **Atmosphere**
  (was "Aurora": scattering + the sky march's samples, airglow & zodiacal light, aurora), Terrain (relief;
  surface & lighting), **Night lights** (city lights, roads, sprites, city light on clouds, night sky light),
  Ocean, Beams (+ the beam light in clouds), Attributions. A tab's INDEX is persisted (`display.active_tab`)
  and indexes `hovTab[]`, so new tabs are APPENDED to `kSettingsTabNames` (Weather 12, Night lights 13,
  **Performance 14**, **Launches 15**, **Sky 16**) and the strip draws `kSettingsStrip`, grouped under headings (GENERAL: Display, Controls,
  Sound; SKY: Constellations, Sky, Launches, Photometry; RENDERING: the advanced tabs + Performance; ABOUT: Attributions — a
  negative entry is a heading) in its own vertical scroll view (an unclipped strip taller than the window
  stretched the body and spilled the content below the window). **Camera (3) has no button** since 2026-10-03:
  its two lines are the Controls tab's MOUSE section, and index 3 still opens Controls. `settingsTabIsAdvanced()`
  is the one "behind Show advanced settings" test (UI, toggle, harness). `settingsTabIndexByName("aurora")` still finds Atmosphere. Collapsible
  sections: `buildCloudSliderSections(..., base)` — each tab owns a range of `cloudSectionOpen` slots
  (Clouds 0-11, Weather 12-23, Atmosphere 24-29, Terrain 30-35; `kCloudSectionSlots` 48). Moving a slider
  between tabs changes nothing else: its slot and settings key stay.
- **2026-10-30 (issue 20261029_205641, UI cleanup):** `uiFold` (SatelliteSimUI.cpp) = a foldable `uiSection` for hand-built rows (Sky and Photometry tabs; folded on open, a search shows all). Sun's Fury is sky-event kind `cme` (the Missions card is gone; Watch = `missionsGoToFury(1)`). Sky events lost the lock-view button. Screenshot / HQ icons are `pixel--camera` / `pixel--photo` from make_icons.py (same camera; the HQ adds a star). Not done: Photometry's Milky Way / scattering rows have not moved to Sky; the Weather / Atmosphere / Night lights / Ocean / Beams reorganisation, the space-weather tab and the Great Comet meteor storm are open.
- **Categories redefined (2026-10-30, issue 20261029_205641).** The strip is Display, Controls, Sound / Constellations, Launches, Sky, Photometry / Clouds, Weather, Terrain, Performance / Attributions. **Sky** (16) = the natural sky: its folds are Stars .. Refraction, then the dark-sky gate, Milky Way, its star clouds and seeing (`buildSettingsPhotometryRows(..., 1)`: the Photometry slider table serves both tabs; fold ids `PhotoSec:<g>` kept), Scattering (+ orbit colour grade, aerial-perspective volume), Airglow & zodiacal light and Aurora (`buildSettingsAuroraTab`, folds `SkySec:6-8`). **Photometry** (5) = how satellites and points are drawn, with the Reflect-Orbital beams as a fold (`PhotoSec:30`, `buildSettingsBeamsTab`). **Terrain** (8) holds Night lights and Ocean as folds (`TerrSec:0/1`). Atmosphere (9), Ocean (7), Beams (10) and Night lights (13) keep their indices and names but have no button: `settingsTabCanonical` maps them (a saved `active_tab`, harness `tab=`) to Sky / Terrain / Photometry / Terrain. Weather: the four switches sit in their sections (`CloudSliderSection::extra`), and the former "Rebuild experiments" are filed where they act (Clouds: Quality / Lighting / Shape; Weather: Cloud shadows & god rays; Terrain: Surface & lighting; Sky: Scattering). Slot ids and settings keys are unchanged. Audit: no UI slider is left whose value nothing reads.
- **Settings round-up + search (2026-10-28, issues 20261028_222345 / _222628).** Rule: **every control that changes the
  look has a Settings row** (windows may keep convenience copies: the Weather window's Controls tab). **Sky (16)**: stars
  (`stars.*`), planets (moved from Constellations, + `planets.disc_gain`), the Moon (size / relief / shadows, moved from
  Photometry; `moon_bases.*`), comets, meteors, refraction (`refraction.enabled/mirage/turbulence/dispersion/
  sun_disc_gain`). Weather: storm objects / haboobs-as-cloud / burn scars switches, arm cirrus, surge, "Wildfires &
  haboobs" section. Terrain: solar parks; Ocean: wave motion filter; Launches: plume dispersal; Photometry: star trails,
  Milky Way zoom; Display (advanced): TAA weights. Slots 400-427 (`kCloudSliderSlots` 428). Still without UI, on purpose:
  debug / A/B / perf internals (`clouds_v2.cdc_*`, `weather_*`, `storm_index_tight`, `storm_shield_bins_ab`,
  `wildfire_debug/_alloc/_smoke_curtain/_flame_band`, `full_res_moving_mode`, `debug_view`, `sky_taa_debug/_gpu_eye/
  _parallax_floor_m`, `refraction.force_*`, `comets.particles_sync`), state (`time`, `camera`, `hud`, keybindings,
  `tutorial_done`: the hints' first-run flag) and the retired `clouds.aurora_ground_gain`. **"Show advanced settings"** is a card at the top of the
  Display tab. **Search**: a box over the tab strip (`SettingsSearch`); with >= 2 typed characters the body builds every
  tab (`buildSettingsSearchBody` -> `buildSettingsTabBody`) with `search_.on`, and each row asks `searchRow(id, label)`
  (match: every word in the label, or the section title's). The kit does it itself (`uiToggleRow`, `uiChoiceRow`, `uiKV`
  with a label — a label-less note is left out —, `uiButton` / `uiDangerButton` at the top level, `uiStatTile`,
  `buildCloudSliderRows` / `Sections`, the Photometry rows, Display's steppers); **a hand-built row must call
  `searchRow(its id, its label)` and hand-built non-row text must skip itself while `searchActive()`**, or it shows in
  every search. Kit calls nested inside a row are never filtered (`SearchRowScope`). Headings (`uiSection`, slider
  sections, constellation groups) become `searchSection()` and are drawn above their first match; the tab heading is a link.
  Advanced tabs are searched while hidden (`countOnly`, "N more in the advanced tabs" + a button). A row id seen twice in
  one search is drawn once. Harness: `ui click SettingsSearch; ui type <q>`, `ui click SettingsSearchClear`;
  `ui_lowres.py`'s `set_search` view; `harness_runs/settingsui/search.satcmd`.
- The Clouds tab's slider slots: `kCloudSliderSlots` (270 since 2026-10-10: 269 far cloud shadows; 269 since 2026-10-05: 261-266 rain particles, 267-268 storm lightning / tendrils; 258 since review 28: 257 sea wave range fade; 257 since review 27: 256 far sea ripple size; 256 since review 24: 248-250 far sea ripple, sea warp x2, 251-255 far cloud layer tunables; 248 since review 22: 244-245 cumulus lobes, 246 fast-flight LOD, 247 fovea radius; 221; 212 = ground pattern range, 213-214 sea state / whitecaps, 215-218 aurora sheets, 219 orbit grade, 220 Moon size) sizes all four per-slider arrays and
  `cloudBufs` (212 since the 2026-09-29 reviews: 200-206 city sprites, twinkle, ground share, history
  moving, ice fog x2, sprite start; 207-209 Cb head lobes, drop distance, snow wind; 210 move speed (Controls tab), 211 erosion size); v2 uses 112-199 and (pass 10-12) 2, 7, 8, 9, 16, 17, 34, 50, 61, 71, 72-76. Still free from v1's deleted
  sliders: none (55-57 went to terrain v2's sky light / night sky light / close-up textures, 58 and 77 to the city street / major road lights, Terrain tab). (Slots 189-199: Cb fill, cumulus variation, sprites, adaptive rate, adaptive
  parallax, fog x3, dust x3.) Several `GpuCloudParams` fields are now unread (v1-only: marchSteps, lightSteps, hgG,
  shadowMaxDistM, maxRenderDistM, the AO/shadow knobs, cloudsV2) — a later compaction can reclaim them.

## HUD popups: Movement and View (2026-10-29, issue 20261029_180413)

`SatelliteSimHudPopups.cpp` (`buildHudPopup`, `hudPopupsTick`), the buttons in `buildRightHudPanel` where the MSL / AGL
toggle was (`HudMoveBtn`, `HudViewBtn`; icons 41 `movement`, 42 `view`, 43 `pin` from `tools/make_icons.py`). Each opens a
small rounded panel in the selected-satellite panel's style (`Pal::panelBg`, UI-kit rows, ids `HudPopupPanel:<0|1>`,
`HudPopupTitle`, `HudPopupPin`). **Unpinned** it sits over its button and closes on any click in the sim
(`hudPopupsTick`, after the scene pick: a click on UI, the popup included, is not "in the sim"; opening one closes the other
unpinned one). **Pinned** (the small round pin button, top right) it stays at `HudPopup::x/y`, dragged by its title bar and
clamped on screen each frame. Persisted: `hud.movement_panel` / `hud.view_panel` {open (only while pinned), pinned, x, y}.
- **Movement**: Reference frame Earth / Stars (= `inertialStart` / `inertialStop`; "Sun" was considered and not offered: a
  Sun-fixed frame is the Earth-fixed one turned once a year, and a view on the Sun is Track / the sky-event Watches);
  Cinematic mode (`cinematicLatched`, settings `hud.right_panel.cinematic_mode`, Settings > Controls > MOVEMENT): the Alt
  pan-drift on without Alt / RMB (the coasting look has no drag since 2026-10-29; the mouse steers only while the look button is held) plus momentum WASD / Q/E (`cineMomentum`: input accelerates to full speed in ~0.9 s, releasing does not slow it, the opposite key brakes; 2026-10-30) and eased zoom (`cineEase`, `cineSm*`) in the movement block of recordCompute; Altitude gauge MSL / AGL (`altModeSeaLevel`, also Settings >
  Display).
- **View**: Exposure slider (-3..+3 EV, detent at 0, Reset) = `cv2ExposureEV`, the Photometry slider ("clouds_v2.exposure_ev");
  Coronagraph = `inertialOcculter`, no longer gated on the inertial lock (every `inertialActive && inertialOcculter` test
  is `inertialOcculter`; `hud.right_panel.coronagraph`, Settings > Photometry > EXPOSURE; the Sky events window's "Occult the
  Sun" is the same switch); Precipitation = `precipParticles` (`clouds_v2.precipitation`, Settings > Weather): only the
  rain / snow PARTICLES (`recordRainParticles`), the clouds' rain shafts and the rain ambience are unchanged.
- **The sidereal-mode movement bug (shot 1: "rotate around a singularity", "cannot change altitude")**: with the eye held
  in ECI (`inertialHold`) `inertialTick` rewrote `obsDir` and `obsHeightOffset` from the held eye every frame and adopted a
  player change only when `obsDir` moved: Q/E (and the altitude field / scroll) change only `obsHeightOffset`, so they were
  overwritten at once. The WASD step is a move along `obsFacing` (the view's azimuth) at constant radius: at an orbit-height
  nadir view (el -88.8 in the report) that azimuth is degenerate and swings with every degree of look, and the step is an
  orbit about the Earth's centre while the view stays on the stars. Now the tick adopts a height change too (exact compares
  against `inertialLastObsDir` / `inertialLastHeight`), and with the hold on WASD fly in the camera's own frame
  (`inertialFly`: forward = the view direction, sideways = the screen's right, the usual height-scaled speed, radius
  clamped to the ground).
- **Cinematic mode tweaks (2026-10-29, issue 20261029_213225):** the look has no drag (a coast keeps turning until the look button is held again); the wheel zoom is a pending log-FOV (`cineScrollZoom`) eased out over ~0.35 s.
- **The cinematic look (2026-10-30, issue 20261030_142354, "releasing right click or entering cinematic mode launches the
  camera at ~200 deg/s"; the look block at the top of `buildUI`).** Two causes: (1) with RMB up, latched mode turned EVERY
  cursor movement over the scene into an impulse on a velocity with no drag, so moving the mouse off the popup after
  switching the mode on spun the camera for good; (2) GLFW moves the cursor at a capture change (on release it restores
  the press position: a delta of minus the whole drag, in the NEXT frame's `dmx`), and latched mode took that as a flick.
  Now: the look rates are mouse pixels per SECOND (x `lookSens()` = deg/s; they were pixels per frame); a capture
  change (RMB, or the harness drag) or a change of `cinematicLatched` discards `dmx` / `dmy` for `kLookSkipFrames` (2)
  frames, and switching the mode zeroes the rates; latched mode with RMB up does not steer (the cursor only moves);
  with RMB held the rate follows the mouse's own rate smoothed over `kCineLookTauS` (0.12 s, capped at
  `kCineHeldMaxDegS` 360); on release it coasts at that smoothed rate capped at `kCineCoastMaxDegS` (45 deg/s), none
  under 0.5 deg/s; holding RMB with the mouse at rest brings a coast to a stop in ~0.3 s. The Alt pan-drift (RMB + Alt)
  keeps its impulse model (the same per-frame impulse at 60 fps, now a rate, capped at 90 deg/s, zeroed on release).
  Harness `inertial drag dx= dy= frames= [warp_x= warp_y=]` holds the look button through this code and injects a
  release jump; `state` -> `camera.cinematic` {latched, alt_mode, held, yaw_deg_s, pitch_deg_s, coast_deg_s,
  skip_frames}; `tools/harness/scripts/cinematic_look.satcmd` (a 1200 px/s drag: 132 deg/s held, 45 coasting, the
  -4000 / +1500 px release jump ignored, stopped by a held button at rest).
- Harness: `inertial move [fwd= right= up=]` holds movement input through the real WASD / Q-E code (until the next
  `inertial move`); `tools/harness/scripts/hud_popups.satcmd` (the report's view, Q/E and WASD in the Stars frame, then every
  popup row through `ui click`: `HudFrame:<327680|327681>`, `HudCinematic:0`, `HudGauge:<327680|327681>`,
  `HudExposureTrack`, `HudExposureReset:0`, `HudCoronagraph:0`, `HudPrecip:0`); `ui_lowres.py` has `hud_movepop` /
  `hud_viewpop` views (passes at all five sizes).

## Subsystem: UIRenderer / Clay

**UI text lives in one table (2026-10-29): `src/text/ui_text_en.inc`**, `UI_TEXT(Id, "text")` per line, read with
`tx(Txt::Id)` (`src/UiText.h`, included by SatelliteSim.h; `txc(Txt::Id)` = a Clay_String, `src/UiTextClay.h`).
**New user-facing text goes there, not into a literal** — rules and style in docs/UI_TEXT.md: ids by kind (Btn, Tip,
Lbl, Sec, Fmt, Toast...), printf formats keep their placeholders in order, plain ASCII, no history notes ("moved
to ...") in the UI, settings keys / harness text / log lines / keybinding action names (persisted) / data stay in the
code. `python tools/ui_text_check.py` checks ids, ASCII and every format's argument count at its call site (MSVC
cannot check a non-literal format: run it after rewording a Fmt string); `tools/ui_text_migrate.py` moves the
literals left at known UI call sites. English only for now (a language = a second table with the same ids).
Not in the table yet: the Attributions tab (credits), the knockout labels (dev), keybinding action names,
lowercase one-word fragments, and text built by concatenating std::string pieces (Watch toasts in
SatelliteSim.cpp / SatelliteSimLaunches.cpp, some report and bookmark messages).

- `#define CLAY_IMPLEMENTATION` only in `UIRenderer.cpp`. All other files `#include "clay.h"` without it.
- `ui.input()` → `UIInput`: per-frame mouse/scroll/button state. `scrollY` positive = scroll up. `screenW/H` = window dims.
- `ui.mouseOverUI()` → **previous frame's** capture result. Read in `buildUI` to gate scene interaction.
- `ui.addMouseCaptureRect(x, y, w, h)` — call for every visible panel in `buildUI`.
- `Clay_Hovered()` only valid **inside** a `CLAY()` element body, not in the config struct.
- **Window titles** (`buildResizableWindow`): the title bar height follows `fs(16)`, and the title
  is `CLAY_TEXT_WRAP_NONE` in a horizontally clipped GROW box. A wrapped title used to spill below
  the fixed 36 px bar into the window body at large UI scales. Button labels that must stay on one
  line need the same `wrapMode`.
- **One-frame hover lag**: store `Clay_Hovered()` in member bools; use those bools for colors the next frame.
- `CLAY_STRING(x)` requires a **string literal**. For runtime strings: `Clay_String{ false, (int32_t)strlen(buf), buf }` with a **member variable** buffer (Clay stores raw pointers read after `buildUI` returns).
- **Clip rule**: never put `.clip` on a floating container that also has `backgroundColor` — SCISSOR_START fires before RECTANGLE, hiding the background.
- **Pointer-capture rule**: any `.floating` element with no explicit `pointerCaptureMode` defaults to `CLAY_POINTER_CAPTURE_MODE_CAPTURE` — Clay's root hit-test DFS stops dead the instant it finds the pointer inside that element, so nothing below it (in z-order) gets hover/click at all. Harmless for a normal panel (you want it to swallow clicks meant for it), but fatal for any floating element that is deliberately drawn *at* the pointer's own position every frame — the pointer is then *always* "inside" it, so it permanently blackholes every panel/button underneath. This shipped once as the gamepad virtual cursor dot (`buildUI`'s `VirtualCursor`): hover/click on every real button looked totally dead while the pad cursor sat still over it (the dot's own stale hitbox from last frame permanently overlapped the current test point), but worked in brief "blips" while the cursor was moving (the one-frame-stale dot briefly lagged behind the live position, leaving a gap for the real element underneath to get tested that frame) — a pattern that reads like a coordinate or deadzone bug but isn't. Any purely-visual floating element positioned at/following the pointer (cursor dots, drag ghosts, custom tooltips) MUST set `.pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH`.
- Scrollable containers: `.clip = {.vertical = true, .childOffset = Clay_GetScrollOffset()}` on the content div, and **call `ui.scrollbar(CLAY_ID("<that container>"))` right after the container closes** — a
  thin thumb along its right edge, sized from `Clay_GetScrollContainerData`. Without it a long panel
  looks like it simply ends at the window edge (the Settings tab bodies and the satellite window's
  section list both had no indication they continue below).

- **Text fields (2026-10-03, SatelliteSimUI.cpp "Text fields"):** `textField` (a string) and `numberField` /
  `numberFieldD` (a value shown as text that becomes a text box on click — every shared slider row's value, the
  Photometry rows, the volumes, the Cinematics window). One field at a time has the keyboard (`textEdit_`): Enter /
  Tab / a click elsewhere commits, Esc cancels, Ctrl+A/C/V; the field applies a commit when it is next drawn
  (`doneId`), so no callbacks. While one is focused `onKey` sends everything to it (`textEditKey`, before the
  console), `onChar` types into it, the polled movement keys (WASD, Q/E, zoom, follow mode's) are off and Esc does
  not quit. `textEditEndFrame` runs at the end of every `buildUI` path (a scope guard): a field not drawn this frame
  loses focus. Display strings live in `textFieldBufs_` (Clay keeps the pointers until record). Typed numbers are
  clamped to the slider's range. Harness: `ui click` / `ui type` / `ui key` / `expect`
  (`tools/harness/scripts/text_fields.satcmd`).
- **Long or user-edited text is a `UITextBlock`, not a `CLAY_TEXT` (2026-10-13):** see `textArea` under *Issue
  reports*. Clay's measurement cache is 65536 words (`Clay_SetMaxMeasureTextCacheWordCount`, 4x the default, the
  arena sized by `Clay_MinMemorySize`), and the error handler prints each kind of Clay error once.
- **Window opacity and stacking (2026-10-20):** a window's panel alpha is "Window opacity" (Display tab,
  `display.window_opacity`, 0.6-1, default 1 = opaque; the themes' 245 let a window's text show through the one over
  it). Every window is a floating root at zIndex 10 and Clay sorts roots stably, so the one declared later is on top;
  a window's own floating children (the info window's view chips, the trace plot's tick labels, the marker labels)
  must use the SAME zIndex as the window (they were 11 / 12, so they drew over any later window covering them).
  So does `ui.scrollbar(id, zIndex = 10)`'s thumb (2026-10-21, issue 20261021_175415: at 20 every window's thumb drew
  over every window); a scroll container in another root passes that root's z (the note bar's `textArea(...,
  layerZ = 44)`). Tooltips (100) and root overlays (toasts, banners, the hints' popup and outline) are above windows on purpose.
- **Low resolutions (2026-10-21; the user: "the Missions window occupies the entire screen basically"):**
  - **The UI scale is the setting x the window's height.** `uiScaleUser` = "Text scale" (`display.ui_scale`, 1.5);
    `uiScale`, which every builder reads, = it x `uiAutoScaleFactor(screenH)` = clamp(H / 1000, 0.7, 1) when "Scale
    with window" (`display.ui_auto_scale`, on) — recomputed at the top of buildUI (`updateUiScale`). Nothing changes
    at 1000 px and taller (a maximized window on a 1080p desktop is 1009). **`fs()` never lets the automatic factor take text below `kAutoFontFloorPx` = 12 px**
    (or the player's own size where that is smaller; it was 16 px, the bitmap font's floor, until the SDF atlas of
    2026-10-21), so on a short window text goes to 12-13 px along with the paddings, titles and fixed-size boxes. Never write `uiScale` itself: it is overwritten every frame.
  - **No window is larger than the screen:** `buildResizableWindow` caps the maximum AND minimum size at the screen
    less `kWinScreenMargin` (8 px) every frame and re-clamps the position, so a window survives a resolution change
    (or a persisted size from a bigger desktop) on screen; its content scrolls.
  - **Default sizes go through `winDefault(px, frac, screen, minPx)`**: the 1080p size where it fits, else a share
    of the screen (Missions 600 x min(720, H - 200) docked right, at most 42% x 70%; Issue reports 60% x 72%;
    Bookmarks 55% x 66%; Cinematics 45% wide; Info 470 or a third of the width, at least 350, its minimum height
    72% of a short screen; Settings / Controls are sized on first build, not in init). New windows: size them the
    same way, never with a bare pixel size.
  - **The right HUD panel goes compact** (`hudCompact_`: no FPS or version label) when last frame's boxes show it
    reaching the left panel (1024 px wide), re-decided whenever the screen or the UI scale changes.
  - Tooltips measure their text and stay on screen on the left too (a 550-px tip started at x -190).
  - **Check it:** `python tools/harness/ui_lowres.py run` (docs/HARNESS.md "Low-resolution UI check"): every window
    and the HUD at 1024x768 / 1280x720 / 1366x768 / 1600x900 / 1920x1009, failing on a window off screen or over 62%
    of it (Settings 80%), text cut sideways or off screen, overlapping text, or overlapping HUD panels.
- **Clips nest (2026-10-03):** `UIRenderer::record` keeps a scissor stack — a clip inside a clipped scroll view is
  intersected with it and its end restores the parent's. SCISSOR_END used to reset to the full screen, so anything
  drawn after a nested clip (a text box, a horizontally clipped row) escaped its window's scroll view. `ui dump`'s
  clip boxes follow the same rule.
- **UI themes (2026-10-14, `UIPalette.h`):** every colour is a row of the `UI_PALETTE` X-macro with four columns
  (Classic [enum Default], Navy, Night red, Midnight = Classic's colours on darker, more opaque backgrounds — the
  default theme since 2026-10-15: `Pal::` starts from the Midnight column, `uiTheme` = 3); `Pal::` names are runtime globals `applyUiTheme()` rewrites, `Style::borderColor` follows
  `Pal::windowBorder`. Night red also sets `UIRenderer::setColorFilter` (pushQuad maps every colour, icons and images
  included, to red at its brightest channel). `display.ui_theme` ("classic" / "navy" / "night" / "midnight"; an old "default" reads as classic), Display tab "UI theme";
  `syncUiTheme()` (buildUI, after the harness) applies a change; `peekBootSettings` applies it before the boot chooser.
  **A new colour goes in the table, never as a `Clay_Color{...}` literal at the call site**, or it ignores the theme
  (the remaining literals are intro captions, image backings, scrims and the harness console). The swapchain is sRGB:
  a palette value of 12 shows as ~61, so dark tints get much bluer/redder on screen than the numbers suggest.
  Harness: `state` -> `render.ui_theme`; `harness_runs/ui_theme/themes.satcmd`.
- **Never `return` or `break` inside a `CLAY(...)` block**: the macro is a for loop that closes the element at its
  end; leaving it early leaves the element open and corrupts the layout. Set a flag and act after the block.
- **UI kit (2026-10-03, SatelliteSimUI.cpp "UI kit"): build windows and settings rows from it.** `uiSection` (small
  caps over a 1-px rule — the Cinematics window's form), `uiKV` (a label column and a value column; the value wraps by words since 2026-10-21 — its column must NOT clip: Clay never compresses a clip container's children, so the text could not wrap and was cut at the window edge), `uiStatTile`
  (a label over a large value in a dark inset), `uiButton` (a 20-px pill; `on`, `enabled`, `grow`), `uiToggleRow`
  (label + On/Off pill, custom words: the knockouts read Skip/On) and `uiChoiceRow` (label + segmented pills;
  returns the picked index). Ids are `CLAY_SIDI(key, index)` — scripts click them as `Key:index`
  (`ui click ReplayIntroBtn:0`) — and the previous frame's hover lives in `uiHov_`, so a row needs no hover
  member. Formatted labels go through `uiKitStr` (a per-frame ring; Clay keeps the pointers). Used by the info,
  trace and Bookmarks windows and the Display, Performance, Controls, Sound, Constellations and Photometry tabs;
  the slider rows are still `buildCloudSliderRows`.
- **HUD pieces in the kit (2026-11-16, for the hint popup and caption band):** `uiHudPanel(fl, childGap = 3)` returns
  the black rounded HUD panel's declaration (Pal::panelBg, Style::panelCornerRadius, padding {10,10,8,8}, FIT x FIT,
  top to bottom, no border, floating by `fl`; adjust the layout after). LeftPanel, SelSatPanel and HudPopupPanel use
  it; RightPanel (a fixed 38-px row) differs and keeps its own. `uiKeycap` is
  the hints' keycap / pad button (`target` amber pulsing, `down` green). `actionLabel(kb[, gamepad])` returns a
  KB_ action's key or pad label by value (`.s`, "-" unbound; by `lastInputWasGamepad` without the flag), so
  keyDisplayName's 4-slot letter pool cannot overwrite it; copy `.s` into member storage before handing it to Clay.
  `uiBottomClearance(screenH)` is how far above the bottom a bottom-centred card sits to clear LeftPanel and
  RightPanel (last frame's boxes, 12 px gap, at least 24): the caption band uses it.
- **The opening's caption band:** the caption's container is `CineCaption`, capped at min(W - 24, fs(19) x 40) wide; a line
  wraps by words inside it (the title is WRAP_NONE). It draws nothing, so `ui dump` lists it as kind `box`
  (UIRenderer's layout-only list), and `ui_lowres.py` checks it with the HUD ids (views `intro_caption`, toolbars held
  back, and `intro_caption_menu`, menu mode during the opening: the band clear of LeftPanel / RightPanel). The
  caption strings are `Txt::NoteIntro*` (keyframe `text` is a Txt; `kIntroNoCaption` = none).
- **HUD text entry (2026-10-03):** the time bar's clock and the right panel's latitude, longitude and altitude are
  `inlineTextField`s (a readout until clicked; the box starts with an edit form — signed decimal degrees, the
  altitude in the HUD's unit). `setSimTimeFromText` takes "2036-11-22 02:06[:30]", ISO with T/Z, or "HH:MM[:SS]"
  for the current date — the WHOLE text must match one form, the day must exist in its month and the year be
  1800-2200 (a day was only checked against 31, so 2036-11-31 rolled over to December 1); `setLatLonFromText` takes a sign or N/S/E/W and "lat, lon" in either field;
  `setAltitudeFromText` takes km/mi (the HUD's unit) or a m/km/ft/mi suffix, MSL or AGL by the HUD's toggle. Text
  that does not parse shows an amber toast (`screenshotToastWarn`) and changes nothing. **The altitude readout was
  wrong above the ground until then**: it showed terrain + offset as MSL and the offset as AGL, but
  `obsHeightOffset` is already above SEA LEVEL (floored at the ground), so MSL = max(ground, offset) and AGL =
  max(0, offset - ground), the ground the GPU's when the depth pass runs. Harness `expect state.<path>`
  (`tools/harness/scripts/ui_windows.satcmd`).
- **HUD layout (2026-10-03):** the bottom-left panel is the clock, the time buttons and the picture buttons
  (Screenshot, HQ photo, Star trails); the bottom-right panel ends in the MENU buttons — Missions, Bookmarks,
  Cinematics, Issue reports, Settings (`menuBtn` in `buildRightHudPanel`: lit while the window is open; the Issue
  reports flag sits immediately left of Settings since 2026-10-21). Their ids kept the `Time` prefix
  (`TimeMissionsBtn`, `TimeBookmarkBtn`, `TimeCineBtn`, `TimeReportBtn`) for scripts. Hints 16-19 outline the
  Sky events, Missions, Weather and picture buttons.

### Icon Atlas
- `ui.loadIcons(ctx, paths, count)` — loads PNGs, packs into RGBA GPU atlas, rebinds descriptor. Call once on first frame (lazy init). Store `VulkanContext*` in your sim.
- Icons: `.image = {.imageData = (void*)(intptr_t)iconIdx}`. Renderer samples the atlas UV range for that index.
- Shader `mode`: `0.0` = solid rect, `1.0` = text glyph, `2.0` = icon sprite. Binding 1 is always valid (1×1 white placeholder at init).
- **The PNGs are generated, not hand-drawn: `python tools/make_icons.py [name...]`** rewrites
  `assets/icons/ui/pixel--<name>.png` from the shapes declared in that file (all WHITE + alpha, 48×48,
  3× supersampled edges, or — the `PIXEL_STYLE` set — solid and hard-edged on a 24-px grid doubled, the look of
  the gear and the camera: new icons are FILLED silhouettes with detail punched out (`cut`), never thin outlines) and prints a 48 px preview plus the box-filtered **16 px** one — the size they
  are actually drawn at, which is the only preview worth judging. Edit a `build_*` shape list and
  re-run; do not retouch the PNGs by hand, or the next run silently drops the change. To add an icon:
  add the builder + the `ICONS` entry there, append its path in `buildUI`'s lazy-load block (the count
  is `sizeof(iconPaths)/sizeof(iconPaths[0])` — no separate number to bump), add a `kIcon*` constant —
  and re-check the preview at 16 px before wiring it in.
- **Which icons the sim actually loaded is recorded once at startup:** `buildUI`'s lazy-load block logs
  `UI: <n>/<count> icons loaded ('assets/icons/ui/...', cwd '<dir>')` into `satlight_log.txt`
  (`Log::path()`), and `loadIcons` returns the number that *loaded* (it used to return the number it
  was asked for, so a missing file was invisible apart from the magenta 1×1 it substitutes). Assets are
  CWD-relative and `main.cpp` chdirs to `Paths::exeDir()` first, so the icons always come from the
  directory the exe sits in — if a regenerated glyph "does not appear", read that line before anything
  else (and note the old process keeps its old atlas: rebuilding does not touch a running instance).

### Font atlas is a signed distance field (2026-10-21)
`loadFont()` bakes ASCII 32-126 once with `stbtt_GetCodepointSDF` at `font.bakedSize` (48 px), padded by
`kSdfPad` (8) texels, into a 1024x1024 R8 atlas (simple shelf packer; a warning is logged if a glyph does not fit):
stored value = `kSdfOnEdge` (128) + distance in texels x `kSdfDistScale` (16), inside brighter. `ui.frag` mode 1
turns it back into coverage over ONE SCREEN PIXEL: texels per screen pixel from the UV derivatives
(`dFdx/dFdy(uv x atlas size)`), `alpha = clamp(dist / texPerPx + 0.5 + bold)`; `bold` (up to 0.1 px) applies only
under 13 px. **The shader's constants must equal UIRenderer.h's `kSdfOnEdge` / `kSdfDistScale`.** The metrics are
unchanged: a throw-away `stbtt_BakeFontBitmap` at the same size still fills `charData` (`stbtt_bakedchar`), whose
`xadvance` Clay's measurement, `pushText` and `textAdvance` use, so no layout moved; each glyph's SDF box
(`FontAtlas::sdf`, padding included) is placed at its own stbtt offset, the same origin the bitmap glyph had.
Measured against the bitmap at 1080p: the same ink within ~1% (Settings text, HUD); intro titles and the "2036"
card are crisp instead of blocky; 9-12 px text is clean (the bitmap was minified without mips and broke up below
~16 px, which is why there was a 16-px floor). Night red (the colour filter) and the loading screen use the same
path. Earlier: a 32 px, then 48 px plain bitmap (`stbtt_BakeFontBitmap`), scaled per fontSize by a ratio.

### MSVC C++20 Designated Initializer Ordering
MSVC requires designators in declaration order:
- `Clay_LayoutConfig`: `sizing` → `padding` → `childGap` → `childAlignment` → `layoutDirection`
- `Clay_ElementDeclaration`: `layout` → `backgroundColor` → `cornerRadius` → `aspectRatio` → `image` → `floating` → `custom` → `clip` → `border` → `userData`
- `Clay_FloatingElementConfig`: `offset` → `zIndex` → `pointerCaptureMode` → `attachTo`

### Manual Hit-Testing
Clay does not expose element positions post-layout. Compute absolute positions from constants that exactly match Clay sizing declarations. Wrap labels in `CLAY_SIZING_FIXED` containers so layout width matches hit-test math.

---

## Subsystem: Controls / Keybinding Pipeline

**All interactive keys go through the `keybindings` vector.** The settings window and rebind UI are driven entirely from this vector — no extra wiring needed.

**Movement speed near the ground (review 6):** WASD's arc rate is capped at `moveSpeedPerHeight`
("Move speed (x height per s)", Controls tab, slot 210, `camera.move_speed_per_height`, default 1) x the
height above the ground per second, never below 3 m/s and never above the old 0.08 rad/s (~510 km/s, so
orbit is unchanged); boost and fine scale it the same way. A fixed 510 km/s crossed a cloud in a frame.
Q/E follow the same setting (review 7): `moveSpeedPerHeight` x (10 m/s + 0.5 x the height above the
ground), boost x10, fine x0.1 — it was 100 m/s + 0.5 x the height above SEA LEVEL.

### The selection's keys (2026-10-28, issue 20261028_223530)

With a satellite selected: **I** = Info window (open / close, `KB_INFO`), **G** = Go to, and G again leaves Go to
where the camera is (`KB_FOLLOW_STAY`, action "Go To / Leave Here"; was only "Leave Go To Here"), **T** = Track on /
off (`KB_TRACK`); `selectionKeyAction`. **During Go to, Track is Go to's aim lock** (`trackToggle`: T and the panel's Track button toggle `followAimLock`, the "Aim: locked / free" chip; the button is lit by it): Track's own lock would fight the ride, and until 2026-10-29 T did nothing there. T was "Select Satellite" (the centre pick), which now has no key (pad A
still; a click selects). Saved bindings migrate by `controls.key_layout` (`kKeyLayoutVersion` 2): a saved T on
"Select Satellite" is dropped once, the old action name carries its key over, and I / T keep their defaults only
where no saved binding uses them. An unbound key is -1 (`onKey` ignores GLFW_KEY_UNKNOWN; `keyDisplayName` shows
"-"). The selection buttons' tooltips name their keys ("Info (I)"), and so do the hints' keycaps. Harness
`ui key <letter>`.

### FPS camera mode (2026-11-16, `SatelliteSimFps.cpp`)

Two mouse modes. **Menu mode** is the old behaviour: a free cursor, hold the right button and drag to look.
**FPS mode** looks with the mouse and no button: the OS cursor is hidden and captured (`GLFW_CURSOR_DISABLED`,
plus `GLFW_RAW_MOUSE_MOTION` when supported, only while FPS mode holds the cursor; the right-drag keeps the OS's
accelerated motion). The look is the same code as the right-drag (buildUI's look block): `camYaw` / `camPitch`,
the FOV scaling, invert X / Y, Track's zeroing, the capture-change two-frame skip (`fpsLive_` counts as the held
button), and Alt's cinematic pan (`KB_CINEMATIC` toggles while `camera.captured || fpsLogicLive()`).

- **Toggle**: `KB_TOGGLE_CURSOR` ("Toggle Cursor") is Tab on the keyboard (FPS mode) and Start on the pad (UC5's
  virtual cursor, unchanged): `dispatchKeyAction(idx, fromPad)` tells them apart. `KB_TOGGLE_UI` is U (pad View).
  Key layout 4 migrates a saved Tab on Toggle UI to U and an unbound Toggle Cursor to Tab, each dropped again when
  another saved binding already has that key. Tab never reaches the dispatch while a text field, the console or a
  rebind has the keyboard (`onKey`'s order), so it does nothing to the mode there.
- **The one writer** is `fpsSet(on, why)`. Into FPS mode: Tab (and harness `fps on`). Out: Tab, a right press (it
  sets `camera.blockUntilRelease`, so the same press never starts a right-drag), and the forced exits: a hotkey
  that opens a window (`dispatchKeyAction` compares `fpsWindowMask()` before and after), any window, the note
  bar or an event window APPEARING by whatever means (the same mask, rising edge per frame in
  `fpsTick`: Settings, controls, info, viewer, trace, Missions, Sky events, Weather, cinematics, Bookmarks, Issue
  reports, the note bar, the event window), the console opening, a text field or rebind taking the keyboard, and the
  window losing focus. Tab back into FPS mode with one open is allowed (only the appearance counts). Unopened
  UI that needs no pointer: the crash-recovery and graphics notices and the Kessler banner (text, timed or
  passthrough), the screenshot toast, the selection panel and chip (I / G / T), the Go to chips and "Go to ...
  again" (T / G / H / [ ]), the REC badge. Nothing re-enters FPS mode when that window closes. The cursor
  is put back at the window centre on every exit.
- **Live vs set**: `fpsMode` is the setting (`camera.fps_mode`, compiled default and absent key = on), read from
  the settings file only (a patch: a cinematic shot's look, a snapshot, harness `set`, never changes the mode).
  `fpsLogicLive()` is false during a playing cinematic (the opening is live: it turns FPS mode on), and for a player during an HQ photo: menu
  mode meanwhile (cursor free, right-drag works), FPS again after. A player's own video recording (F6) keeps it
  live. Nothing captures before the scene runs: the loading screen and the boot chooser run before `init()`
  returns, App's key / cursor callbacks reach the sim only once `simInited`, `win` is first set by them, and only
  `buildUI` (`fpsTick`) and `fpsSet` capture, both needing `win`. The OS cursor is never
  captured under the harness: there FPS mode is logic only and only `mouse move` turns the view; `harnessInit`
  starts a run in menu mode and a loaded settings file never turns it on (only `fps on`).
- **UI inert**: `Simulation::uiPointerInert()` makes App hand the UI a pointer at (-1e5, -1e5) with the real
  buttons, so no hover or click reaches an element from the frozen hidden cursor and `mouseOverUI()` is false for
  the centre click and the wheel zoom. A scripted pointer (`ui click`) and the pad's virtual cursor still count as
  pointers.
- **Centre click**: an FPS left click runs `pickAtScreenCentre()`, the pick chain at the window centre in the
  mouse's coordinates (`glfwGetWindowSize`), shared with `KB_SELECT_SAT` (which used the framebuffer's
  `swapExtent`: wrong under display scaling). A miss clears the selection, as a menu click on empty sky does. The
  mode stays FPS. Each click flashes a ring at the centre (`buildFpsRing`, a `fs(12)` circle with a 1-px stroke at UI
  scale 1 and 1.5, passthrough, drawn with the harness overlays so clean shots skip it): a lone click shows it
  `kFpsRingShowS` 0.9 s, a click while it still holds adds `kFpsRingAddS` 1 s to what is left, capped at
  `kFpsRingMaxS` 5 s, then a `kFpsRingFadeS` 0.3 s fade (the owner: "briefly show a circle marker" that "should stay
  a while if you click a lot"; the first cut, 26 px for 0.6 s, was far too big and a little too brief).
- **The selection popup** (2026-11-17, the owner: the actions "often the next thing after clicking a
  satellite ... the tooltip is right there and center"): in FPS mode the screen centre is the pointer of the
  popup by the selected object (`SelSatPanel`), and of nothing else. `fpsCentreSelBtn(drawn)` tests the centre
  against last frame's boxes of the popup and its Info / Go to / Trace / Track buttons (`SelActBtn` 4-7), counting
  only what that frame drew (`fpsSelDrawn_` / `fpsSelDrawnPrev_`: Clay keeps stale boxes, and a hidden HUD draws
  none); buildUI stores the answer in `fpsCentreBtn_` before the click. The button under the centre hovers (its
  colour, rollover sound and tooltip, the tooltip placed at the centre through a copy of `UIInput`), and a left
  click presses it instead of picking; a click on the popup between its buttons does nothing (a menu-mode click on
  the panel is captured too); the ring flashes either way and the click counts in `fpsFacts_.clicks`. The pad's
  A (with a selection, ahead of the pad focus) and `KB_SELECT_SAT` press the centre's button the same way
  (`fpsPadPressPending_`, consumed by the next panel build). Off the popup the click is the pick as before. The
  popup is anchored to the object on screen (offset 18, -8 px), so turning moves the centre across it, and an
  object picked at the centre leaves the buttons a turn away, below and right of the crosshair. Info and Trace
  open a window: menu mode through the window rule; Go to and Track keep FPS mode. While Track holds, the mouse
  look is dropped, so the popup cannot be aimed at (T untracks). While the popup is drawn in FPS mode a steady
  filled dot marks the centre (`FpsDot`, `kFpsDotDiamPx` 4 scaled as the ring: 4 px at UI scale 1, 6 at 1.5,
  white with a 1-px `Pal::panelBg` rim so it reads on a hovered light button; z 101, above the button's tooltip,
  whose corner sits on the centre), with the overlays (not in clean shots). Harness: `mouse aim <ElementId>[:index]` turns the view
  onto an element through FPS mouse moves; `fps state`'s `centre_popup` / `centre_dot`, `mouse click`'s `popup`.
- **Mouse sensitivity** (Controls > MOUSE, slider slot 626, `controls.mouse_sensitivity`, 0.1-4, default 1)
  multiplies the 0.12 deg/px rate in both modes (`SkyCamera::sensMul`); the pad's look keeps its own rate (it
  scales by `fovLookScale()` only).
- `camera.captured` readers: Go to's aim unlock also counts `fpsLookedThisFrame_`
  (an actual FPS turn, not the mode), the report bar's "busy hands" sees FPS mode already left.
- `fpsFacts_` keeps sticky facts (left by Tab / right click, entered from menu mode, click count and times, degrees
  of mouse look either mode, right-drag used); harness `fps state` reports them, and the hints read `clicks` and
  `lookDeg` as running counters (their own gates come from `fpsSet`'s `hintsNoteFps`).
  Harness: `fps on|off|state`, `mouse move dx= dy= frames=`, `mouse click left|right`
  (`tools/harness/scripts/fps_mode.satcmd`).

### Gamepad layout 2 and the selection's focus (2026-10-03)

Default pad bindings: A select (centre pick) · B back · X pause · Y reverse · D-pad up/down faster/slower ·
LB/RB zoom · RS click reset zoom · LS click boost (held) · View toggle UI · Start virtual cursor; Move Fine
and Star Trails have no pad default (the sticks are analog). **`padContextButton`** runs ahead of the
bindings in `pollGamepad` and consumes a press it uses: with a satellite selected (and no cursor),
D-pad left/right move `padFocus` over its buttons (`SelActBtn` id % 4: Info, Go to, Trace pass, Track; Info
and Go to only with a geometry model, `selActionAvailMask`), A presses the focused one THROUGH the button's
own click path (`padActivatePending` -> `padActivateNow`, read by `buildSelActionButton`), B goes back one
layer (`padBack`: the 3D view / info / trace window, then Go to, then Track, then the selection). The ring and
a hint line under the panel show only while the pad was the last input. The windows themselves are still
cursor-driven. Saved bindings from layout 1 (B pause, D-pad time) keep their keys but drop their pad
buttons once (`controls.pad_layout`, `kPadLayoutVersion`). Harness `pad <button>` presses one without a
controller.

### `KeyBinding` struct
```cpp
struct KeyBinding {
    const char *action;  // display name in settings
    int  key;            // GLFW_KEY_*
    bool held;           // true = polled; false = event (pressed once)
    bool listening;      // true = waiting for rebind input
};
```

### `KB` enum (canonical indices)
The enum is in `SatelliteSim.h` (`KB_TOGGLE_UI = 0` .. `KB_ROLL_RIGHT = 33`, `KB_COUNT = 34`) and the
`keybindings` initializer in `SatelliteSim::init()` lists the same order with each default key / pad button
(`static_assert(KB_COUNT == 34)`). Read those two rather than a copy here: an 8-entry listing that stood here
went stale at KB_COUNT 8.

### Adding a new control (complete checklist)
1. Add `KB_NEWNAME` before `KB_COUNT` in the enum
2. Add one line to `keybindings` in `init()`: `{"Display Name", GLFW_KEY_X, held, false}`
3. Bump `static_assert(KB_COUNT == N)` to the new count
4. Wire the action:
   - **Event** (`held=false`): `if (pressed(KB_NEWNAME)) { ... }` in `onKey()`
   - **Held** (`held=true`): `glfwGetKey(win, keybindings[KB_NEWNAME].key) == GLFW_PRESS` in `recordCompute()`

Settings display, rebinding, hover state, and `keyDisplayName()` all work automatically. `hovRebind[KB_COUNT]` is sized by the enum so no array changes are needed.

`keyDisplayName()` handles: letters, digits, Space, Tab, Enter, Esc, Bksp, modifier keys (LShift/RShift/LCtrl/RCtrl/LAlt/RAlt), F-keys (F1–F12), arrow keys, nav cluster (PgUp/PgDn/Home/End/Ins/Del), and common punctuation.

## Camera rig (2026-11-14, camera revamp phases 1-3; `SatelliteSimCamera.cpp`)

**State.** The camera is three DOUBLE unit vectors, Earth-fixed (ECEF): `camEyeDir` (the eye's direction from the
Earth's centre; the height stays `obsHeightOffset`), `camFwd` (the look) and `camUp` (the camera's up, normal to the
look). `obsDir`, `obsFacing`, `obsLatDeg` / `obsLonDeg`, `camera.azDeg` / `elDeg` / `rollDeg` and the view
(`camera.viewEnu`, ENU -> camera, rows right / up / back: `viewMatrix()` returns it) are MIRRORS written by
`camPublish()` for their ~400 readers. **Every write goes through a setter**, each publishing at once:
`camSetEyeDir` (look and up carried by the rotation taking the old local up to the new one: heading, elevation and
roll kept; a WASD step and a teleport alike), `camSetLook` / `camSetLookDir` / `camAimAzEl` (`aimCameraAzEl` wraps it),
`camYaw` / `camPitch` / `camRoll` / `camSetRoll`. An explicit aim (`camera az= el=`, bookmarks, cinematics, the intro)
levels the horizon; a continuous re-aim (Go to, Track, inertial: `keepRoll`) keeps the roll ANGLE from level (turning
the up the shortest way with the look kept it only for small turns: Go to's jump to a satellite arrived banked 5.7
deg). `camFacing()` is the look's horizontal part, blending within ~6 deg of the nadir / zenith to the screen's top /
bottom (the same direction for a level camera), so the heading never flips. `camSync()` runs before the look block
(buildUI) and before the movement (recordCompute): a mirror written since the last publish (a write that bypassed the
setters) is adopted and logged `camera rig: adopted an outside write to ...` (state `camera_rig.adopted_writes`: 0 in
every phase 2 / 3 run). `SkyCamera::updateCapture` only captures / releases RMB; the turn is the rig's. Inertial
hold's fly keeps the look fixed in ECEF (the view stays on the stars). Sound pan (thunder, impacts, launches) is the
source's direction against the camera's right, which rolls with it.

**Horizon auto-level** (`camLevelActive()`: Settings > Controls "Auto-level the horizon" `camera.auto_level`, default
on, and the eye below "Auto-level up to (km)" `camera.auto_level_km`, default 1000 (100 for its first hours: the user
set 1000, above the satellite shells), slot 625; Go to: the camera's
height). On: the mouse / stick yaw about the local up (looking straight down the ground turns under you), pitch about
the camera's right, stopping at +-89 deg, and `camLevelTick` eases any roll to level (tau 0.3 s, faded within ~10 deg
of the vertical where roll is the heading: 60 deg -> 51 in 3 frames -> 4.9 after 1 s). Off (above, or switched off):
yaw / pitch about the camera's own axes, no pitch limit (pitching past the nadir comes out upside down, roll 180), and
Z / C (`KB_ROLL_LEFT` / `RIGHT`, held, 45 deg/s, boost x2, fine x0.25; key layout 3 moved Reset Elevation Z -> X and
dropped Toggle Cursor's C) roll. No toast at the crossing (one was tried; the user turned it down). Blending the yaw to the
camera's up near the nadir under auto-level was tried and dropped: a sideways drag at -89 slid the view to -79 and
swung the heading 144 deg.

**Q / E follow the same rule** (issue_20261115_133412, the user's call 2026-11-15). Auto-levelling: the pure altitude
move (`obsHeightOffset` only; the eye's lat / lon do not change). Free: the eye moves along `camUp` (in double, the
ground a floor, `camSetEyeDir` carrying the look; under inertial hold the look is re-held as `inertialFly` does), so
from orbit looking down Q slides the view toward the screen's top. `camVertFreeWeight()` is the weight of `camUp`
against the local up: 0 below `camLevelAltKm`, smoothstep to 1 at `kCamVertBlend` (1.25) x it, 1 with auto-level
off; the move is along the normalised blend, so the direction turns continuously across the limit instead of jumping
by up to the look's pitch (90 deg looking straight down). The rate is unchanged. Follow mode's Q / E (the satellite's
frame, `updateFollow`) are untouched.

**Measured.** No snap: `viewMatrix()` was lookAt(az / el, up = Up, North past |el| 88): -87.9 -> -88.1 deg from 420 km
turned the image by the azimuth (mean change 40 levels); now 8.3 (cloud detail; 2.7 at 10 km). A level camera's frames
are as before (phase 3 vs 2 A/B: PSNR 64-70 dB, mean |difference| 0.006-0.025 levels, every camera state the same).
Roll renders in every pass (`harness_runs/cam_phase1`: a rolled frame equals the roll-0 frame turned about the screen
centre to 0.1-0.4 levels, light within ~1%; the sky AP limb grid sizes itself from a cone round the frustum). Open:
orbit night at roll 90 carries +4% light in its point sources (satellite shells), the same points in the same places.

**Why double.** `obsDir` used to be canonical and float: a unit vector's ulp is ~6e-8 (~0.4 m on the surface), and
WASD added `speed * dt * dir` a frame. Holding W 3 s over the sea moved 0.2 m of 9 at 2 m height (3 m/s), 34 m on a
bearing of 2.7 deg at 10 m whether facing 0 or 45 deg (the rounding chose it), 75% of the distance at 30 m. In double:
9.1 / 30.5 / 91 / 303 m (the wait's wall-clock slack) on bearings 0.00 / 45.00 deg, forward + 0.5 right at 71.57 deg
(exact). **The GPU still gets the float mirror** (`obsECEFDir`): the rendered eye moves in ~0.4 m steps, a
camera-relative (hi / lo) eye in the shaders would remove that.

**Fly where you look** (phase 4). W / S move along `camFwd` and A / D along the camera's right, in 3D, at any angle
and roll; Q / E are the pure up / down while auto-levelling (along the camera's up when free, above). The speed is the old arc rate on the Earth's radius, still scaled by the height
above the ground (a dive slows toward the ground, a climb speeds up), and the ground is a floor: a step into it keeps
its part along the ground (W looking down slides). After each step the look is carried with the ground (`camSetEyeDir`),
so W at el 0 holds the height (follows the Earth's curve). Measured (`harness_runs/cam_p4`, 2 s of W / D): looking up 30
from 2 m climbs at 31 deg; down 30 from 1 km descends at -30.0; down 30 on the ground drops to the floor and slides;
level at 45 deg holds 1000.0 m on bearing 45.00; up 89 climbs at 89; D looking up 60 strafes level; D rolled 90 at
420 km goes straight down. Until then WASD walked the heading at a constant height (a lat / lon move): pointing down
and pressing W never brought anyone down. Inertial hold's fly (`inertialFly`) was already this; its A / D now use the
camera's right too (rolled 90 at 420 km, D goes straight down with the view held on the stars). Stars frame verified
(`harness_runs/cam_stars`: held and actual view / eye equal to 0.0000 deg through time, W, drag and roll; `inertial
state` -> `actual_view_off_deg`, `actual_eye_off_deg`, `roll_deg`).

**Still open.** Bookmarks and
cinematic keys store az / el only, so they restore level. Settings load restores the elevation and position but faces
north: `camera.az_deg` is saved and never applied (as before the rig). The pad has no roll binding by default.

---

## Subsystem: The opening (UC3)

The opening is the intro's captions played over the player's own first minute in the scene. `showIntro` = the
captions are playing. `updateIntro()` (`SatelliteSim.cpp`, called from `recordCompute()` ahead of the movement
block) sets the scene on its first frame (`!introBasisValid`) and from then on runs only the caption clock, the
UC1 benchmark and the end; nothing in it touches the camera, the observer or the clock after that first frame.
`buildIntroOverlay()` (`SatelliteSimUI.cpp`) draws the caption. Whether it plays on launch is the persisted
`playIntroOnStartup` (Display tab "Play intro on startup"; see the persistence note below).

**Window boots maximized** (`App::initWindow`, `glfwWindowHint(GLFW_MAXIMIZED, ...)` before
`glfwCreateWindow`) — windowed, not exclusive fullscreen, but full monitor work-area size by
default rather than the fixed `WIN_W`x`WIN_H` (1280x720). `WIN_W`/`WIN_H` are still passed as the restore size
for whenever the player un-maximizes later.

**The first frame.** The moment `kIntroSimStartJ2000S` (2036-11-25 23:15:46.57 UTC), the cloud map's phase
`kIntroCloudPhase` (2.356 rad: the offset only, the player's drift rate is kept), any playing cinematic stopped,
follow, Track and the inertial lock off, the clock at plain 1x running forward. The place and the look are the
user's cinematic `intro2`'s first key (`kIntroStart*`, `SatelliteSim.h`): the Mississippi delta (Arkansas) at
33.564 N, 91.289 W, 54.56 m above sea level (floored at the ground), az 3.19 / el 18.25 / FOV 46.5, facing north up
Starmind's train at twilight, the Reflect mirrors' busiest beam site ~55 km east, a Starship climbing out of
Starbase, Texas to the south. Applied once with `harnessApplyCam` (as the Cinematics playback applies a key: it
sets `camera.azDeg` AND `obsFacing`, which `buildUI` derives the azimuth from). Then FPS mode on
(`fpsSet(true, FpsWhy::Intro)`) with `fpsFacts_` reset, so the mouse looks from the first frame
(`fpsLogicLive()` is true during the opening: the OS cursor is captured for a player once the window has focus;
logic only under the harness). A replay from the Display tab returns to the same moment and place, wherever the
player has since gone (`startIntroReplay` clears `introBasisValid`).

**Why that date.** intro2 was framed on 2036-11-21 23:19:46 UTC, under a 16%-lit crescent. The user wanted a
quarter-to-half Moon, for some ambient light at night (where the clouds look best). `launch list` over Nov 23-27
found the same scene four days later: a Starship from Starbase, Texas lifting off eastward at 23:10:16 (the
cinematic's at 23:14:16) with the Sun ~4.5 deg up at the pad; the opening starts 330.6 s after lift-off as the
cinematic did, the observer's Sun at -4.1 deg, the Moon 50% lit, ~42 deg up in the south. Launches, beams and
satellites are pure functions of sim time, so the moment is identical on every machine. The default cloud phase
put a bank over the train and the beam at Nov 25; a sweep of 9 phases picked 2.356 (clear over the vantage, the
beam showing). `kInitWholeSec` (the boot time, 2036-11-22 02:06:22) is unchanged: harness runs start there.

**The player has control throughout.** Movement (WASD along the look in 3D, Q / E, boost, all the way to orbit),
look, zoom, picking (the FPS centre click and its ring, or a menu-mode click), the clock's keys (Space is Pause)
and the pad (Start keeps its action) all work as in normal play from the first frame: the movement block in
`recordCompute`, the look block in `buildUI`, `onKey`, `pollGamepad` and `padContextButton` have no opening gate.
There is no skip: the captions run their 53.27 s (Settings > Display > "Play intro on startup" is the off switch).

**Captions** (`kIntroKeyframes[]`: a time and a `Txt` per key, `kIntroNoCaption` = none; timings are multiples
of `songbeat` = 7.61 s, a music-tempo unit hand-tuned by the user): `kIntroYearIndex` (0) is the "2036" card, a
larger font, bottom-anchored like the rest; beats 1-5 the narrative lines; `kIntroTitleIndex` (6, 45.66 s) "SAT
LIGHT SIM", one line (`CLAY_TEXT_WRAP_NONE`: it wrapped to "SAT LIGHT / SIM"); the last key (7, 53.27 s, no
caption) is the end. A caption fades in over 0.8 s from its key; everything fades out over the last second. The
caption clock `introElapsed` advances by the frame's dt (it stays with the music, which `startIntroReplay` cuts to
track 0; ambience: the `intro` driver, the upwell stem muted and the bus at 0.5 while `showIntro`,
`SatelliteSimAmbience.cpp`). The band (`CineCaption`, inside the full-screen `IntroCaptionOuter`) never takes
input: no capture rect, `CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH`. Its bottom offset is 90 px, or
`max(90, uiBottomClearance(H))` when the corner toolbars draw that frame, so it sits clear of them. The overlay is
drawn with the harness overlays, before the `uiVisible` check (it shows with the HUD hidden).

**The corner toolbars** (`LeftPanel`, `RightPanel` and their Movement / View popups) are held back while the
captions play until the player is first in menu mode (Tab, a right click, or any forced exit: `!fpsMode` after
`fpsTick`, checked in `buildUI` once the scene is set), or the captions end; then `introToolbarsShown` sticks.
Everything else (the selection panel and chip, the follow bar, windows opened by hotkey, toasts, notices, the
console, issue shots and the note bar, the ring) behaves as in normal play.

**The end** (`finishIntro(false)` at the last key): `showIntro` off, the toolbars show, the ambience bus returns;
no camera or FOV change. Nothing else starts there: the first run's hints (below, "Hints") run on through the
end, and on a light preset the graphics notice explains it ("The light-preset notice", below). `finishIntro(true)` (only the harness's `intro skip`) ends
the captions without the benchmark.

**UC1 benchmark.** `updateIntro` adds `gpuMsTotalSmoothed` every frame while `introElapsed <= kIntroBenchEndT`
(the whole caption period: the player on the ground, steering). `finishIntro` decides with
`introBenchDecision(mean, preset)`: over 33 ms (under ~30 FPS) steps down (Medium -> Low -> Planetarium -> Potato),
under 4.8 ms steps up (Potato -> Planetarium, Low -> Medium -> High); High, Ultra and Custom are never moved; the
"graphics set to X" notice shows either way (on Planetarium / Potato the light-preset notice, below, in its place). Not on a cut-short end, a replay, a crash-recovery launch or after a
boot-chooser pick. The harness's `intro play bench=1` measures and decides as a first run; `intro state` ->
`bench` {frames, mean_ms, would_set, last}.

**Other readers of `showIntro`:** the ambience (above), the toolbar hold and the overlay (`buildUI`), the
hints (the beat-2 hold, "captions ended"), `loadSettings` (a full load arms it from `playIntroOnStartup`; a settings patch never starts it
mid-session), the harness (`harnessInit` and `snapshot` keep it off). The Display tab's "Replay Intro" closes
Settings and starts it (allowed during the opening too).

**`timeScaleIdx`'s compiled-in default was `1` ("10x"), not `0` ("1x")** — a real bug, fixed 2026-10:
`loadSettings()` overrides it from `settings.json`'s `time.scale_idx` when present, so a genuine first run
booted at 10x. The opening's first frame also sets 1x, since a replay runs on live state.

**Persistence: `playIntroOnStartup`, not a raw `showIntro` save.** `showIntro` is runtime play-state, so
persisting it conflated "did today's playthrough finish" with "should it play again next launch."
`playIntroOnStartup` (Display tab checkbox, next to "Replay Intro") is the preference, saved every close and
applied to `showIntro` at a full load: `playIntroOnStartup = d.value("play_intro_on_startup", false)` inside the
`"display"` block. Absent-key default is `false` there (not the compiled-in `true`) so a player upgrading from a
build that predates the key does not suddenly get it — a genuine first run (no settings.json at all) never
reaches that line and keeps the compiled-in `true`. Turning it off resumes wherever the player last was:
`obsDir` / `camera` are restored from settings.json's `"observer"` / `"camera"` blocks regardless.

**History.** Until 2026-11-16 the intro was a camera flight along intro2's eight keys to orbit (the HUD and all
input locked, sim time driven at 5 s per beat, skippable with Space / pad Start); its times and captions remain.

---

## Subsystem: Hints (2026-11-17, `SatelliteSimHints.cpp`)

The first run's teaching: **one small hint at a time** (`HintPopup`: keycaps, then one line), chosen from what the
player has and has not done: the only teaching path (the card tutorial it replaced was deleted 2026-11-18).
Hints 1-11 are the first run's **pass**; 12-19 are **contextual** (round 2, 2026-11-17): they outlive the pass and
fire when their moment comes, in any later session, until used.
`hintsTick` (buildUI, after the look block, before the click pick) measures and decides; `hint_` (`HintRun`) holds
every bit of state; `buildHints` (late in buildUI, after every HUD element it avoids) draws.

**Gates** (`HintGates`, sticky since the pass began): `looked` (30 deg of view turned in total: `fpsFacts_.lookDeg`'s
growth in either mode + the pad's stick x `fovLookScale`), `moved` (1 s of movement input in total), `climbed`
(100 m of height changed by elevation input: `hintClimbM_`, summed in the Q / E block), `boosted` (boost held while
moving or climbing), `zoomed` (the wheel's zoom in buildUI or the zoom keys), `clicked` (an FPS centre click),
`leftByTab` / `leftByRightClick` / `returned` (`fpsSet` -> `hintsNoteFps`; `returned` = any entry but the opening's
and the harness's own; a pad player: the virtual cursor's on / off edges), `dragLooked`, `selected` (a CHANGE to a
satellite by any path: the FPS click, a menu-mode click, the pad's A at the centre or at its virtual cursor, a window
or list, the harness's `select`; a selection standing when the pass began does not count), `timeBarShown` (a time control used, or
hint 9's 8 s), `menuBarShown` (Missions / Weather / Sky events / Settings opened, or hint 10's 8 s); `dragLooked`
(a right-drag turn in menu mode: `camera.captured` while not in FPS mode) is hint 11's. Movement is
read from the movement block's MERGED input (`hintMoveIn_`, `hintBoostIn_`: W A S D + the left stick + harness
`inertial move`), one recordCompute behind.
**Live signals:** the mode (`hintMenuMode()`: `!fpsMode`, or for a pad player `vCursorToggled && uiVisible`),
moving now and the streak without a break, seconds since any input (keys, pad buttons, clicks, wheel, look,
movement, the mode, the time bar, the windows), menu-mode idle, the view held still (< 3 deg/s), FPS clicks in the
last 5 s (a ring of 8 times), the caption beat and the captions' end (`!showIntro`: no opening counts as ended),
whether a window is open.

| # | Text (`ui_text_en.inc` `Hint*`) | Offered when (gate unmet) | Holds while up |
|---|---|---|---|
| 1 | Move the mouse / Hold the right mouse button and drag / Move the right stick ... to look around | always | always |
| 2 | [W][A][S][D] to move / Left stick to move | `looked` (or case c) | always |
| 4 | Hold [Shift] ([LS]) to go faster | `moved` | always |
| 3 | [Q] / [E] (pad [RT] / [LT]) to rise and descend | `moved`, and `boosted` or 4 offered once | moving now or `moved` |
| 5 | Scroll to zoom / [LB] / [RB] to zoom | view still 2 s, and a selection or a drawn mesh satellite within max(24 px, its size) of the centre | that satellite |
| 6 | [Tab] or right click for the cursor / [Start] for the cursor | FPS mode, never in menu mode, and 6 s idle, or 2 FPS clicks in 5 s, or the captions ended | FPS mode, never in menu mode |
| 11 | Hold the right mouse button and drag to look around (`HintLookDrag`) | menu mode, no selection, not a pad player (the stick looks in either mode) | menu mode, no selection |
| 7 | Click a satellite | menu mode | menu mode |
| 9 | Time: slow down, pause, speed up | menu mode, `selected` | the same |
| 10 | Missions, Weather, Sky events and Settings | menu mode, 9 done | menu mode |
| 8 | [Tab] / [Start] to look around again | menu mode, 9 and 10 done (or 10 s idle in menu mode) | menu mode |

Keycap labels come from `actionLabel` (live bindings; W A S D and the pad's RT / LT are literal). 4 before 3
(round 2, the user: near a near-black ground "it genuinely seems like you are going nowhere until the Boost hint
shows up"): boost is offered as soon as `moved` is met, `kHintHandoffS` (1 s) after hint 2 leaves instead of the
3-s gap, and rise / descend once boost was used or its hint had its turn (shown once, whatever its exit).

**Contextual hints** (12-19; gate = the fact, kept in settings): offered only when no pass hint wants the slot, after
the captions, with no window open (`fpsWindowMask()`) and no text entry; a pass hint that comes to want the slot
takes it (`how` = yield: the contextual one gets its offer back). Same popup, timings and `kHintMaxOffers`, but the
offer counts persist, so a returning player is offered each at most twice ever.

| # | Text | Offered when | Holds | Gate (`display.hint_gates` name) | Outlines |
|---|---|---|---|---|---|
| 12 | [Z] / [C] to roll | horizon free (`!camLevelActive()`: above `camLevelAltKm`, or auto-level off) | the same | Z / C rolled the view (`rolled`) | - |
| 13 | [G] to go to it | a selection with a mesh, not following, no cinematic | selection, not following | `followActive` outside a cinematic (`gone_to`) | - |
| 14 | Hide the Sun's glare: View panel > Coronagraph | the Sun (above the Earth's limb, the glare's own test) within `kHintSunDeg` 10 deg of the view centre for `kHintSunS` 4 s in total this session (reset by its timeout) | always | `inertialOcculter` (`coronagraph`) | HudViewBtn |
| 15 | Lock the view to the stars: Movement panel > Reference frame > Stars | eye above `kHintGeoKm` 35,786 km, not locked | above it | `inertialActive` (`star_lock`) | HudMoveBtn |
| 16 | Sky events: eclipses, meteor showers, comets and aurora | a song gap | always | Sky events opened (`sky_events`) | TimeSkyBtn |
| 17 | Missions: rocket launches to watch, live and coming up | a song gap | always | Missions opened (`missions`) | TimeMissionsBtn |
| 18 | Weather: storms, cyclones and wildfires around the world | a song gap, weather available | always | Weather opened (`weather`) | TimeWeatherBtn |
| 19 | [F12] for a screenshot, [F8] for a high-resolution photo | a song gap | always | a screenshot or HQ photo taken (`requestScreenshot` / `requestPhoto`; `screenshot`) | TimeScreenshotBtn + TimePhotoBtn |

Order when free: 12, 15, 14, 13, then in a gap the not-done one of 16-19 offered least (ties in that order: Sky
events, Missions, Weather, the picture keys), **one per gap** (`gapUsed`). A song gap = the music audible (playing,
not paused, master x music volume x the altitude fade > 0.03) and between two tracks (`!trackPlaying()`,
`gapRemaining() > 0`: `musicGapS`, 30 s); with no audible music (off, paused, muted, high orbit) a "gap" of
`kHintGapWindowS` 30 s opens every `kHintNoMusicS` 240 s of silence. Their keys use the pad's binding when one is
bound, else the keyboard's (roll, Go to and the picture keys have no pad default). The gates are measured every frame
(`hintsCtxGates`, hints on or off) and saved at once (`hintSaveDirty_` -> `saveSettings`).

**The scheduler** (constants `kHint*` in SatelliteSim.h, one block): nothing before beat 2 begins (15.22 s of
captions; `kHintBeat`); one hint at a time, 3 s from one gone to the next (`kHintGapS`), 0.3 s fades; a hint fades
2 s after its gate (`kHintLingerS`; gone 2.30-2.35 s after at 20 steps / s), and at once when its "holds" column
fails (`how` = condition); a gate met before its hint ever showed retires it; ignored 20 s -> fades and rests 30 s,
two offers at most (9 and 10 instead leave after 8 s up, counted as done). The order when the slot is free:
a. captions ended: 6 if never in menu mode, else 7 in menu mode without a selection (both never time out);
b. menu mode with a selection: 9, then 10; c. a selection and 9 + 10 done: 8 in menu mode, then 2 if not moved
(never times out); d. 1; e. `moved`: 4, then 3 (not climbed); f. 6 on idle / two clicks; g. 7 in menu mode;
h. 2, 3, 5, then 8 on menu idle; in a and g, 11 comes before 7 (the first time in menu mode); then the contextual
hints. **Two FPS clicks within 5 s take the slot for 6 at once** (`preempted`: the hint
up gives way, gets its offer back, no 3-s gap) unless the hint up is in a never-time-out case: without it the
clicker would see 2 (on at beat 2 for anyone who looked) and wait. **Complete** = captions ended + `selected` +
`moved` + 9 and 10 done (retired by use counts): `firstRunDone` (`display.tutorial_done`) set and `saveSettings()`; then 3, 4, 5 and 8 may
fire once more that session (`postFired`).

**When:** `hintsPass_` is decided on the first frame (`!firstRunDone` = a pass), and every opening's first frame
(`updateIntro`, a first run's or a Replay Intro's) starts a pass from zero (`hintsReset`, which also primes the
edges); a Replay Intro (`introIsReplay`) also forgets the contextual gates and counts (`hintsReset(true)`). Without a
pass (a returning player, `hintsPass_` 0) the scheduler still runs, for the contextual hints only: one whose gates
are all met or spent sees nothing. `display.hints` (Settings > Display > Startup "Show hints", default on) turns
every hint off; Replay Intro keeps that choice. Never shown during a playing cinematic, an HQ photo, or with the HUD hidden (`how` =
suppressed); a harness run keeps them off (`harnessInit`) until `hints on`.

**The popup** (z 27: over windows 10 and notices 25, under the caption 30; passthrough; width <= W - 24; fs(13);
keycaps fs(13) x 1.6 tall). While a caption is declared (`introCaptionBuilt_`) it is attached to `CineCaption`'s
top edge, 8 px up (Clay places it the same frame, so it rises when the caption wraps); between two captions (the
next one's first frame draws none) it stays where it was. Else bottom centre, above both toolbars (12 px) and
anything else in its way. 9 / 10 go above `LeftPanel` / `RightPanel` (left / right aligned, clear of a 4-px
outline, `HintOutline`), 14-19 above their button's toolbar, centred on the button (the outline round the button),
unless that would cover the caption or another element: then the bottom stack, with the outline still marking the
toolbar or button. Avoided (only if declared this frame: `UIRenderer::declaredThisFrame`, Clay
keeps stale boxes): LeftPanel, RightPanel, HudPopupPanel 0 / 1, SelSatChip, FollowHud, FollowAgainBtn, CineHud,
FpsRing, CineCaption. Its own size is last frame's box (an estimate on a new hint's first frame, at alpha 0).
`ui_lowres.py` has sixteen hint views (`hint_caption_menu`: the longest pass hint, 1 in menu mode, over beat 2;
`hint_caption`, `hint_9`, `hint_9_caption`, `hint_10`, `hint_pad`, `hint_11` ... `hint_19`, `hint_14_pop` with the
View popup open) and `HintPopup` in `HUD_IDS`; `HintOutline` must be on screen and never under the popup.

**Harness:** `hints on|off|reset|state|set <gate> [0|1]|force <n>|gap [s=] [pad=0|1]` (`on` keeps the saved
contextual state, `reset` clears it; `set` takes the contextual gate names too; `gap` opens a song gap now), `state`
-> `hints` (gates, live incl. `sun_deg`, `sun_s`, `level_active`, `above_geo`, `music_audible`, `song_gap`,
`gap_open`, `gap_used`, `silent_s`; `saved` {gates, offers, all_done}; showing, offers, retired, texts, `seen`
"1,6,7", `log` with shown / gate-met / gone times and `how`), `mouse scroll dy=`, `audio music end` (a real gap).
**A pad player's menu mode is the virtual cursor with the FPS mode left on**, so buildUI's pick takes the centre
(`fpsCentreClick`) only while no virtual cursor is up (`fpsLive_ && !vCursorActive`); with it up, A (App's lmb) picks
at the cursor as a mouse click does. Until 2026-11-18 the FPS branch swallowed that press: A on a satellite under the
cursor selected nothing, and hint 7 could only be met by A without the cursor (the centre pick).
`tools/harness/scripts/hints_players.satcmd` (`--fixed-dt 0.05`, one launch): Still 1,6,11,7,9,10,8,2 then complete
and 4; Explorer 4,3,6,11,7,9,10,8; Clicker 2,6 (6 on the second click); Mover 2,4,3 (4 one second after 2 leaves) and
2,4,3 with 4 timed out; each hint 6,11,7,9,10,8,2,4,3,5; Veteran 9,10,8 (1, 2, 6, 7 retired); Returning with every
gate met: none; Contextual 13,13,12,15,14,16,17,18,19,17 then none (all done); Selector (`selected` met by a real
menu-mode click during and after the captions, by the pad's A at its virtual cursor, by `select`); Gamepad labels.

---

## Subsystem: The light-preset notice (2026-11-18, `finishIntro`)

The hints are the only teaching path. The card tutorial (2026-10-03 to 2026-11-18, `SatelliteSimTutorial.cpp`)
is deleted; its one card that was not about the controls, the light presets' "Graphics" card, lives on as a notice.
When a **first run's opening ends** (`finishIntro`, cut short or not) on **Planetarium or Potato**, the
`GraphicsAutoNotice` band (top centre, 16 px down or 8 px below whatever already sits at the top across its width:
`SelSatChip`, `FollowHud`, `FollowAgainBtn`, `CineHud`; z 25, `Pal::toastInfo`) says what the preset leaves out and
where to change it, for `kLightPresetNoticeS` (20 s), once a session (`lightPresetNoticed_`):
- after the benchmark decided (`introBenchDecision`, which also writes the band): `FmtGraphicsSetToALightPreset`
  ("Graphics set to %s for this computer, a light preset: there are no clouds, and the terrain, sky and sea are
  simpler. If it runs smoothly, try Low in Settings > Display > Preset: it brings back the clouds and every effect,
  at half resolution."), replacing the benchmark's one-line "Graphics set to %s" text;
- otherwise (the startup chooser's pick, crash recovery's forced Planetarium, the hardware default with the opening
  cut short): `FmtGraphicsAreOnTheFastestPreset` ("Graphics are on %s, a light preset: ...", the card's own text).
A first run = not a Replay Intro and `display.tutorial_done` false (`firstRunDone`: the key keeps the card
tutorial's name so existing settings files keep working); the harness's `intro play first=1` (no benchmark) and
`bench=1` count as one and re-arm the once-a-session flag. The band is capped at min(W - 32, fs(13) x 46) wide and
wraps; it sits at the top, clear of the caption band and the hint popup (bottom). `intro state` -> `notice`
{text, remaining_s, light_preset_noticed}. `ui_lowres.py` view `light_notice` (the ISS selected and out of view, Planetarium, `intro play first=1
at=52.7`, hint 7 forced; `intro notice off` after): `GraphicsAutoNotice` is in `HUD_IDS`.
