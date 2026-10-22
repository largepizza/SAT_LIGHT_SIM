"""Command line: build a project's data, render its scenes, record / normalise clips, assemble the video.

    python -m vidkit list
    python -m vidkit data     <project> [--force]
    python -m vidkit render   <project> [Scene ...] [--quality preview|final]
    python -m vidkit clips    <project> [clip ...]          (records `harness` clips; `file` clips need nothing)
    python -m vidkit assemble <project> [--quality preview|final]
    python -m vidkit all      <project> [--quality preview|final]

Run from tools/video with the venv's Python (setup.ps1). See README.md for project.json.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

from . import edit, env

QUALITY = {  # (width, height, fps) for Manim; assembly uses the project's own size
    "preview": (960, 540, 15),
    "final": None,  # the project's width / height / fps
}


def load_project(name: str) -> dict:
    p = env.PROJECTS / name / "project.json"
    if not p.exists():
        raise SystemExit(f"no project '{name}' ({p})")
    with open(p, encoding="utf-8") as f:
        cfg = json.load(f)
    cfg.setdefault("width", 1920)
    cfg.setdefault("height", 1080)
    cfg.setdefault("fps", 30)
    cfg.setdefault("scenes_file", "scenes.py")
    cfg.setdefault("xfade", 0.5)
    cfg["_name"] = name
    cfg["_dir"] = env.PROJECTS / name
    return cfg


def scene_env(cfg) -> dict:
    e = dict(os.environ)
    e["PYTHONPATH"] = str(env.VIDEO_ROOT) + os.pathsep + e.get("PYTHONPATH", "")
    e["VIDKIT_PROJECT"] = cfg["_name"]
    env.ensure_tex_on_path()
    e["PATH"] = os.environ["PATH"]
    return e


def cmd_data(cfg, force=False):
    for step in cfg.get("data", []):
        outs = [Path(env.expand(o, cfg["_name"])) for o in step.get("outputs", [])]
        if outs and all(o.exists() for o in outs) and not force:
            print(f"[data] {step['name']}: up to date")
            continue
        print(f"[data] {step['name']}")
        cmd = [env.expand(c, cfg["_name"]) for c in step["cmd"]]
        if cmd[0].endswith(".py"):  # a project script: run it with this venv's Python
            cmd = [sys.executable] + cmd
        exe = Path(cmd[0])
        if exe.suffix == "" and not exe.exists() and Path(cmd[0] + ".exe").exists():
            cmd[0] += ".exe"
        r = subprocess.run(cmd, cwd=str(env.REPO), capture_output=True, text=True)
        tail = "\n".join((r.stdout or "").splitlines()[-4:])
        print("   ", tail.replace("\n", "\n    "))
        if r.returncode != 0 and not step.get("allow_fail"):
            print(r.stderr)
            raise SystemExit(f"[data] {step['name']} failed ({r.returncode})")


def scene_names(cfg):
    return [t["scene"] for t in cfg.get("timeline", []) if "scene" in t]


def cmd_render(cfg, scenes, quality):
    w, h, fps = QUALITY[quality] or (cfg["width"], cfg["height"], cfg["fps"])
    media = env.out_dir(cfg["_name"], "media")
    dest = env.out_dir(cfg["_name"], "scenes", quality)
    src = cfg["_dir"] / cfg["scenes_file"]
    failed = []
    for s in scenes or scene_names(cfg):
        print(f"[render] {s} ({w}x{h} @ {fps})")
        # --disable_caching: Manim hashes the whole scene (image pixel arrays included) before every animation, which
        # stalled scenes that redraw images every frame for hours.
        cmd = [sys.executable, "-m", "manim", "render", "--progress_bar", "none", "-v", "WARNING", "--disable_caching",
               "-r", f"{w},{h}", "--fps", str(fps), "--media_dir", str(media), "-o", s, str(src), s]
        if subprocess.run(cmd, env=scene_env(cfg), cwd=str(cfg["_dir"])).returncode != 0:
            failed.append(s)
            continue
        hits = sorted((media / "videos" / src.stem).glob(f"*/{s}.mp4"), key=lambda p: p.stat().st_mtime)
        if not hits:
            raise SystemExit(f"[render] no output for {s}")
        shutil.copy2(hits[-1], dest / f"{s}.mp4")
        print(f"   -> {dest / (s + '.mp4')}")
    if failed:
        raise SystemExit(f"[render] failed: {', '.join(failed)}")


def clip_source(cfg, name) -> Path:
    c = cfg["clips"][name]
    if "file" in c:
        return Path(env.expand(c["file"], cfg["_name"]))
    return env.out_dir(cfg["_name"], "clips") / f"{name}.raw.mp4"


def cmd_clips(cfg, names):
    """Record harness clips: run the script, then turn its frames into <name>.raw.mp4."""
    for name in names or list(cfg.get("clips", {})):
        c = cfg["clips"][name]
        if "harness" not in c:
            src = clip_source(cfg, name)
            print(f"[clips] {name}: file {'ok' if src.exists() else 'MISSING'} ({src})")
            continue
        run_dir = env.out_dir(cfg["_name"], "clips", name)
        script = cfg["_dir"] / env.expand(c["harness"], cfg["_name"])  # absolute paths stay absolute
        cmd = [sys.executable, str(env.REPO / "tools" / "harness" / "run.py"), str(script), "--out", str(run_dir),
               *[env.expand(a, cfg["_name"]) for a in c.get("args", [])]]
        print(f"[clips] {name}: recording {script.name} (the harness spaces launches 30 s apart)")
        run_env = dict(os.environ)
        run_env.update({k: env.expand(v, cfg["_name"]) for k, v in c.get("env", {}).items()})
        subprocess.run(cmd, check=True, cwd=str(env.REPO), env=run_env)
        frames = run_dir / c.get("frames", f"captures/cine_{name}")
        out = clip_source(cfg, name)
        f2v = env.REPO / "tools" / "harness" / "frames2video.py"
        arg = str(frames) if frames.is_dir() else str(frames)
        subprocess.run([sys.executable, str(f2v), arg, "-o", str(out), "--fps", str(c.get("fps", cfg["fps"]))],
                       check=True)
        print(f"   -> {out}")


def cmd_assemble(cfg, quality):
    w, h, fps = cfg["width"], cfg["height"], cfg["fps"]
    if quality == "preview":
        w, h, fps = QUALITY["preview"]
    seg_dir = env.out_dir(cfg["_name"], "segments", quality)
    for old in list(seg_dir.glob("*.mp4")) + list(seg_dir.glob("*.png")):  # segments of an older timeline
        old.unlink()
    scenes = env.out_dir(cfg["_name"], "scenes", quality)
    segs = []
    for i, t in enumerate(cfg.get("timeline", [])):
        if t.get("skip"):
            continue
        if "scene" in t:
            src = scenes / f"{t['scene']}.mp4"
            if not src.exists():
                raise SystemExit(f"[assemble] {src} missing: render it first")
            label = t["scene"]
        else:
            src = clip_source(cfg, t["clip"])
            if not src.exists():
                raise SystemExit(f"[assemble] clip {t['clip']} missing ({src})")
            label = t["clip"]
        dst = seg_dir / f"{i:02d}_{label}.mp4"
        print(f"[assemble] {dst.name}")
        edit.normalize(src, dst, w, h, fps, t.get("start"), t.get("end"), t.get("caption"), t.get("speed", 1.0),
                       mark=t.get("mark"))
        segs.append(dst)
    music = cfg.get("music")
    music = env.expand(music, cfg["_name"]) if music else None
    out = env.out_dir(cfg["_name"], "final") / f"{cfg['_name']}_{quality}.mp4"
    total = edit.join(segs, out, cfg["xfade"], music, cfg.get("music_gain_db", -6.0))
    print(f"[assemble] {out}  ({total:.1f} s, {len(segs)} segments)")


def main(argv=None):
    sys.stdout.reconfigure(line_buffering=True)  # progress shows up in a redirected log as it happens
    ap = argparse.ArgumentParser(prog="vidkit", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["list", "data", "render", "clips", "assemble", "all"])
    ap.add_argument("project", nargs="?")
    ap.add_argument("items", nargs="*")
    ap.add_argument("--quality", choices=list(QUALITY), default="preview")
    ap.add_argument("--force", action="store_true")
    a = ap.parse_args(argv)
    if a.command == "list":
        for p in sorted(env.PROJECTS.glob("*/project.json")):
            cfg = load_project(p.parent.name)
            print(f"{p.parent.name:24s} {cfg.get('title', '')}")
        return
    if not a.project:
        ap.error("project required")
    cfg = load_project(a.project)
    if a.command in ("data", "all"):
        cmd_data(cfg, a.force)
    if a.command in ("render", "all"):
        cmd_render(cfg, a.items, a.quality)
    if a.command == "clips":
        cmd_clips(cfg, a.items)
    if a.command in ("assemble", "all"):
        cmd_assemble(cfg, a.quality)
