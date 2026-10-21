# UI (Clay)

The app's UI is built with [Clay](https://github.com/nicbarker/clay), a single-header immediate-mode layout
library, and drawn by `UIRenderer` as Vulkan quads. This page lists the rules that Clay and this renderer
impose, and describes the icon atlas, the bitmap font, embedded images and plots, text fields and long text,
hit-testing, window stacking and the colour themes. Which windows and panels exist is player-facing and covered in [Using](../using/index.md).

## How a frame of UI is built

1. `App` calls `UIRenderer::beginFrame()` with the mouse, buttons, scroll and window size. That resets Clay
   and saves last frame's "mouse over UI" result.
2. `SatelliteSim::buildUI()` declares the layout with the `CLAY(...)` and `CLAY_TEXT(...)` macros, reading
   input from `ui.input()` (`UIInput`: mouse position and delta, button down/pressed/released, `scrollY`
   positive for scrolling up, window size, `dt`).
3. After `recordDraw()`, `App` calls `UIRenderer::record()`, which ends the Clay layout and turns its render
   commands into vertices: one vertex format (`UIVertex`) with a `mode` of 0 for a solid rounded rectangle
   (an SDF in the fragment shader), 1 for a text glyph, 2 for an icon or image.

Scene interaction (picking satellites, dragging the camera) must not happen through UI panels.
`ui.addMouseCaptureRect(x, y, w, h)` registers each visible panel; `ui.mouseOverUI()` reports whether the
mouse was over one **last frame**, and scene input is gated on it.

## Clay rules

!!! warning "Invariant"
    Never `return` or `break` inside a `CLAY(...)` block. The macro is a `for` loop that closes the element at its
    end; leaving it early leaves the element open and corrupts the rest of the layout. Set a flag and act after
    the block.

- **`CLAY_IMPLEMENTATION` is defined only in `UIRenderer.cpp`.** Every other file includes `clay.h` without it.
- **Designated initialisers in declaration order.** MSVC requires C++20 designators in the order the members
  are declared: `Clay_LayoutConfig` is `sizing`, `padding`, `childGap`, `childAlignment`, `layoutDirection`;
  `Clay_ElementDeclaration` is `layout`, `backgroundColor`, `cornerRadius`, `aspectRatio`, `image`,
  `floating`, `custom`, `clip`, `border`, `userData`; `Clay_FloatingElementConfig` is `offset`, `zIndex`,
  `pointerCaptureMode`, `attachTo`.
- **Hover lags one frame.** `Clay_Hovered()` is valid only inside an element's body, not in its configuration.
  Store it in a member bool and use that bool for the element's colour next frame. Two elements must not share
  one hover bool, or they fight over it.
- **Strings.** `CLAY_STRING(x)` needs a string literal. A runtime string is
  `Clay_String{false, (int32_t)strlen(buf), buf}`, and `buf` must outlive `buildUI()` (a member or `static`
  buffer): Clay keeps the raw pointer until `record()`.
- **No `.clip` on a floating container that also has a `backgroundColor`.** The scissor starts before the
  rectangle is drawn and hides the background. Put the clip on a child.
- **Pointer capture.** A floating element with no explicit `pointerCaptureMode` captures the pointer: Clay's
  hit test stops at the first floating element under the mouse. That is right for a panel, and fatal for any
  purely visual floating element drawn *at the pointer* (a cursor dot, a drag ghost, a custom tooltip), which
  then blocks hover and clicks on everything beneath it. Such elements must set
  `.pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH`.
- **Scrollable containers** use `.clip = {.vertical = true, .childOffset = Clay_GetScrollOffset()}` on the
  content element, and call `ui.scrollbar(CLAY_ID("<that container>"))` right after it closes. The scrollbar is
  a thin thumb along the right edge; without it a long panel looks as if it simply ends at the window edge.
- **Clips nest.** `UIRenderer::record()` keeps a scissor stack: a clip inside a clipped scroll view is
  intersected with it, and its end restores the parent's scissor.
- **Single-line labels** (window titles, button labels) use `CLAY_TEXT_WRAP_NONE` inside a horizontally
  clipped box, or a large UI scale wraps them out of their bar.
