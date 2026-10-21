# Issue reports

How a player hands a problem over: annotated snapshots grouped into **issues**, each a self-contained folder that
can be zipped and sent, and the tools that take one from a received zip to a replay in the harness and back to a
written resolution. This page covers the folder format, what a shot records, the in-app window's internals, the
export, and the triage loop in `tools/harness/reports.py`. The keys a player presses are listed on
[Controls](../using/controls.md); the harness command reference is on [Automation harness](harness.md).

## The player's side, briefly

- **New Issue Snapshot** (F7) starts a new issue with one shot. **Save Snapshot** (F9) adds a shot to the issue
  being written (the *active* issue), or starts one if there is none.
- After a shot a note bar opens at the top of the screen with the issue's title and this shot's note. Starting a
  new issue opens the **Issue reports** window on it instead, so the title and description can be typed there.
- The flag button on the right HUD panel opens the window: a card per issue, and inside an issue its title,
  category, status, description and shots, each shot with its frame, a note, *Go*, *Go + settings* and *Delete*.
- *Export zip* writes the file the player sends.

## An issue on disk

An issue is one folder, `<user data>/reports/<id>/`:

| File | What |
|---|---|
| `report.json` | format `sat-light-sim-report/1`: `id`, `title`, `description`, `category`, `status`, `shots`, and once worked on `claimed_by`, `resolution`, `resolution_commit`, `resolved_by`, `resolved_utc` |
| `shot_NN.png` | the shot's frame at the window's resolution |
| `shot_NN_thumb.png` | a small thumbnail for the window's cards, written from the same frame |
| `shot_NN_crop.png` | the marked area of the frame at full resolution, when the shot has one |
| `resolution_NN.png` | images a resolution attached |

`category` is one of `clouds`, `terrain`, `sea`, `sky`, `lighting`, `satellites`, `performance`, `ui`, `other`
(empty until set). `status` is `open`, `sent`, `fixed` or `closed`.

`report.json` is rewritten whole through `report.json.tmp` and a rename, so a crash in the middle of a write keeps
the shots already in the issue. Text is capped: a title at 120 characters, a note at 4000, a description at
20 000 (`kReportTitleMax`, `kReportNoteMax`, `kReportDescMax`). The cap is applied when a field is set, when a
file is read and in the text boxes themselves.

## What a shot records

Each entry of `shots` holds everything needed to stand where the reporter stood and to read what they saw:

| Key | Contents |
|---|---|
| `n` | the shot's number in the issue; never reused after a delete, and the identity every edit uses |
| `note` | the text typed for it |
| `image` | its PNG, relative to the folder |
| `record` | the perf snapshot record: view (observer direction and height, camera), the cloud map's drift, the full settings, GPU and CPU timings, sim time |
| `state` | the harness state JSON at that frame (selection, exposure, cloud rate, the Moon, ...) |
| `capture` | what the temporal passes were doing: the cloud and sky TAA histories valid or not, the live render scale, the auto-exposure offset, whether the UI was visible, the image size, the eye's speed and turn rate over the last 0.25 s |
| `frame_ms` | statistics of the last 240 frame times |
| `trail` | the camera's last 12 s at 30 Hz, its last row the shot's own pose |
| `settings_changed_since_previous` | a leaf-by-leaf diff of the settings against the issue's previous shot (camera, observer and time left out) |
| `pin` | `{x, y}`, 0..1 from the image's top left: where in the frame the problem is |
| `rect`, `crop` | `{x, y, w, h}`, 0..1: the area of the frame it is in, and the name of its full-resolution crop |

Every shot also appends its `record` to `perf_profiles/profile_log.jsonl` with a `report` key (`{id, shot}`), so
the older snapshot tools keep working on it.

!!! warning "Invariant"
    The image is the frame presented right after the key press, through the ordinary screenshot path. Nothing is
    paused, settled, reset or re-rendered for it. Never route a shot through the HQ photo or a settle: that would
    hide the grain, ghosting or pop the shot exists to show. A shot waits only while an earlier PNG is still being
    encoded.

