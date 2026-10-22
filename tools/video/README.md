# tools/video — technical videos (vidkit)

Explainer videos about the simulator, built from three parts:

- **Manim scenes** for the explanation. Each is drawn from real data that the app's own tools export, so
  re-running the data steps after a model change updates the numbers on screen.
- **Engine clips**: existing cinematic exports, or new ones recorded through the automation harness
  (`tools/harness/run.py`, docs/HARNESS.md).
- **ffmpeg** for the edit: clips normalised to one size and frame rate, captions burned in, crossfades, and
  music underneath.

One folder per video under `projects/`. Everything generated goes to `out/` (gitignored).

## Setup (once)

```powershell
powershell -ExecutionPolicy Bypass -File tools\video\setup.ps1
```

This creates `tools/video/.venv` on Python 3.10-3.12 (Manim has no wheels for 3.14 yet), installs
`requirements.txt`, and installs MiKTeX (LaTeX for `MathTex`, fetching packages on first use) and ffmpeg if
they are missing.

## Use

From `tools/video`:

```powershell
.venv\Scripts\python -m vidkit list                              # projects
.venv\Scripts\python -m vidkit data     sat_lighting             # run the data steps (skips up-to-date outputs; --force)
.venv\Scripts\python -m vidkit render   sat_lighting [Scene ...] # Manim, preview quality (960x540 @ 15)
.venv\Scripts\python -m vidkit assemble sat_lighting             # -> out/sat_lighting/final/sat_lighting_preview.mp4
.venv\Scripts\python -m vidkit all      sat_lighting --quality final   # data + every scene at full size + the edit
.venv\Scripts\python -m vidkit clips    sat_lighting [clip ...]  # record "harness" clips (launches the app)
```

Preview renders take seconds to a minute per scene; `--quality final` renders at the project's size and
frame rate, and takes much longer. To check one frame while editing, run Manim directly with `-s`.

## A project (`projects/<name>/`)

| File | |
|---|---|
| `project.json` | size, fps, data steps, clips, the timeline (see below) |
| `scenes.py` | Manim scenes, one class per timeline `scene` entry |
| `clips/*.satcmd` | harness scripts for clips recorded in the engine |

Start a new one by copying `projects/_template`.

`project.json`:

- **`data`**: commands run from the repo root, skipped while all their `outputs` exist. Strings can use `{repo}`,
  `{data}` (= `out/<project>/data`), `{out}`, `{video}` and `{project_dir}`. Set `allow_fail` on a step whose
  non-zero exit is expected; SatModelTool's benchmark runs return 1 when an ungated benchmark misses.
- **`clips`**: `{"file": path}` (an existing video) or
  `{"harness": "clips/x.satcmd", "args": [...], "frames": "captures/cine_<name>"}`. A harness clip is recorded by
  `vidkit clips`: run.py runs the script, then `tools/harness/frames2video.py` turns the frames into
  `out/<project>/clips/<name>.raw.mp4`. Launch rules apply: the harness spaces launches 30 s apart.
  Use one script per clip and never relaunch back to back.
- **`timeline`**: in order, `{"scene": "Name"}` or `{"clip": "name", "start": s, "end": s, "speed": 1, "caption": "..."}`.
  Add `"skip": true` to leave an entry out.
- `xfade` (crossfade in seconds), `music` (a path, laid under the whole video, faded) and `music_gain_db`.

## The library (`vidkit/`)

| Module | |
|---|---|
| `theme.py` | palette, fonts, `ThemedScene` (background, section label, `say()` captions), `stat`, `card`, `counter` |
| `geom.py` | `Mesh.draw()` (flat-shaded triangles as 2D shapes: fast, use with `always_redraw`), `SphereView` (the sphere of directions: outline, dots, hemisphere heatmap), `heat_image` (equirectangular heatmap) |
| `plots.py` | `ChartFrame`: axes along the box's edges, log scales, inverted y (magnitudes) |
| `data.py` | `LobeDump` (SatModelTool `--dump-json`), `BenchRun` (`--run-benchmark` reports) |
| `edit.py` | `normalize` (trim, speed, letterbox, captions), `join` (crossfades + music) |
| `cli.py` | the commands above |

Restyle every video by editing `theme.py`.

## Data the app exports for videos

- `SatModelTool <model.json>... --dump-json <file>` writes, per model (`MODEL` in the path is replaced by its
  id): the rest-pose triangles; the lobes at the exact merge and at budgets 256 / 128 / 64 / 48 / 24 / 12 / 6,
  with each lobe's member triangles; 4000 random (Sun, observer) pairs scored by brute force and by every
  level; and an intensity sphere per level (`tools/sat_model_tool/dump_json.cpp`).
- `SatModelTool --run-benchmark <file>` reports carry the published observations, every simulated sample,
  the phase curve and the comparison metrics.
- Engine footage: `cine export preview|hq` (docs/HARNESS.md "Cinematics"). The b-roll cinematics in
  `tools/harness/scripts/broll_*.satcmd` are already exported under `screenshots/cinematics/`.