- **Text sizes** go through `fs(base)` (`SatelliteSim.h`), which multiplies by the UI scale and never returns
  less than 8 px.

## Windows

`buildResizableWindow()` (in `SatelliteSimUI.cpp`) draws a movable, resizable window with a title bar.
`UIRenderer::updateWindowChrome()` applies drag and resize to a `WindowChrome` (position, size, drag state):
call it once per window before building its tree. It arms edge and corner resizing itself (a plain rectangle
test against the edges, with OS resize cursors) and keeps drags anchored to where they started, so the cursor
and the window edge do not drift apart after a clamp. The title bar's drag is armed by the caller when its
title bar is pressed, and is skipped when the press is on a title-bar icon.

**Stacking.** Every window is a floating root at `zIndex` 10, and Clay sorts roots stably, so the window declared
later in `buildUI()` is drawn on top. A window's own floating children (chips over an image, plot tick labels,
marker labels) must use the **same** `zIndex` as the window; a higher one draws them over any later window that
covers their parent.

**Low resolutions.** Every builder reads `uiScale`, the effective scale set once per frame at the top of
`buildUI()` (`updateUiScale()`): the setting `uiScaleUser` (`display.ui_scale`) times `uiAutoScaleFactor()` =
clamp(window height / 1000, 0.7, 1) while `uiScaleAuto` (`display.ui_auto_scale`) is on. `fs()` never lets the
automatic factor take text below 16 px. Window sizes and minimums are capped at the screen less 8 px every frame
(`buildResizableWindow()`), and windows are kept on screen after a resolution change. A window's default size is
its 1080p size where it fits and a share of the screen otherwise, so give a new window a share-based default
rather than a fixed pixel size. `tools/harness/ui_lowres.py run` opens every window alone at its default and
checks it, and the HUD, at 1024 x 768, 1280 x 720, 1366 x 768, 1600 x 900 and 1920 x 1009
([Harness](harness.md#analysis-tools)).

**Opacity.** A window's panel alpha is *Window opacity* (Display tab, `display.window_opacity`, 60 to 100%,
default 100% = opaque), applied in `buildResizableWindow()`.

## Hit-testing outside Clay

Clay does not expose element positions while the layout is being declared. Two patterns are used:

- **Last frame's laid-out box.** `Clay_GetElementData()` returns an element's bounding box from the previous
  frame. Mouse-capture rectangles for panels whose size depends on their content come from these real boxes
  (`captureLaidOut()`), not from size estimates: a click on part of a panel outside an estimated rectangle
  would fall through to the scene.
- **Computed positions.** Where a position must be known in the same frame, compute it from the same
  constants as the Clay sizing declarations, and wrap labels in `CLAY_SIZING_FIXED` containers so the laid-out
  width matches the arithmetic.

## Icons

Icons are white glyphs with an alpha channel, 48 x 48 px, drawn at about 16 px. They are **generated, not
hand-drawn**: `tools/make_icons.py` holds each icon as a list of shapes (small predicates over a point in the
48 x 48 box) and writes `assets/icons/ui/pixel--<name>.png` with 3x supersampled edges. It prints a 48 px preview
and a box-filtered 16 px one, the size actually drawn, which is the preview to judge.

```bash
python tools/make_icons.py              # every icon
python tools/make_icons.py spin studio  # only these
```

Do not retouch the PNGs by hand: the next run of the script overwrites them.

To add an icon:

1. Add its `build_*` function and its `ICONS` entry in `tools/make_icons.py`; run it and check the 16 px
   preview.
2. Append its path to the `iconPaths[]` list in `buildUI()`'s lazy-load block (`SatelliteSimUI.cpp`). The count
   is computed from the array.
3. Add a `kIcon*` index constant at the top of `SatelliteSimUI.cpp`.
4. Build: the runtime-file sync copies the new PNG next to the executable.

`UIRenderer::loadIcons()` packs the PNGs into one atlas on the first frame and returns how many loaded; a
missing file becomes a 1 x 1 magenta placeholder. The sim logs `UI: <n>/<count> icons loaded (...)` to
`satlight_log.txt`, which is the first thing to check when a new icon does not appear. An icon is drawn with
`.image = {.imageData = (void*)(intptr_t)iconIdx}`. The renderer draws icons untinted, so contrast against the
background is the caller's problem (see the translucent chip colours in the palette).

## The font

`UIRenderer::loadFont()` bakes ASCII 32 to 126 once with `stbtt_BakeFontBitmap` at 48 px into a 768 x 768
single-channel atlas. Every requested font size scales that one bitmap; there is no rasterisation per size.
Text near the baked size or below is sharp; much larger text (the intro's title captions) is an upscaled
bitmap. `loadFont()` logs a warning if the baker drops glyphs for lack of atlas space. True resolution
independence would need a signed-distance-field font, which is not implemented.

The atlas is ASCII only: there are no media symbols or arrows, so UI text uses ASCII stand-ins (`<<`, `||`,
`>>`, `+`/`-` section headers).

## Images and plots

Two Clay *custom elements* draw things Clay cannot:

**`UIImage`** draws a texture the caller owns (an offscreen render, such as the satellite 3D view) over the
element's box. `UIRenderer::registerImage(device, view, sampler)` returns an id (at most 4 images,
`kMaxExternalImages`; the 3D view, the Bookmarks atlas and the Issue reports atlas take three, so a new window
with many images packs them into one atlas and shows sub-rectangles); `updateImage()` re-points it after the owner recreates the image. The image must be in
`SHADER_READ_ONLY_OPTIMAL` whenever the UI pass runs and hold display-ready values in the swapchain's format
family. `u0, v0, u1, v1` select a sub-rectangle, so one render can be shown in two elements of different aspect
without stretching. Point the element's `.custom.customData` at the `UIImage`.

!!! warning "Invariant"
    Never give a `UIImage` element its own `backgroundColor`. Clay emits the custom element before that element's
    rectangle, which then covers the image. Put the backing colour on a parent.

**`UIPlot`** draws line series inside the element's box (`UIPlotSeries`: x and y arrays normalised to the box,
0..1 left to right and bottom to top, a colour and a pixel thickness; a NaN y breaks the line). It is drawn as
axis-aligned quads, with no separate line pipeline. The pass-trace window's magnitude plot uses it. The
`UIPlot` and its arrays must stay alive until `record()`.