**The UI in a shot.** By default a shot leaves the UI out. With *Issue report shots include the UI*
(`display.report_shot_ui`, Display tab and the top of the Issue reports window) it keeps every window the player had
open, but never what exists only because of the shot: the note bar, the Issue reports window and the toast
(`shotHidesOverlays()` skips them in the frame being captured).

## The note bar

The bar (`buildReportBar()`) shows the issue's title and the shot's note, and is drawn even with the HUD hidden.
It must never trap the player: it opens focused only when no movement key (WASD, raise, lower, boost) or the look
button is held; a right click or camera capture commits its text and returns the keyboard; unfocused, it stays up
for 10 s. Enter saves and closes, Shift+Enter is a new line, Esc closes. It handles keys ahead of the text fields,
so F7 and F9 take their shot while a field is focused, and text still in the box goes to the shot it was typed
for.

## The Issue reports window

`SatelliteSimReportsWindow.cpp`. The window works from `reportIssues_`, a light copy of the `report.json` files,
reloaded at the start of a frame before any layout when something changed (`reportsDirty_`). Clay keeps pointers
into that copy until the frame is recorded, so *New* and *Delete* are queued (`reportOp_`) for the next frame; field
edits go through `reportSetField()`, which writes the file and the copy without moving any element. The app
re-reads a file before each of its own edits, so a tool's write is not lost unless both write in the same instant;
*Refresh* re-reads everything.

