# Controls

This page lists every control with its default binding, explains how movement speed adapts to your height, and
covers rebinding, follow mode and typing values into the settings. What the buttons in the interface do is on
[Features tour](features.md).

Most controls can be rebound in Settings → **Controls**. The movement keys **W A S D** cannot; they are fixed.

## Keyboard and mouse

### Looking and zooming

| Input | Action |
|---|---|
| Right mouse button, drag | Look around (the cursor is captured while the button is held) |
| Scroll wheel | Zoom (changes the field of view) |
| `=` / `-` (held) | Zoom in / out |
| `0` | Reset the zoom |
| Left Alt, while dragging with the right button | Toggle **Cinematic Pan**: the view keeps drifting smoothly after you move the mouse |

### Moving

| Input | Action |
|---|---|
| `W` `A` `S` `D` | Walk or fly forward, left, back, right over the Earth |
| `Q` (held) | Climb |
| `E` (held) | Descend (never below the ground) |
| `Z` | Back down to the ground |
| Left Shift (held) | Move fast (boost) |
| Left Ctrl | Toggle fine (slow) movement. Pressing boost switches fine mode off |

### Time

| Input | Action |
|---|---|
| Space | Pause / resume |
| `,` | Slower |
| `.` | Faster |
| `R` | Reverse the direction of time |

Time runs at one of nine speeds: 1x, 10x, 1 minute, 5 minutes, 1 hour, 1 day, 1 week, 1 month and 1 year per
second.

### Selecting, pictures, windows and the rest

