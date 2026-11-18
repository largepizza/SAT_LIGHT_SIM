"""Placeholder event-window banners from a harness capture run.

    tools/harness/.venv/Scripts/python tools/make_event_banners.py harness_runs/event_banners [--out assets/events]

Takes each `captures/<id>.png` of a run of tools/harness/scripts/event_banners.satcmd, crops it to its centre 3:1
(the full width, a third of it high, centred), scales it to 960 x 320 and writes `<out>/<id>.png`. No text is drawn:
the window has its own title. A capture that is missing is reported and skipped (the window then draws its text
placeholder for that id). docs/subsystems/sky-events.md "Event windows".
"""
import argparse
import sys
from pathlib import Path

from PIL import Image

IDS = [
    "lunar_eclipse", "comet_inner_planets", "comet_perihelion", "comet_passes_earth", "comet_meteor_storm",
    "australasian_eclipse", "suns_fury", "first_collision", "destroyed_10", "destroyed_50",
    "betelgeuse_explodes", "betelgeuse_fades", "explode_warning",
]
W, H = 960, 320


def banner(src: Path) -> Image.Image:
    im = Image.open(src).convert("RGB")
    w, h = im.size
    cw, ch = w, w // 3
    if ch > h:  # (a frame wider than 3:1: keep the height)
        cw, ch = h * 3, h
    x0, y0 = (w - cw) // 2, (h - ch) // 2
    return im.crop((x0, y0, x0 + cw, y0 + ch)).resize((W, H), Image.LANCZOS)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("run", type=Path, help="the harness run folder (holds captures/)")
    ap.add_argument("--out", type=Path, default=Path(__file__).resolve().parent.parent / "assets" / "events")
    a = ap.parse_args()
    caps = a.run / "captures" if (a.run / "captures").is_dir() else a.run
    a.out.mkdir(parents=True, exist_ok=True)
    missing = 0
    for i in IDS:
        src = caps / f"{i}.png"
        if not src.exists():
            print(f"missing {src}")
            missing += 1
            continue
        dst = a.out / f"{i}.png"
        banner(src).save(dst, optimize=True)
        print(f"{dst}  <- {src.name}")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