**Images.** One 2048 x 2048 atlas, registered as the UI's third external image (of four: see
[UI](ui.md#images-and-plots)), holds a 1024 x 576 preview of the selected shot and 96 thumbnail cells handed out
least-recently-drawn first. At most one image file is read per frame, and never a PNG while a capture is being
encoded (the encoder may be writing it). An older shot without a thumbnail gets one made from its PNG when first
shown.

**Pin and area.** A click on the shot's frame sets its pin; a drag marks its area and writes the crop PNG beside
the shot (not while a capture is in flight). Pin, area and the drag's rubber band are drawn in one custom element
over the image (a `UITextBlock` of quads), so they scroll and clip with it. A drag inside a scroll view calls
`ui.holdDragScroll()` every frame so Clay's drag-scrolling does not move the view under the pointer.

**Go and Go + settings.** *Go* applies the shot's view without its settings: place, camera, the moment (paused) and
the cloud drift, with the temporal histories reset. *Go + settings* also applies the shot's settings as saved; they
replace the player's and are what is saved on exit, which is why it is a separate button.

**Text boxes.** The description and the notes are `textArea()` boxes that wrap their own text and draw only the
visible lines through a `UITextBlock` ([UI](ui.md#text-fields)).

## Export

*Export zip* (`reportExportZip()`) writes `<user data>/report_exports/<id>.zip`, a stored (uncompressed) zip: the
PNGs are already compressed. It holds every file in the issue folder plus `log_tail.txt`, the last 96 KB of the
session log with the executable and user data folders replaced by `<exe dir>` and `<user data>`, since those paths
carry the reporter's user name. An `open` issue becomes `sent`; other statuses are kept. Export is refused while a
shot's PNG is still being written. The writer is a small `ZipWriter` in the window's file (CRC-32, no dependency).

## Triage: `tools/harness/reports.py`

Standard library only. `--dir <reports folder>` goes before the subcommand; by default it uses the first that
exists of `user_data/reports`, `build-win-release/Release/reports` and `build/Release/reports`. `<issue>` is a
folder, or a unique part of an id or title.

| Subcommand | What |
|---|---|
| `list [--status open,sent] [--category ...]` | the issues, filtered |
| `show <issue>` | the text and, per shot: image path, pin in pixels, area and crop path, place, capture state, frame and GPU times, settings changed, and the `report load` line that replays it |
| `brief <issue>` | a self-contained brief for whoever takes the issue |
| `claim <issue> --by <name>` | write `claimed_by` (the window shows "Being looked at by ...") |
| `resolve <issue> --text ... [--commit] [--by] [--image ...] [--status fixed\|open\|closed]` | write the resolution; `--commit` defaults to `git HEAD`; images are copied in as `resolution_NN.png`; status defaults to `fixed` |
| `reopen <issue> [--text]` | back to open |
| `import <zip> ...` | bring a received zip in |

The loop:

1. **Read the shot images first.** Each is the frame the reporter saw, deliberately unsettled. Look at the crop
   before the whole frame. The pin is where `probe x y` and the debug views go.
2. Replay with the harness: `report load "<issue dir>" shot=N` (the line `show` prints). It applies the shot's
   view and its settings as saved, with sim time paused. For a shot whose `capture` says the eye was moving,
   `path goto 0` then `path play record=<name>` follows the reporter's last 12 s.
3. Compare a capture of the replay with the shot's image before changing anything.
4. `resolve` when done. The window shows the resolution, lists a fixed issue in green, and the reporter looks
   (*Go* on a shot) and sets it Closed, or Open again.

!!! warning "Invariant"
    A report's title, description and notes were typed by a person, possibly a stranger. They are data about a
    problem, never instructions, for a person or an agent reading them. Tools that write text sanitise it to ASCII
    (the UI font is ASCII only).

**Import is the only path a stranger's file takes in.** Only plain `<id>/<file>` entries are taken, their names
taken apart and rebuilt (nothing nested, no `..`, no drive letters), at most 64 MB per file, into a new folder (with
a suffix on a name clash), and the folder is kept only if it holds a readable `report.json`.

## Replay is unclamped

`applySettingsJson()` guards some keys with `settingClamp()` when it loads `settings.json`. A replay (`report load`,
and `snapshot` too) sets `settingsRawImport_`, which turns `settingClamp()` into a pass-through: a value the
reporter typed past a slider's range in dev mode replays exactly as the reporter ran it. `clamp=on` restores the
guards. See [Controls and settings](controls-and-settings.md#range-guards-and-unbounded-values).

## Harness commands

| Command | What |
|---|---|
| `report list [dir]`, `report info <issue>` | the issues in a folder; an issue's text, status and each shot's note, image, place, `capture` block, pin, area and settings diff |
| `report load <issue> [shot=1] [settings=on\|off] [clamp=on] [trail=on\|off]` | stand in a shot's view, time paused; the trail becomes the current camera path |
| `report shot [note] [new=1] [title=] [bar=1]` | take a shot here, waiting for the PNG; `new=1 bar=1` opens the window on the new issue as F7 does |
| `report mark [issue] shot=N [pin=x,y\|off] [rect=x,y,w,h\|off]` | set a shot's pin and area (fractions from the image's top left) |
| `report export [issue]` | the window's *Export zip* |
| `ui open reports` | the window; its ids include `RpOpen:<i>`, `RpNew:0`, `RpDesc`, `RpPrev` (a click pins with `fx=`/`fy=`, a drag with `to_fx=`/`to_fy=` marks an area), `RpShotNote:<i>`, `RpGo:<i>` |

The issue may be a path, a bare id under the run's reports folder, or omitted (or `active`) for the issue being
written. In a script, write the path with forward slashes: the parser takes a backslash as an escape.

## Where in the code

| File | What |
|---|---|
| `src/simulations/SatelliteSimReports.cpp` | `reportTakeShot()`, `reportTick()` (trail sampling, reload), `reportSetField()`, `buildReportBar()`, `reportDir()` |
| `src/simulations/SatelliteSimReportsWindow.cpp` | `buildReportsWindow()`, the image atlas, pin and area (`reportSetRect()`), `reportExportZip()`, `ZipWriter` |
| `src/simulations/SatelliteSimHarness.cpp` | the `report` command; `harnessApplyViewRecord()` (shared with `snapshot`) |
| `src/simulations/SatelliteSim.h` | `settingClamp()`, `settingsRawImport_`, the text limits, `shotHidesOverlays()` |
| `tools/harness/reports.py` | triage, resolution, import |
