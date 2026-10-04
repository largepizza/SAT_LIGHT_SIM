# Controls and settings plumbing

How input bindings, the gamepad, `settings.json`, the settings window's sliders and the graphics presets are
wired, and the checklists for adding a control or a setting. The player-facing control list is on
[Controls](../using/controls.md); the presets as a player sees them are on
[Graphics settings](../using/graphics-settings.md).

## Key bindings

Every rebindable action is one entry in the `keybindings` vector (`SatelliteSim.h`), indexed by the `KB` enum.
The Controls tab, the rebinding UI, hover state, key-name display and persistence are all driven from that
vector, so a new entry needs no UI code.

```cpp
struct KeyBinding {
    const char *action;   // display name, also the key in settings.json
    int key;              // GLFW_KEY_*
    int gpButton = -1;    // GLFW_GAMEPAD_BUTTON_*, -1 = none
    bool held = false;    // true = polled every frame, false = an event (pressed once)
    bool listening = false;     // waiting for a new key
    bool listeningPad = false;  // waiting for a new pad button
};
```

The default table is initialised in `SatelliteSim::init()`:

| Index | Action | Key | Pad | Kind |
|---|---|---|---|---|
| `KB_TOGGLE_UI` | Toggle UI | Tab | View (Back) | event |
| `KB_PAUSE` | Pause/Resume | Space | X | event |
| `KB_SLOWER` | Slow Down | `,` | D-pad down | event |
| `KB_FASTER` | Speed Up | `.` | D-pad up | event |
| `KB_REVERSE` | Reverse Time | R | Y | event |
| `KB_MOVE_BOOST` | Move Fast | Left Shift | left stick click | held |
| `KB_MOVE_FINE` | Move Fine | Left Ctrl | none | event (toggles fine movement) |
| `KB_CINEMATIC` | Cinematic Pan | Left Alt | none | event (toggles camera drift while panning) |
| `KB_RAISE_ELEV` | Raise Elevation | Q | right trigger (analog) | held |
| `KB_LOWER_ELEV` | Lower Elevation | E | left trigger (analog) | held |
| `KB_RESET_ELEV` | Reset Elevation | Z | none | event |
| `KB_ZOOM_IN` | Zoom In | `=` | right bumper | held |
| `KB_ZOOM_OUT` | Zoom Out | `-` | left bumper | held |
| `KB_ZOOM_RESET` | Reset Zoom | 0 | right stick click | event |
| `KB_SELECT_SAT` | Select Satellite | T | A | event (the satellite nearest the screen centre) |
| `KB_SCREENSHOT` | Screenshot | F12 | none | event |
| `KB_TOGGLE_CURSOR` | Toggle Cursor | C | Start (Menu) | event (the gamepad's virtual cursor) |
| `KB_TOGGLE_TRAILS` | Star Trails | F | none | event |
| `KB_SAVE_SNAPSHOT` | Save Snapshot | F9 | none | event |
| `KB_PHOTO` | HQ Photo | F8 | none | event |

Event actions are dispatched by `dispatchKeyAction()`, shared by `onKey()` (keyboard) and `pollGamepad()`
(gamepad edge detection), so the two input paths cannot drift apart. Held actions are polled each frame in
`recordCompute()` with `glfwGetKey(win, keybindings[KB_X].key)` and `gpHeld(KB_X)`. The triggers for raising
and lowering are read as analog values, not through the binding's button.

### Adding a control

1. Add `KB_NEWNAME` before `KB_COUNT` in the enum.
2. Add one line to the `keybindings` initializer in `init()`: `{"Display Name", GLFW_KEY_X, padButtonOr-1, held, false}`.
   The initializer must stay in enum order.
3. Bump the `static_assert(KB_COUNT == N)` right after it.
4. Wire the action: for an event, a `case KB_NEWNAME:` in `dispatchKeyAction()`; for a held action, a poll in
   `recordCompute()`.

The settings display, rebinding, hover arrays (sized by `KB_COUNT`), the key-name display
(`keyDisplayName()`) and persistence then work without further changes.

Bindings are saved under `controls.keybindings` as a list of `{action, key, gp_button}`, matched by action
name on load. An action missing from the file keeps its default.

## The gamepad

`pollGamepad()` runs once per frame from `recordCompute()`. It finds a connected gamepad, edge-detects button
presses, handles pad rebinding, and fills the movement and look values from the sticks.

Before a press reaches the bindings, **`padContextButton()`** gets a chance to consume it. It gives buttons a
meaning that depends on what is on screen:

| State | Button | Meaning |
|---|---|---|
| the first-run tutorial is showing | Start / View | next card / skip the tutorial |
| a satellite is selected | D-pad left / right | move the focus ring over the selection panel's buttons (Info, Go to, Trace pass, Track) |
| a satellite is selected | A | press the focused button, through that button's own click path |
| any | B | go back one layer: close the 3D view, info or trace window, then leave Go to, then release Track, then clear the selection |

The focus ring and a hint line under the selection panel show only while the gamepad was the last input
device. The windows themselves are operated with the virtual cursor (Start toggles it), which overrides the
mouse for the UI pass (`Simulation::virtualCursor()`).

The default pad layout has a version number (`kPadLayoutVersion`, saved as `controls.pad_layout`). When a
settings file's layout is older, its saved pad buttons are dropped once (keys are kept), so an old binding
does not collide with a button the new layout gives a context meaning.

The harness's `pad <button>` presses a button without a controller, through the same path.

## `settings.json`

### Location and lifetime

`settings.json` lives in the user data folder: next to the executable when that directory is writable,
otherwise `%APPDATA%\SatLightSim` (Windows), `$XDG_DATA_HOME/SatLightSim` or `~/.local/share/SatLightSim`
(Linux), `~/Library/Application Support/SatLightSim` (macOS). It is read once during `init()` and written when
the settings window closes and when the app exits. With no file (a first run), every compiled-in default is
used and the graphics preset is seeded from the device type: Medium for a discrete GPU, Low otherwise.

The `SatLightSimFresh` build target never reads or writes the file, so every launch is a first run.

### One read path, one write path

| Function | Role |
|---|---|
| `loadSettings()` | reads the file, then calls `applySettingsJson(j, false)` |
| `applySettingsJson(j, isPatch)` | applies every key present; every field reads `member = section.value("key", member)`, so an absent key leaves the member unchanged |
| `buildSettingsJson()` | builds the whole object |
| `saveSettings()` | writes `buildSettingsJson()` |

Because every field is read as "the value in the file, else the current value", a partial object changes only
the keys it contains. The [harness](harness.md)'s `set section.key value` is exactly that: it builds a one-key
patch and calls `applySettingsJson(patch, true)`. A patch skips the schema check, does not re-arm the intro
and does not re-derive a preset unless it names the preset. The harness's `get` reads `buildSettingsJson()`.
`set` refuses a key that `buildSettingsJson()` does not produce, and refuses a non-number for a number.

**Adding a setting:** add a member with its default in `SatelliteSim.h`, read it in `applySettingsJson()` and
write it in `buildSettingsJson()`, under the same section and key. That is all the harness needs: the setting
is immediately scriptable with `set` and `get`, with no harness code.

### Schema version

The file carries `schema_version` (`kSettingsSchemaVersion`), `app_version` and `git_commit`. When the schema
version differs, the graphics-affecting sections (photometry, clouds, render scale) are reset to their
defaults and everything else (camera, audio, key bindings, observer, constellation toggles) is kept. Bump the
version only when stored graphics values would not make sense against the current code.

## Slider slots

Most tuning sliders (the Clouds, Weather, Atmosphere, Terrain, Night lights, Ocean, Beams tabs and the Sound
tab's advanced mix) are rows built by `buildCloudSliderRows()` from `CloudSlider` entries:

```cpp
struct CloudSlider { const char *label; float *val; float vmin, vmax, step; const char *fmt; int idx; };
```

`idx` is the row's **slot**: an index into four per-slider arrays (`hovCloudMinus`, `hovCloudPlus`,
`draggingCloud`, and the value-text buffers `cloudBufs`). All four are sized by one constant,
`kCloudSliderSlots`.

!!! warning "Invariant"
    A new slider takes an unused slot index below `kCloudSliderSlots`; raise the constant when all are taken.
    Two rows on screen at once must never share a slot. An index past the arrays' size writes into the memory
    declared after them (the window-chrome state), which breaks unrelated UI with no error.

A slider's slot and its settings key are independent of which tab shows it, so a slider can move between tabs
without touching anything else. The Photometry tab has its own smaller set of arrays (`hovPhotoMinus`,
`hovPhotoPlus`, `draggingPhoto`), sized for its rows. Editing a slider on any tab except Sound switches the
graphics preset label to Custom (`marksPreset`).

## Settings tabs and sections

The tab names are `kSettingsTabNames[]` in `SatelliteSimUI.cpp`. A tab's **index** is persisted
(`display.active_tab`) and indexes the tab hover array, so a new tab is **appended** to the array; the order
on screen is a separate list, `kSettingsTabOrder[]`. `settingsTabIsAdvanced()` is the single test for "shown
only with Show advanced settings" (used by the UI, the toggle and the harness), and
`settingsTabIndexByName()` finds a tab by name (it also accepts "Aurora" for the Atmosphere tab).

Long tabs group their sliders into collapsible sections with `buildCloudSliderSections(..., base)`. Each tab
owns a range of section slots (`cloudSectionOpen[]`, sized `kCloudSectionSlots`): Clouds 0 to 11, Weather 12
to 23, Atmosphere 24 to 29, Terrain 30 to 35. Section open state is session state and starts collapsed.

## Graphics presets

`GraphicsPreset` is `Planetarium, Low, Medium, High, Ultra, Custom, Potato`. The persisted value is the enum's
integer, which is why Potato comes after Custom: appending kept every existing index stable.
`applyGraphicsPreset()` owns a fixed set of fields, set from one `PresetValues` row per preset:

| Field | What |
|---|---|
| `debugDisableMask` | knockout bits, including the two sky-tier pipeline swaps ([Profiling](profiling.md#knockout-bits)) |
| `renderScale` | background render scale |
| `cloudCoverage` | the legacy 2D cloud coverage |
| `cv2MaxIters`, `cv2LightSteps`, `cv2StepGrowth` | volumetric cloud march budget |
| `viewSamplesMin/Max`, `lightSamples` | the atmosphere march |
| `oceanSeaOctaves`, `oceanDetailOctaves`, `oceanReflSamples` | sea quality |
| `terrainDistFadeStartM/EndM`, `cloudDistFadeStartM/EndM` | reach of terrain and clouds |
| terrain detail, shadow, material and close-up texture strengths | on for Medium, High and Ultra only |

Everything outside this list belongs to the user and is never touched by a preset. Satellite part occlusion,
in particular, is opt-in and no preset or first run turns it on: it multiplies the cost of the orbit pass
several times over at millions of satellites.

**High is the compiled-in defaults.** Its row repeats the member defaults from `SatelliteSim.h`. Change a
default that appears in `PresetValues` and the High row in the same edit.

## Defaults

The compiled-in member initialisers in `SatelliteSim.h` are the out-of-box look. Tuned defaults are written to
two significant figures for legibility; round a value taken from a tuned settings file before baking it in.

`tools/settings_defaults_diff.py [settings.json]` lists every key in a settings file whose value differs from
its compiled-in default. It parses `applySettingsJson()` to find the member behind each key and reads that
member's initialiser. Pointed at a tuned file, it shows which values would have to move into the code for a
first run to look the same; runtime state (camera, observer, window geometry) is expected to differ.

## Movement speed

WASD movement (the W, A, S, D keys are fixed, not part of the binding table; the left stick adds to them) is
an arc rate along the ground. At normal speed it is *Move speed (x height per s)* (Controls tab,
`camera.move_speed_per_height`, default 1) times the height above the ground per second, never below 3 m/s and
never above 0.08 rad/s of arc (about 510 km/s, so movement in orbit is fast). Boost multiplies the rate by
6.25 (cap 0.5 rad/s) and fine movement by 1/16. Q and E move vertically at the same setting times
(10 m/s + 0.5 x the height above the ground), x10 with boost and x0.1 with fine movement, never below the
ground. While a text field is focused or a cinematic is playing, movement keys are ignored.

## Where in the code

| File | What |
|---|---|
| `src/simulations/SatelliteSim.h` | `KeyBinding`, `KB`, `GraphicsPreset`, `CloudSlider`, slot constants, every setting's member and default |
| `src/simulations/SatelliteSim.cpp` | the `keybindings` initializer, `dispatchKeyAction()`, `onKey()`, `pollGamepad()`, `padContextButton()`, `padBack()` |
| `src/simulations/SatelliteSimUI.cpp` | `loadSettings()`, `applySettingsJson()`, `buildSettingsJson()`, `saveSettings()`, `applyGraphicsPreset()`, `seedGraphicsPresetFromDevice()`, `buildCloudSliderRows()`, `buildCloudSliderSections()`, the tab tables |
| `tools/settings_defaults_diff.py` | defaults diff |
