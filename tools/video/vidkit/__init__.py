"""vidkit: a small toolkit for technical videos about this project.

Manim scenes for the explanation (driven by real data the app's tools export), engine footage from the
automation harness, and ffmpeg for the edit. One folder per video under tools/video/projects/.
"""
from __future__ import annotations

import os
from pathlib import Path

from . import env


def project_name() -> str:
    """The project being rendered (set by `python -m vidkit render`)."""
    return os.environ.get("VIDKIT_PROJECT", "")


def project_data() -> Path:
    """out/<project>/data — where the project's `data` steps write."""
    return env.out_dir(project_name(), "data")


env.ensure_tex_on_path()
