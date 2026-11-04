# UI, HUD, controls, settings window, intro, tutorial

HUD popups, UIRenderer / Clay rules and kit, icon + font atlases, controls and keybindings, the settings window's layout and search, the intro cinematic, the first-run tutorial.

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
  `tutorial_done`) and the retired `clouds.aurora_ground_gain`. **"Show advanced settings"** is a card at the top of the
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
  layerZ = 44)`). Tooltips (100) and root overlays (toasts, banners, tutorial outlines) are above windows on purpose.
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
  (`TimeMissionsBtn`, `TimeBookmarkBtn`, `TimeCineBtn`, `TimeReportBtn`) for scripts. The tutorial's Missions steps
  outline the rocket; its "Menus" step outlines Bookmarks .. Settings.

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
"-"). The selection buttons' tooltips name their keys ("Info (I)"), and so do the tutorial's cards. Harness
`ui key <letter>`.

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
```cpp
enum KB {
    KB_TOGGLE_UI  = 0,   // Tab    — event
    KB_PAUSE      = 1,   // Space  — event
    KB_SLOWER     = 2,   // ,      — event
    KB_FASTER     = 3,   // .      — event
    KB_REVERSE    = 4,   // R      — event
    KB_MOVE_BOOST = 5,   // LShift — held
    KB_MOVE_FINE  = 6,   // LCtrl  — held
    KB_CINEMATIC  = 7,   // LAlt   — event (toggle cinematic pan mode while RMB held)
    KB_COUNT      = 8,
};
```

### Adding a new control (complete checklist)
1. Add `KB_NEWNAME` before `KB_COUNT` in the enum
2. Add one line to `keybindings` in `init()`: `{"Display Name", GLFW_KEY_X, held, false}`
3. Bump `static_assert(KB_COUNT == N)` to the new count
4. Wire the action:
   - **Event** (`held=false`): `if (pressed(KB_NEWNAME)) { ... }` in `onKey()`
   - **Held** (`held=true`): `glfwGetKey(win, keybindings[KB_NEWNAME].key) == GLFW_PRESS` in `recordCompute()`

Settings display, rebinding, hover state, and `keyDisplayName()` all work automatically. `hovRebind[KB_COUNT]` is sized by the enum so no array changes are needed.

`keyDisplayName()` handles: letters, digits, Space, Tab, Enter, Esc, Bksp, modifier keys (LShift/RShift/LCtrl/RCtrl/LAlt/RAlt), F-keys (F1–F12), arrow keys, nav cluster (PgUp/PgDn/Home/End/Ins/Del), and common punctuation.

---

## Subsystem: Intro Cinematic (UC3)

`showIntro` drives `updateIntroCinematic()` (`SatelliteSim.cpp`, called from `recordCompute()` in
place of normal WASD/zoom input) through a fixed beat sheet, `kIntroKeyframes[]`
(`SatelliteSim.h`). `buildIntroOverlay()` (`SatelliteSimUI.cpp`) only draws the caption/skip-hint
text on top — the camera path itself *is* the cinematic. Whether it plays on launch is a real
persisted toggle, `playIntroOnStartup` (Display tab "Play intro on startup") — see the persistence
note near the bottom of this section; it is no longer forced on every launch.

**Window boots maximized** (`App::initWindow`, `glfwWindowHint(GLFW_MAXIMIZED, ...)` before
`glfwCreateWindow`) — windowed, not exclusive fullscreen, but full monitor work-area size by
default rather than the fixed `WIN_W`x`WIN_H` (1280x720). A small window the player has to
manually enlarge undercut the cinematic's impact and invited resizing instead of watching it.
`WIN_W`/`WIN_H` are still passed as the restore size for whenever the player un-maximizes later.

**Fixed vantage, locked every playback.** The intro always opens from `kIntroObserverLatDeg/LonDeg`
+ `kIntroStartAzDeg/ElDeg/FovDeg` (`SatelliteSim.h`) — the California coast at twilight facing the
SpaceX AI-datacenter satellites and the Reflect Orbital mirrors aimed at the nearby Topaz solar
farm — not whatever the player's current/saved observer position happens to be.
`updateIntroCinematic`'s one-time init block (`!introBasisValid`) force-sets
`obsDir`/`obsLatDeg`/`obsLonDeg`/`obsHeightOffset`/`camera.azDeg/elDeg/fovYDeg` from these
constants before computing the East/North tangent basis the rest of playback rides on. The
Display tab's "Replay Intro" button resets `introBasisValid = false`, so a replay re-locks to the
same spot regardless of where the player has since wandered off to. Also forces
`timeScaleIdx = 0` / `timePaused = false` / `timeDir = 1.0f` here — the beat sheet is tuned at 1x
and a replay runs on live state, not a fresh boot, so it can't rely on `loadSettings()` alone.

**`camera.azDeg` vs `obsFacing` — read this before touching keyframe azimuth.** `camera.azDeg` is
what `SkyCamera::viewMatrix()` actually renders with; `obsFacing` is only the ground-movement
tangent, and outside the intro `camera.azDeg` is *derived from* `obsFacing` every frame in
`buildUI` (search "Derive camera.azDeg from obsFacing"). That derivation is skipped entirely while
`showIntro` is true (same gate as the RMB-look block), so `updateIntroCinematic` must set BOTH
every frame — it used to set only `obsFacing`, which meant the rendered view never actually panned
in azimuth during playback and then snapped to match `obsFacing` on the first post-intro frame.
Fixed by computing the mixed azimuth once and assigning it to both.

**Beat sheet** (`kIntroKeyframes[]`, timings synced to `songbeat` = 7.61s, a music-tempo unit —
hand-tuned by the user, not derived): `kIntroYearIndex` (0) is a "2036" title card, bottom-anchored
like every other caption but at a larger font size (not centered — that was tried and reverted, it
read as inconsistent with the rest of the captions); `kIntroHintRevealIndex` (1) is the first
narrative line and also where the bottom-right skip hint first appears (not from frame 0 — see
below); the middle beats hold at ground level then pull to LEO; `kIntroTitleIndex` (6) is the
arrival "SAT LIGHT SIM" reveal; beat 7 (the last key, text null) settles the final framing while the
title fades, and the intro ends there (`kIntroBenchEndT` = its time: the whole intro feeds the UC1
benchmark).

**Camera path is a Catmull-Rom/cubic-Hermite spline, not per-segment smoothstep.** The original
implementation eased in/out (smoothstep) independently within each keyframe segment, which gives
zero velocity at *every* waypoint, not just the first and last — the camera visibly decelerated to
a stop and re-accelerated at every beat boundary, reading as a stutter rather than one continuous
move. `updateIntroCinematic`'s local `hermite(field)` lambda instead estimates a time-weighted
tangent at each interior key from its two neighbors and blends with cubic Hermite basis functions,
so velocity carries through a waypoint instead of resetting there. The start's tangent is the
one-sided difference (~0: beats 0-1 share their values); the LAST key's is zero, so the camera eases
to a stop on the handoff frame.

**No controls in the intro (2026-10-03).** Beat 7 used to be a "WASD to move / Q-E / click a
satellite" caption that unlocked movement and look (`kIntroControlsIndex`) while clicking stayed
disabled until the intro ended — and the post-intro "Click to select any satellite" hint
(`buildSelectHint`) then said it again. Both are gone: input is live only once `showIntro` is
false, and `finishIntro` starts the first-run tutorial (below).

**Dismissal is a single defined key, not "any key."** `onKey()` only calls `finishIntro(true)` for
literal `GLFW_KEY_SPACE` (independent of whatever `Pause/Resume` is currently rebound to);
`pollGamepad()` only responds to `GLFW_GAMEPAD_BUTTON_START`. Mouse clicks do NOT skip —
`buildIntroOverlay` still covers the screen with a mouse-capture rect so a click doesn't leak
through to satellite picking, but it no longer calls `finishIntro`. This was a real usability
issue in the previous "any key/click" version: almost no one saw the cinematic play out, because
touching the keyboard or clicking into the window to focus it immediately skipped it.

**The intro hides the entire normal HUD**, not just its own overlay — `buildUI()` checks
`showIntro` before icon loading/scroll-zoom/satellite-picking and before the `uiVisible` check, so
none of the left/right HUD panels, settings/view-controls windows, or scene-interaction input run
while the cinematic owns the camera, regardless of whether the player has the rest of the UI
toggled on or off.

**`timeScaleIdx`'s compiled-in default was `1` ("10x"), not `0` ("1x")** — a real bug, now fixed.
`loadSettings()` overrides it from `settings.json`'s `time.scale_idx` when present, which is why it
looked "sometimes" wrong: a genuine first run (or any state where that key was never written)
booted at 10x. The intro's one-time init above also force-sets it defensively, since a replay runs
on live state that loadSettings() never touches again.

**Persistence: `playIntroOnStartup`, not a raw `showIntro` save.** `showIntro` is runtime
play-state (flips false the moment the cinematic ends), so persisting it directly conflated "did
today's playthrough finish" with "should it play again next launch." `playIntroOnStartup` (Display
tab checkbox, next to "Replay Intro") is the actual user preference, saved every close and applied
to `showIntro` at load time: `playIntroOnStartup = d.value("play_intro_on_startup", false)` inside
the `"display"` block. Absent-key default is `false` there (not the compiled-in `true`) so a
player upgrading from a build that predates this key doesn't suddenly get a cinematic that didn't
exist in their version — a genuine first run (no settings.json at all) never reaches that line, so
it still keeps the compiled-in `true` default from the "no file" early-return branch. Disabling it
does not need to separately handle "resume at last known location" — `obsDir`/`camera` are always
restored from settings.json's own `"observer"`/`"camera"` blocks regardless of `showIntro`, so
turning the intro off just resumes wherever the player last was.

---

## Subsystem: First-run tutorial (2026-10-03, `SatelliteSimTutorial.cpp`)

One card at a time (`buildTutorial`, z 40) — a low, wide strip, the graphic beside the text, in the HUD's
palette (`Pal` / `Style`, `UIPalette.h`; wanted input pulses in the red accent, held input is solid red). Thirteen
steps (`TutStep`; a fourteenth, Graphics, only on the light presets: "no clouds", the preset to try, done by opening
Settings or changing the preset; `tutStepCount()`):
1. look, 2. move, 3. climb, 4. boost, 5. select — finished by DOING it, with a keyboard (Q W E / A S D / Shift, labels
   from the live bindings), mouse or gamepad graphic (`buildTutKeyboard/Mouse/Gamepad`; the pad's when
   `lastInputWasGamepad`), plus a progress bar;
6. **Info** (opening the info window on the selection, `tutInfoOn`), 7. **Go to** (the eye; once riding, the follow
   HUD's Stay here and Exit are outlined and explained; finished by leaving: Exit's flight home or Stay here),
   8. **Trace and Track** (either) — split 2026-10-21 from one "selection buttons" step any of the four finished. Each
   outlines only its own `SelActBtn` (id % 4). With nothing suitable selected (`tutNeedsPick`: Info / Go to need a
   geometry model) the card asks for a selection and offers **"Pick one for me"** (`TutPick`, `tutPickSatellite`: a
   sunlit, > 12 deg up station or Starlink with a model, else anything > 5 deg; the camera is aimed at it). Not on the
   pad view (no cursor; A selects at the centre);
9. time, 10. pictures — outlined HUD controls, finished by using one (a change from `tutBase*`) or Next;
11. **Missions: what's next** — the rocket button (or M) outlined until the window opens, then Upcoming's first launch's
   Watch and the Sun's Fury's (two outline boxes, `TutOutline:<g>`; one button each since 2026-10-24); finished by any of those
   (`msUpcomingActs_`, counted after a successful action in buildMissionsWindow);
12. **Missions: under way** — the Ongoing tab outlined; 5 s on it (progress bar) or any of its buttons
   (`msOngoingActs_`) finish it;
13. menus (Bookmarks, Cinematics, Issue reports, Settings).
The Missions cards sit above the left panel while the window (docked top right) is open, above the right panel while
it is closed. **The Missions step's jump has a way back:** entering it saves the time, place, camera, clock and cloud
drift (`tutSaveReturn`; not again once a jump happened); after the jump every later card shows **"Back to <UTC>"**
(`TutReturn`, `tutReturnNow`: bookmarkGo's cut) until used. A finished step shows "Done!" for 1.1 s and moves on.
- **Starts** from `finishIntro` (not on a replay of the intro), or on the first frame in the scene when
  the intro is off (`updateTutorial`, `tutAutoChecked`), once: Skip and Finish both set
  `display.tutorial_done` (absent = not done, so existing installs see it once). Settings > Display >
  STARTUP > Replay tutorial runs it again. A harness run never starts it on its own.
- **Detection** (`updateTutorial`, in buildUI right after the look block and before the click pick) reads
  the keys and gamepad values itself; it does not hook the movement code.
- **Clay keeps stale boxes** for elements not drawn this frame, so the selection buttons' outline uses only
  the `SelActBtn` ids `buildSelActionButton` drew this frame (`selBtnDrawnMask`).
- `keyDisplayName` / `gamepadButtonDisplayName` (SatelliteSimUI.cpp) are no longer file-static; keyDisplayName's
  letters rotate through a 4-slot pool, so keycap labels are copied (`tutKeyLabel`).
- A controller moves through the card with **Start = Next / Finish, View = Skip** (`padContextButton`).
- Harness: `tutorial start [step] [pad=1] | step <n> | next | back | skip | state`;
  `tools/harness/scripts/tutorial.satcmd` captures every card and clicks through the HUD steps.
