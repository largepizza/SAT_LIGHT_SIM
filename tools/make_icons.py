#!/usr/bin/env python3
"""make_icons.py — regenerate the UI icon PNGs in assets/icons/ui/.

Why this exists: the icons were hand-drawn at first, then a few were rasterised ad-hoc from shell
one-liners, which meant the only copy of a glyph's geometry was a command in a terminal
scrollback. This file is that geometry, in the repo, in one place:

    python tools/make_icons.py              # write every icon + print previews
    python tools/make_icons.py spin studio  # only the named ones

Each icon is a `build_*` function returning a list of SHAPES — small predicates over a point
(px, py) in the 48x48 box, y down, origin top-left. Adding or tuning a glyph means editing that
list and re-running; the printed preview (48 px, then box-filtered to the on-screen 16 px) is the
check, so no image viewer or screenshot round trip is needed.

Icons are WHITE with an alpha channel; UIRenderer draws them untinted over whatever the button
background is, so contrast is the caller's problem (see Pal::chipIdle and friends).

No third-party imports: a few lines of zlib PNG writing keeps this runnable anywhere the project
builds. (Pillow is not a dependency of this repo and should not become one for a build asset.)
"""

import math
import struct
import sys
import zlib
from pathlib import Path

SIZE = 48          # every icon is 48x48, whatever it is drawn at (see UIRenderer::loadIcons)
SUPERSAMPLE = 3    # sub-samples per axis; the alpha is their coverage, so edges stay soft

OUT_DIR = Path(__file__).resolve().parent.parent / "assets" / "icons" / "ui"


# ─────────────────────────────────────────────────────────────────────────────
# PNG writing (RGBA8, no dependencies)
# ─────────────────────────────────────────────────────────────────────────────
def write_png(path: Path, pixels: list[list[int]]) -> None:
    """pixels[y][x] = alpha 0..255; every written pixel is white."""
    raw = bytearray()
    for row in pixels:
        raw.append(0)  # filter type 0 (None)
        for a in row:
            raw += bytes((255, 255, 255, a))  # straight (non-premultiplied) RGBA

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
           + chunk(b"IEND", b""))
    # 2026-09-25: the bytes were built and then dropped on the floor — `main` only noticed because it
    # stats the file afterwards, so running this script failed with FileNotFoundError instead of
    # quietly writing nothing.
    path.write_bytes(png)


# ─────────────────────────────────────────────────────────────────────────────
# Shapes: each returns True when (px, py) is inside it
# ─────────────────────────────────────────────────────────────────────────────
def ring(cx, cy, r, half):
    return lambda px, py: abs(math.hypot(px - cx, py - cy) - r) <= half


def arc(cx, cy, r, half, a0, a1):
    """Ring segment. Angles are degrees, y down, 0 = 3 o'clock, so +90 = 6 o'clock."""
    def hit(px, py):
        if abs(math.hypot(px - cx, py - cy) - r) > half:
            return False
        a = math.degrees(math.atan2(py - cy, px - cx)) % 360.0
        return a0 <= a <= a1

    return hit


def disc(cx, cy, r):
    return lambda px, py: math.hypot(px - cx, py - cy) <= r


def seg(x0, y0, x1, y1, half):
    dx, dy = x1 - x0, y1 - y0
    l2 = dx * dx + dy * dy

    def hit(px, py):
        t = 0.0 if l2 == 0 else max(0.0, min(1.0, ((px - x0) * dx + (py - y0) * dy) / l2))
        return math.hypot(px - (x0 + t * dx), py - (y0 + t * dy)) <= half

    return hit


def rect(x0, y0, x1, y1):
    return lambda px, py: x0 <= px <= x1 and y0 <= py <= y1


def tri(p0, p1, p2):
    def hit(px, py):
        def side(a, b):
            return (px - b[0]) * (a[1] - b[1]) - (a[0] - b[0]) * (py - b[1])
        d1 = side(p0, p1)
        d2 = side(p1, p2)
        d3 = side(p2, p0)
        neg = d1 < 0 or d2 < 0 or d3 < 0
        pos = d1 > 0 or d2 > 0 or d3 > 0
        return not (neg and pos)

    return hit