## Text fields

`textField()` edits a string; `numberField()` and `numberFieldD()` show a value as text that becomes an edit
box when clicked. Every shared slider row's value, the Photometry rows, the volumes and the Cinematics window
use them.

- One field at a time has the keyboard (`textEdit_`). Enter, Tab or a click elsewhere commits; Esc cancels;
  Ctrl+A, Ctrl+C and Ctrl+V work. Typed numbers are clamped to the slider's range.
- A commit is applied when the field is **next drawn**: the field sees that its edit ended and returns true,
  so no callbacks are needed. A field that is not drawn in a frame (its window closed, its section collapsed)
  loses focus. `textEditEndFrame()` runs at the end of every `buildUI()` path through a scope guard.
- While a field is focused, `onKey()` sends every key to it before anything else, `onChar()` types into it,
  the polled movement keys (WASD, Q/E, zoom) are ignored, and Esc does not close the app
  (`capturesKeyboard()`).
- Display strings live in `textFieldBufs_`, because Clay keeps the pointers until `record()`.

**Range.** A slider row's typed value is clamped to the slider's range, unless *Dev mode: unbounded values* is on
and the row allows it ([Controls and settings](controls-and-settings.md#range-guards-and-unbounded-values)).

The harness drives them with `ui click`, `ui type` and `ui key` (see
`tools/harness/scripts/text_fields.satcmd`).

### Long and user-edited text

Anything long, or typed by the user, is drawn as a **`UITextBlock`**, not a `CLAY_TEXT`. Clay measures every text
element through a word cache (`Clay_SetMaxMeasureTextCacheWordCount`, 65 536 words here); each distinct string is
an entry holding one item per word, so text that changes with every key press fills it and Clay stops laying out
text. A `UITextBlock` is a custom element holding runs of text at given positions and coloured quads under them;
the caller wraps the text itself (`UIRenderer::textAdvance()` gives a run's width) and passes only the visible
lines. The block, its arrays and its strings must stay alive until `record()`.

`textArea()` is the multi-line box built on it (the issue reports' description and notes, the note bar). It
grows from `minLines` to `maxLines`, then becomes a Clay scroll container with a scrollbar thumb, and follows the
caret. A click places the caret (it never selects all), a drag or Shift with the arrows selects, a double click
takes a word; Up and Down move by wrapped line, Ctrl+Left and Right by word; Ctrl+A, C, X and V work. Enter is a new
line and Ctrl+Enter, Tab or a click elsewhere commits; `enterCommits` swaps Enter and Shift+Enter. With a
`readOnlyColor` it shows a long text the same way without a frame. `maxLen` is enforced on typing, paste and Enter.

!!! warning "Invariant"
    A drag inside a scroll view (a text selection, a rubber band over an image) calls `ui.holdDragScroll()` every
    frame of the drag. Clay's drag-scrolling is on, and without the hold the view under the pointer moves with the
    drag.

Clay's error handler prints each kind of error once, so a capacity error in the log appears a single time however
often it recurs.

## Tooltips

`ui.tooltip(inp, hovered, text, fontSize)` draws a small floating box near the cursor, flipping left or up at
the screen edges. Call it right after computing the element's hover bool. Icon-only buttons use the action's
**name** as the tooltip, never a sentence.

## The palette and themes

`src/simulations/UIPalette.h` holds every UI colour as one row of the `UI_PALETTE` X-macro, with one column per
theme: **Classic** (near-black panels, the red accent), **Navy** (blue-black panels, a deep azure accent), **Night
red** and **Midnight** (Classic's colours on darker, more opaque panels; the default). The `Pal::` names (panel and
title-bar backgrounds, button states including the orange danger pills, text tiers, the speed indicator, the amber
selection reticle, the translucent chips over the 3D view) are ordinary globals that `applyUiTheme()` rewrites, so
call sites read `Pal::btnAccent` and follow the theme. `Style::borderColor` follows `Pal::windowBorder`.

The theme is `display.ui_theme` (`classic`, `navy`, `night`, `midnight`; Display tab *UI theme*). `syncUiTheme()`,
called in `buildUI()` after the harness, applies a change in the frame it was made; the startup graphics chooser
reads the setting before `init()` so it is themed too. Night red also turns on `UIRenderer::setColorFilter()`, which
maps every colour the renderer draws (icons, images, the 3D view, thumbnails, any literal colour) to red at its
brightest channel, so that column only has to order colours by brightness. The theme colours the interface only,
not the sky.

!!! warning "Invariant"
    A new UI colour goes in the `UI_PALETTE` table with a value for every theme, never as a `Clay_Color{...}`
    literal at the call site, or it ignores the theme. The swapchain is sRGB: a palette value of 12 shows as about
    61, so dark tints look much more saturated on screen than their numbers suggest.

## Inspecting a layout

The harness's `ui dump [name]` records every drawn rectangle, text, image and scissor with its box and element
id, and checks for text cut by its scissor, text off the window, and overlapping text under the same scissor
(`UIRenderer::requestLayoutDump()`). Read those checks before looking at a screenshot. Element ids are what
`ui click` targets. See [Automation harness](harness.md).

## Where in the code

| File | What |
|---|---|
| `src/UIRenderer.h/.cpp` | Clay setup, `beginFrame()`, `record()`, `loadFont()`, `loadIcons()`, `registerImage()`, `UITextBlock`, `textAdvance()`, `setColorFilter()`, `holdDragScroll()`, `updateWindowChrome()`, `tooltip()`, `scrollbar()`, layout dump |
| `src/simulations/SatelliteSimUI.cpp` | every panel and window, `buildResizableWindow()`, the text fields and `textArea()`, slider rows, `syncUiTheme()` |
| `src/simulations/UIPalette.h` | `UI_PALETTE`, `applyUiTheme()`, the theme names |
| `tools/make_icons.py` | icon geometry |
| `shaders/ui.vert`, `shaders/ui.frag` | the UI shaders |