| Input | Action |
|---|---|
| Left click | Select the satellite, rocket, fireball or planet under the cursor (a planet wins when both are under it) |
| `T` | Select the object nearest the centre of the screen |
| `F` | Star trails (long exposure) on / off |
| F12 | Screenshot, without the interface (unless *Screenshots include the UI* is on) |
| F10 | Screenshot with the interface |
| F8 | HQ photo: a supersampled, settled picture (see [Features](features.md#pictures)) |
| F9 | Save a shot into the active issue report (see [Reporting a problem](features.md#reporting-a-problem)) |
| F7 | Start a new issue report with a shot of the current view |
| `M` | Open or close the [Missions](missions.md) window |
| `]` / `[` | Skip to the next / previous [mission milestone](missions.md#mission-milestones) of the followed or selected launch object |
| `G` | Leave Go to and stay where the camera is (see [Follow mode](#follow-mode-go-to)) |
| `H` | Go to: return to the followed object's default view, or follow the last object again |
| Tab | Hide / show the interface |
| `C` | Gamepad virtual cursor on / off (no effect with a mouse) |
| F11 | Fullscreen / windowed (fixed key, works during the intro) |
| Esc | Quit. While a cinematic plays or exports, Esc stops it instead; while a text field is being edited, Esc cancels the edit; while a key or button rebind is waiting, Esc cancels it; while the issue note bar is open, Esc closes it |
| `` ` `` | Developer console (see [Automation harness](../development/harness.md)) |

**Scrolling over the position readout.** Hover the latitude, longitude or altitude at the bottom right and scroll
to change it: 5 degrees per notch for latitude and longitude, 5% of the altitude (at least 10 m) per notch for
altitude. Hold the boost key for five times the step, or the fine key for a fifth of it. Clicking one of them, or
the clock at the bottom left, lets you type a value instead (see [Typing values](#typing-values)).

## Gamepad

Any controller that GLFW recognises as a gamepad works (Xbox and PlayStation layouts and most others). Names below use the Xbox layout.

| Control | Action |
|---|---|
| Left stick | Move (adds to `W A S D`) |
| Right stick | Look |
| Right trigger | Climb (pressure sets the speed) |
| Left trigger | Descend |
| A | Select the object nearest the centre; with a selection, press the focused selection button |
| B | Back (see below) |
| X | Pause / resume |
| Y | Reverse time |
| D-pad up / down | Faster / slower |
| D-pad left / right | With a satellite selected, move the focus over its buttons |
| LB / RB | Zoom out / in |
| Right stick click | Reset the zoom |
| Left stick click (held) | Move fast (boost) |
| View (Back) | Hide / show the interface |
| Menu (Start) | Virtual cursor on / off |

**Selection buttons.** With a satellite selected, D-pad left and right move a highlight ring over its buttons
(Info, Go to, Trace pass, Track) and **A** presses the highlighted one, exactly as a click would. A hint line under
the panel shows which is focused. The ring is shown only while the gamepad was the last thing you used.

**B goes back one layer**: it closes the satellite's info window, 3D view or trace window first, then ends Go to,
then releases Track, then clears the selection.

**Virtual cursor.** Menu (Start) turns on a cursor driven by the right stick; **A** clicks with it. Use it for the
settings and the other windows, which are mouse-driven. While the cursor is on, the right stick does not look
around and A does not select at the centre.

**Intro and tutorial.** During the intro, Menu (Start) skips it. While the tutorial card is up, Menu (Start)
goes to the next card and View skips the tutorial.

**Inverting the look axes.** Settings → Controls → *Invert look* has separate switches for the mouse and
the controller, horizontal and vertical.

## Movement speed follows your height

Near the ground, walking speed is proportional to your height above the ground, so you can approach a cloud, a
ridge or a satellite mesh without overshooting it, and still cross a continent quickly from orbit.

| Mode | Horizontal speed | Vertical speed (`Q`/`E`) |
|---|---|---|
| Normal | *k* × height above ground per second, between 3 m/s and about 510 km/s | *k* × (10 m/s + 0.5 × height above ground) |
| Boost | 6.25 × normal, up to about 3200 km/s | 10 × normal |
| Fine | 1/16 × normal | 0.1 × normal |

*k* is **Move speed (x height per s)** in Settings → Controls (default 1, range 0.05 to 5). The horizontal speed
is an angle over the Earth (0.08 radian per second at most in normal mode), so the cap is the same at every
height. You can climb to 100 000 km above sea level; `Z` puts you back on the ground.

| Setting | `settings.json` key | Default |
|---|---|---|
| Move speed (x height per s) | `camera.move_speed_per_height` | 1 |
| Invert mouse X / Y | `controls.invert_mouse_x` / `invert_mouse_y` | off |
| Invert controller X / Y | `controls.invert_pad_x` / `invert_pad_y` | off |

## Follow mode (Go to)

**Go to**, in a selected object's panel, its satellite window or a row of the Missions window, flies the camera out
to it and keeps it there, riding along (only for objects with a [geometry model](../modding/satellite-models.md):
modelled satellites, rockets and debris pieces). From the ground the flight takes 0.8 to 2.5 s and arrives on the
line from your ground position to the object, so you see the same side of it, and the same flare, that you saw from
below. When you are already following something, Go to on another object flies straight from where the camera is,
keeping the camera's direction and closing the distance, so it does not pass through the new object. Any movement
key during the flight skips to its end.

While following:

- `W A S D`, `Q` and `E` move the camera around the object, in your own horizontal frame. The speed is 0.6 ×
  the distance per second (0.5 m/s to 20 km/s), times 10 with boost or 0.1 in fine mode.
- The camera never goes below the ground or the sea: at the ground it slides along it.
- A bar at the top of the screen shows the object, its distance and these buttons:

| Button | Key | What it does |
|---|---|---|
| **Aim: locked** / **Aim: free** | | Locked keeps the object centred while you move; looking around with the right mouse button switches to free |
| **Return** | `H` | Flies back to the default view of the object, behind and above it |
| `<` and **Next: ... >** | `[` / `]` | For a launch object, the previous and next [mission milestone](missions.md#mission-milestones) |
| **Stay here** | `G` | Leaves Go to without flying home: you become a free observer exactly where the camera is, with its view and zoom. Paused, the object stays beside you; with time running it flies on |
| **Exit** | | Flies you back to where you were on the ground |

Pressing **Go to** again (it reads **Leave** in the satellite window), or B on a gamepad, also ends it.

**Following through a mission.** A followed rocket carries on through its flight: at staging the camera stays with
the upper stage, through its burn, coast, deployment and deorbit, and into its re-entry fireball or, for Starship's
ship, its return. A satellite in a train hands over to its slot in the constellation on arrival, and a retiring
satellite or a debris piece to its re-entry fireball. Through every milestone and hand-over (staging, a capsule's
separation, the ship's entry and descent, a catch or a landing) the camera keeps your offset and view relative to the
object: it glides across the hand-over's small jump with its view held in the object's frame, and the object keeps
its place on screen. For a rocket, ship, capsule or fireball the frame's forward direction turns smoothly toward the
direction of travel (at most 1.5 deg per sim second, or 20 deg per real second under time warp) instead of
snapping to it, and a reversal such as a booster's boostback keeps the side you were on. When nothing continues the
object, Go to ends where the camera is, with a notice.

**Following again.** For 30 seconds after you leave Go to, a chip at the top of the screen, **Go to** *name* **again**
(the name alone, without the object's status), follows the same object again, flying from where the camera is; the
`H` key does the same for the rest of the session. A rocket is carried along its mission to the present first, and a
visiting spacecraft (a Progress, a Dragon) that is still flying is followed again too; an object that has since ended
is reported gone. The chip disappears when time jumps by more than ten minutes.

As with any observer, only the latitude and longitude of a free observer are saved: after a restart you start on the
ground below.

**Track** is different: it leaves you where you are and only turns the camera to keep the object centred as it
crosses the sky. You can still walk with `W A S D` and zoom freely; mouse and stick look are ignored while it holds
the camera. Press **Track** again, select something else, or press **Go to** to release it.

## Rebinding

Settings → **Controls** → *Bindings* lists every rebindable action in three columns: the action, its keyboard key
and its gamepad button.

- **Rebind**, then press the new key. Esc cancels (it does not quit while a rebind is waiting).
- **Bind**, then press the new controller button. A button already held when you click is ignored until
  released. Esc cancels this too.

Climb and descend on the gamepad are the analog triggers and are not part of the button bindings; binding a button
to *Raise* or *Lower Elevation* adds it as a full-speed alternative. Bindings are saved in `settings.json`.

The rest of the Controls tab holds *Movement* (the move speed below), *Mouse* (a reminder that the right button
drag looks around, the wheel zooms and a left click selects), *Invert look* (mouse and controller, each axis) and
*Help*, whose **Controls overlay** switch opens a small window listing the current bindings, in keyboard or
gamepad order depending on which you used last.

## Typing values

Every slider value in the settings, the Photometry rows, the volumes, the fields in the cinematics and bookmarks
windows, and the clock, latitude, longitude and altitude on screen can be typed. Click the value and it becomes a
text box. The formats the clock and the position fields accept are on
[Features tour](features.md#the-screen).

| Key | Action |
|---|---|
| Enter, Tab, or a click elsewhere | Apply |
| Esc | Cancel |
| Left / Right, Home / End | Move the cursor |
| Backspace / Delete | Delete |
| Ctrl+A, Ctrl+C, Ctrl+V | Select all, copy, paste |

A typed number is clamped to the slider's range; a clock, position or altitude that does not parse shows an
amber notice and changes nothing. While a field has the keyboard, movement keys and Esc's quit are off, so you can
type `w` or `q` safely.

## Where in the code

- `src/simulations/SatelliteSim.cpp`: the `keybindings` table in `init()`, `dispatchKeyAction()`, `onKey()`,
  `pollGamepad()`, `padContextButton()`, ground movement in `recordCompute()`; follow mode in `startFollow()`,
  `updateFollow()`, `stopFollowHere()` and `followReturn()`.
- `src/simulations/SatelliteSim.h`: the `KB` enum.
- `src/simulations/SatelliteSimUI.cpp`: the Controls tab (`buildSettingsControlsTab()`), the follow bar
  (`buildFollowHud()`), mouse look and picking in `buildUI()`, the text fields (`textField()`, `numberField()`, `inlineTextField()`, `textEditKey()`).
- `src/App.cpp`: the Esc-to-quit handler.
- The plumbing behind all of this is on [Controls and settings plumbing](../development/controls-and-settings.md).