# ─────────────────────────────────────────────────────────────────────────────
# The icons (name -> shapes). Index order lives in SatelliteSimUI.cpp's kIcon* constants.
# ─────────────────────────────────────────────────────────────────────────────
def build_maximize():
    """Two diagonal arrows with a centre gap: pop the 3D view out / restore it.

    The barbs are two strokes at 90 degrees (the classic corner arrowhead) rather than the filled
    triangle `arrow()` makes — at 16 px that triangle merged with the shaft into one blob."""
    return [
        seg(22, 26, 10, 38, 2.6), seg(10, 38, 10, 30, 2.6), seg(10, 38, 18, 38, 2.6),
        seg(26, 22, 38, 10, 2.6), seg(38, 10, 38, 18, 2.6), seg(38, 10, 30, 10, 2.6),
    ]


def build_observer():
    """The view from the ground observer: horizon arc, sight line, satellite.

    There was an eye above the arc first, which read as a generic "eye" — the same idea as the Go
    to icon two buttons away — and at 16 px was a smudge under the arc. Ground -> up -> satellite
    is what the preset actually does. The star is deliberately the biggest element (r 6.5): at 16 px
    anything smaller box-filters away to a dot and the glyph loses its subject."""
    return [
        parabola(24.0, 37.5, 16.0, 5.0, 2.0),            # the horizon
        *arrow(16.5, 33.5, 28.5, 19.5, 2.0, 5.5, 3.6),   # the sight line (shaft + head)
        sparkle(36.5, 10.0, 6.5),                        # the satellite
    ]


def build_spin():
    """Rotate / idle spin: a 3/4 ring with a chunky tangential arrowhead at its open end."""
    cx, cy, r = 24.0, 24.0, 12.5
    a_head = math.radians(300.0)
    px, py = cx + r * math.cos(a_head), cy + r * math.sin(a_head)
    tx, ty = math.cos(a_head + math.pi / 2), math.sin(a_head + math.pi / 2)  # clockwise tangent
    nx, ny = math.cos(a_head), math.sin(a_head)                              # radial
    return [
        arc(cx, cy, r, 2.2, 20.0, 300.0),  # the ring, with a gap at the 1-3 o'clock wedge
        tri((px + 6.5 * tx, py + 6.5 * ty),
            (px + 4.6 * nx, py + 4.6 * ny),
            (px - 4.6 * nx, py - 4.6 * ny)),
    ]


def build_studio():
    """Studio lighting: a bulb with light lines coming off it (against the live sky).

    The diagonal rays start clear of the bulb (13.5 rather than 12.5): further in and they merge
    with the bulb's top into one flower-shaped blob at button size."""
    cx, cy = 24.0, 22.0
    shapes = [
        # Bulb: a ring open only where the neck joins, so it reads as a round bulb, not a ring.
        lambda px, py: abs(math.hypot(px - cx, py - cy) - 9.2) <= 2.5 and py <= 26.5,
        rect(20.5, 24.5, 27.5, 29.5),  # neck
        rect(19.0, 31.0, 29.0, 34.0),  # screw
        rect(21.0, 35.5, 27.0, 38.0),  # base tip
    ]
    for ang, r0, r1 in ((90.0, 13.5, 17.5), (45.0, 13.5, 18.5), (135.0, 13.5, 18.5),
                        (0.0, 12.5, 17.5), (180.0, 12.5, 17.5)):
        a = math.radians(ang)
        dx, dy = math.cos(a), math.sin(a)
        shapes.append(seg(cx + dx * r0, cy + dy * r0, cx + dx * r1, cy + dy * r1, 1.7))
    return shapes


