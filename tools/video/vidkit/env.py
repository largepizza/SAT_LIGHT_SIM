"""Paths and external tools (repo root, output folders, ffmpeg, LaTeX).

Everything generated goes under tools/video/out/ (gitignored):
    out/<project>/data/    inputs the scenes read (dumps, benchmark reports)
    out/<project>/media/   Manim's working folder (partial movies, images, TeX cache)
    out/<project>/scenes/  one mp4 per rendered scene, named <Scene>.mp4
    out/<project>/clips/   engine clips, normalised to the project's size and frame rate
    out/<project>/final/   the assembled video
"""
from __future__ import annotations

import os
import shutil
from pathlib import Path

VIDEO_ROOT = Path(__file__).resolve().parents[1]
REPO = VIDEO_ROOT.parents[1]
OUT_ROOT = VIDEO_ROOT / "out"
PROJECTS = VIDEO_ROOT / "projects"


def out_dir(project: str, *parts: str) -> Path:
    p = OUT_ROOT.joinpath(project, *parts)
    p.mkdir(parents=True, exist_ok=True)
    return p


def _tex_candidates():
    local = os.environ.get("LOCALAPPDATA", "")
    yield Path(local) / "Programs" / "MiKTeX" / "miktex" / "bin" / "x64"
    yield Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "MiKTeX" / "miktex" / "bin" / "x64"
    yield Path("/Library/TeX/texbin")
    yield Path("/usr/local/texlive/bin/x86_64-linux")


def ensure_tex_on_path() -> bool:
    """Put a LaTeX install on PATH if it is not already (MiKTeX installs per user and a fresh shell
    may not see it). Returns whether `latex` can be found."""
    if shutil.which("latex"):
        return True
    for c in _tex_candidates():
        if (c / ("latex.exe" if os.name == "nt" else "latex")).exists():
            os.environ["PATH"] = str(c) + os.pathsep + os.environ.get("PATH", "")
            return True
    return False


def ffmpeg() -> str:
    exe = shutil.which("ffmpeg")
    if not exe:
        raise SystemExit("ffmpeg not found on PATH")
    return exe


def ffprobe() -> str:
    exe = shutil.which("ffprobe")
    if not exe:
        raise SystemExit("ffprobe not found on PATH")
    return exe


def expand(s: str, project: str) -> str:
    """{repo}, {video}, {out}, {data}, {project_dir}, {python} in project.json strings."""
    import sys
    return s.format(
        python=sys.executable.replace("\\", "/"),
        repo=str(REPO).replace("\\", "/"),
        video=str(VIDEO_ROOT).replace("\\", "/"),
        out=str(out_dir(project)).replace("\\", "/"),
        data=str(out_dir(project, "data")).replace("\\", "/"),
        project_dir=str(PROJECTS / project).replace("\\", "/"),
    )