def build_track():
    """Track: lock the camera onto the selected satellite and follow it across the sky.

    A sight-reticle — centre ring with four gapped cardinal arms — not a crosshair: the gaps are what
    keep the glyph distinct from pixel--crosshair.png (the Select icon) at button size, and the ring
    is the thing being tracked."""
    cx, cy = 24.0, 24.0
    shapes = [ring(cx, cy, 3.0, 2.2)]
    for ang in (0.0, 90.0, 180.0, 270.0):
        a = math.radians(ang)
        dx, dy = math.cos(a), math.sin(a)
        shapes.append(seg(cx + dx * 11.5, cy + dy * 11.5, cx + dx * 20.5, cy + dy * 20.5, 2.2))
    return shapes


ICONS = {
    "maximize": build_maximize,
    "observer": build_observer,
    "spin": build_spin,
    "studio": build_studio,
    "track": build_track,
}


# ─────────────────────────────────────────────────────────────────────────────
# Rasterise + preview
# ─────────────────────────────────────────────────────────────────────────────
def rasterise(shapes) -> list[list[int]]:
    step = 1.0 / SUPERSAMPLE
    total = SUPERSAMPLE * SUPERSAMPLE
    out = []
    for y in range(SIZE):
        row = []
        for x in range(SIZE):
            hits = 0
            for sy in range(SUPERSAMPLE):
                for sx in range(SUPERSAMPLE):
                    px = x + (sx + 0.5) * step
                    py = y + (sy + 0.5) * step
                    if any(s(px, py) for s in shapes):
                        hits += 1
            row.append(round(255 * hits / total))
        out.append(row)
    return out


def preview(pixels: list[list[int]]) -> str:
    """The 48 px glyph, then the box-filtered 16 px one (button size) — the actual check."""
    ramp = " .:-=+*#%@"
    lines = ["".join(ramp[min(9, pixels[y][x] * 10 // 256)] for x in range(SIZE))
             for y in range(SIZE)]
    lines += ["", "16 px (as drawn on a button):"]
    factor = SIZE // 16
    for y in range(16):
        line = ""
        for x in range(16):
            block = [pixels[y * factor + j][x * factor + i]
                     for j in range(factor) for i in range(factor)]
            line += ramp[min(9, (sum(block) // len(block)) * 10 // 256)]
        lines.append(line)
    return "\n".join(lines)


def main(argv: list[str]) -> int:
    wanted = argv[1:] or sorted(ICONS)
    for name in wanted:
        if name not in ICONS:
            print(f"unknown icon '{name}' (known: {', '.join(sorted(ICONS))})", file=sys.stderr)
            return 2
        pixels = rasterise(ICONS[name]())
        out = OUT_DIR / f"pixel--{name}.png"
        write_png(out, pixels)
        print(f"=== pixel--{name}.png ({out.stat().st_size} bytes) ===")
        print(preview(pixels))
        print()
    return 0


def arrow(x0, y0, x1, y1, shaft_half, head_len, head_half):
    """Shaft + a filled triangular head at (x1, y1)."""
    length = math.hypot(x1 - x0, y1 - y0)
    ux, uy = (x1 - x0) / length, (y1 - y0) / length
    bx, by = x1 - ux * head_len, y1 - uy * head_len   # head's base centre
    nx, ny = -uy, ux                                  # perpendicular
    return [seg(x0, y0, bx, by, shaft_half),
            tri((x1, y1), (bx + nx * head_half, by + ny * head_half),
                (bx - nx * head_half, by - ny * head_half))]


def sparkle(cx, cy, r, exponent=0.55):
    """A 4-point star: a superellipse with an exponent below 1 pinches the arms in."""
    def hit(px, py):
        u, v = abs(px - cx) / r, abs(py - cy) / r
        return u ** exponent + v ** exponent <= 1.0

    return hit


def parabola(cx, y0, w, sag, half):
    """A shallow bowl: y = y0 - sag at the middle, y0 at the ends (the horizon arc)."""
    def hit(px, py):
        if abs(px - cx) > w:
            return False
        t = (px - cx) / w
        return abs(py - (y0 - sag * (1.0 - t * t))) <= half

    return hit


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
