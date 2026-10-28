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


def cut(fill, *holes):
    """`fill` with `holes` punched out (transparent): detail on a solid silhouette."""
    return lambda px, py: fill(px, py) and not any(h(px, py) for h in holes)


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
    """Pop the 3D view out / restore it: four solid corner brackets (the "expand" glyph).

    Filled since 2026-10-03, like the gear and the camera: the first cut was two thin diagonal
    arrows, which read as a different icon family from everything else in the HUD."""
    t, leg = 5.0, 14.0
    shapes = []
    for (cx, sx) in ((5.0, 1.0), (43.0, -1.0)):
        for (cy, sy) in ((5.0, 1.0), (43.0, -1.0)):
            xa, xb = sorted((cx, cx + sx * leg))
            ya, yb = sorted((cy, cy + sy * t))
            shapes.append(rect(xa, ya, xb, yb))                          # horizontal leg
            xa, xb = sorted((cx, cx + sx * t))
            ya, yb = sorted((cy, cy + sy * leg))
            shapes.append(rect(xa, ya, xb, yb))                          # vertical leg
    return shapes


def build_observer():
    """The view from the ground observer: a telescope on its tripod, aimed up the sky.

    The code's "observer" is the parked ground telescope, so that is the glyph (2026-10-03). The
    first two cuts (an eye over a horizon arc, then a hill with a sight arrow) read as the Go to icon
    or as nothing at button size."""
    return [
        seg(10.0, 30.0, 30.0, 17.0, 4.0),        # the tube
        seg(27.0, 19.0, 33.0, 15.0, 6.0),        # its wider objective end
        seg(21.0, 26.0, 13.0, 43.0, 2.0),        # tripod legs
        seg(21.0, 26.0, 29.0, 43.0, 2.0),
        seg(21.0, 26.0, 21.0, 43.0, 2.0),
    ]


def build_spin():
    """Rotate / idle spin: a thick 3/4 ring with a big tangential arrowhead at its open end."""
    cx, cy, r = 24.0, 25.0, 12.5
    a_head = math.radians(290.0)
    px, py = cx + r * math.cos(a_head), cy + r * math.sin(a_head)
    tx, ty = math.cos(a_head + math.pi / 2), math.sin(a_head + math.pi / 2)  # clockwise tangent
    nx, ny = math.cos(a_head), math.sin(a_head)                              # radial
    return [
        arc(cx, cy, r, 3.5, 35.0, 292.0),     # the ring, open at the 1-2 o'clock wedge
        tri((px + 12.0 * tx, py + 12.0 * ty),
            (px + 9.5 * nx, py + 9.5 * ny),
            (px - 9.5 * nx, py - 9.5 * ny)),
    ]


def build_studio():
    """Studio lighting: a solid bulb with its screw base, and light rays over it (against the live sky)."""
    cx, cy = 24.0, 22.0
    shapes = [
        disc(cx, cy, 10.5),
        tri((15.0, 27.0), (33.0, 27.0), (24.0, 36.0)),   # the bulb's taper into the neck
        rect(18.0, 30.0, 30.0, 33.0),                    # screw bands, a gap between them
        rect(18.0, 35.0, 30.0, 38.0),
        rect(21.0, 39.0, 27.0, 42.0),                    # base tip
    ]
    for ang in (180.0, 225.0, 270.0, 315.0, 0.0):        # y down: 270 = straight up
        a = math.radians(ang)
        dx, dy = math.cos(a), math.sin(a)
        shapes.append(seg(cx + dx * 14.5, cy + dy * 14.5, cx + dx * 20.0, cy + dy * 20.0, 2.2))
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


def build_photo():
    """HQ photo: a solid camera (the screenshot icon's family: lens punched out, a dot inside) with a
    sparkle at its top right — the "quality" mark that keeps it apart from camera-solid.png beside it."""
    body = rect(4.0, 17.0, 36.0, 41.0)
    hump = rect(10.0, 12.0, 22.0, 17.0)
    lens_hole = disc(20.0, 29.0, 8.0)
    return [
        cut(lambda px, py: body(px, py) or hump(px, py), lens_hole),
        disc(20.0, 29.0, 4.5),                 # the lens
        sparkle(40.0, 9.5, 8.5, 0.8),         # the "HQ" sparkle (0.8: fat enough for the pixel grid)
    ]


def build_film():
    """Cinematics: a clapperboard — a solid slate with a play triangle punched out, under a striped
    clapper arm — the camera-path editor's button (review 17; filled since 2026-10-03)."""
    def arm(px, py):
        if not (6.0 <= px <= 42.0 and 9.0 <= py <= 16.0):
            return False
        return (px + (py - 9.0)) % 10.0 >= 4.0   # slanted stripes cut through it
    slate = rect(6.0, 19.0, 42.0, 41.0)
    play = tri((19.0, 24.0), (19.0, 36.0), (31.0, 30.0))
    return [arm, cut(slate, play)]


def build_record():
    """Record video: a solid video camera (body + the lens hood flaring to the right) with a record dot punched
    out of the body — the HUD's video button (2026-10-25)."""
    body = rect(4.0, 13.0, 31.0, 35.0)
    hood = tri((31.0, 24.0), (44.0, 14.0), (44.0, 34.0))
    dot = disc(17.5, 24.0, 6.0)
    return [cut(lambda px, py: body(px, py) or hood(px, py), dot)]


def build_bookmark():
    """Bookmarks: a solid ribbon bookmark (notched tail) with a star punched out of it — a saved place
    at a moment, the Bookmarks window (2026-10-03)."""
    def ribbon(px, py):
        x0, x1, top, bot, notch = 11.0, 37.0, 5.0, 43.0, 32.0
        if not (x0 <= px <= x1 and top <= py <= bot):
            return False
        half = (x1 - x0) * 0.5
        cutline = bot - (bot - notch) * (1.0 - abs(px - 24.0) / half)  # the V notch in the tail
        return py <= cutline
    return [cut(ribbon, sparkle(24.0, 18.0, 8.0, 0.8))]


def build_report():
    """Issue reports: a solid flag on its pole with an exclamation mark punched out of it — "something
    is wrong here", the Issue reports window (2026-10-12)."""
    pole = rect(8.0, 5.0, 13.0, 44.0)
    banner = rect(13.0, 7.0, 42.0, 29.0)
    bang = lambda px, py: (25.0 <= px <= 30.0) and (10.0 <= py <= 19.0 or 22.0 <= py <= 26.0)
    return [pole, cut(banner, bang)]


def build_missions():
    """Missions: a solid rocket (nose, body, two fins, a flame under it) with a round window punched out —
    the Missions window: launches, crew flights, the Sun's Fury (2026-10-19)."""
    body = rect(18.5, 13.0, 29.5, 35.0)
    nose = tri((18.5, 13.5), (29.5, 13.5), (24.0, 2.0))
    window = disc(24.0, 20.0, 3.4)
    fin_l = tri((18.5, 24.0), (18.5, 36.0), (10.0, 39.0))
    fin_r = tri((29.5, 24.0), (29.5, 36.0), (38.0, 39.0))
    flame = tri((20.0, 38.0), (28.0, 38.0), (24.0, 47.0))
    return [cut(lambda px, py: body(px, py) or nose(px, py), window), fin_l, fin_r, flame]


def build_weather():
    """Weather: a solid storm cloud (three puffs on a flat base), a lightning bolt under it and two rain streaks
    beside the bolt — the Weather window: the radar, the storms (2026-10-23)."""
    puffs = [disc(15.0, 19.0, 8.0), disc(26.0, 13.0, 10.0), disc(36.0, 20.0, 7.5)]
    base = rect(7.0, 19.0, 43.0, 28.0)
    cloud = lambda px, py: (any(d(px, py) for d in puffs) or base(px, py)) and py <= 28.0
    bolt = [tri((22.0, 30.0), (31.0, 30.0), (19.0, 40.0)), tri((19.0, 38.0), (29.0, 38.0), (21.0, 47.0)),
            rect(19.0, 37.0, 27.0, 40.0)]
    rain = [seg(12.5, 32.0, 9.5, 42.0, 2.0), seg(37.5, 32.0, 34.5, 42.0, 2.0)]
    return [cloud] + bolt + rain


def build_storm_tornado():
    """The Weather window's tornado events: a funnel of stacked bars narrowing to the ground, leaning (2026-10-23)."""
    bars = [(6.0, 40.0, 0.0), (13.0, 31.0, 2.5), (20.0, 23.0, 4.5), (27.0, 16.0, 5.5), (34.0, 10.0, 4.5), (41.0, 6.0, 2.0)]
    return [rect(24.0 + off - w / 2.0, y, 24.0 + off + w / 2.0, y + 4.5) for (y, w, off) in bars]


def build_storm_hurricane():
    """Hurricanes / typhoons: the weather map's tropical-cyclone symbol, a ring with two spiral arms (2026-10-23)."""
    cx, cy = 24.0, 24.0
    shapes = [ring(cx, cy, 7.0, 3.6)]
    for a0 in (math.radians(-90.0), math.radians(90.0)):
        pts = []
        for i in range(13):
            th = i / 12.0 * math.pi * 0.95
            r = 8.0 + 13.0 * th / math.pi
            pts.append((cx + r * math.cos(a0 + th), cy + r * math.sin(a0 + th), 4.0 - 2.2 * i / 12.0))
        for (p, q) in zip(pts, pts[1:]):
            shapes.append(seg(p[0], p[1], q[0], q[1], p[2]))
    return shapes


def build_storm_haboob():
    """Haboobs: a billowing dust wall rolling in, with wind streaks ahead of it (2026-10-23)."""
    puffs = [disc(13.0, 28.0, 10.0), disc(22.0, 17.0, 10.0), disc(30.0, 27.0, 9.0), disc(12.0, 13.0, 7.0), disc(31.0, 12.0, 6.0)]
    base = rect(3.0, 28.0, 39.0, 42.0)
    wall = lambda px, py: (any(d(px, py) for d in puffs) or base(px, py)) and py <= 42.0
    return [wall, seg(38.0, 33.0, 46.0, 33.0, 1.8), seg(41.0, 21.0, 46.0, 21.0, 1.8), seg(40.0, 40.0, 46.0, 40.0, 1.8)]


def build_storm_supercell():
    """Supercells: an anvil cloud — a flat spreading top over a tower and a wide base — with a bolt under it (2026-10-23)."""
    anvil = tri((3.0, 5.0), (45.0, 5.0), (24.0, 22.0))
    lid = rect(3.0, 5.0, 45.0, 10.0)
    tower = rect(17.0, 10.0, 31.0, 31.0)
    base = rect(9.0, 26.0, 39.0, 33.0)
    bolt = [tri((24.0, 33.0), (31.0, 33.0), (21.0, 41.0)), tri((21.0, 39.0), (29.0, 39.0), (22.0, 47.0)), rect(21.0, 38.0, 27.0, 41.0)]
    return [anvil, lid, tower, base] + bolt


def build_storm_squall():
    """Squall lines / MCSs: a long shelf cloud, its sloping leading edge to the right, rain shafts slanting under it (2026-10-23)."""
    shelf = tri((2.0, 12.0), (46.0, 12.0), (2.0, 26.0))
    top = rect(2.0, 6.0, 40.0, 13.0)
    rain = [seg(8.0 + 10.0 * k, 28.0, 4.0 + 10.0 * k, 43.0, 2.4) for k in range(3)]
    return [shelf, top] + rain


def build_storm_waterspout():
    """Waterspouts: a thin funnel from a cloud base down to a spray ring on the waves (2026-10-23)."""
    cloud = rect(3.0, 4.0, 45.0, 11.0)
    funnel = tri((19.0, 11.0), (31.0, 11.0), (24.0, 37.0))
    neck = rect(22.0, 30.0, 26.0, 38.0)
    spray = tri((16.0, 41.0), (32.0, 41.0), (24.0, 35.0))
    waves = [parabola(12.0, 46.0, 9.0, -3.0, 1.6), parabola(36.0, 46.0, 9.0, -3.0, 1.6)]
    return [cloud, funnel, neck, spray] + waves


def build_storm_fire():
    """Wildfires: a flame (a teardrop with its tongue to one side) with an inner flame punched out (2026-10-23)."""
    def drop(cx, cy, r, tip_y, lean):
        def hit(px, py):
            if math.hypot(px - cx, py - cy) <= r:
                return True
            if py > cy or py < tip_y:
                return False
            t = (cy - py) / (cy - tip_y)
            x0 = cx + lean * t * t
            return abs(px - x0) <= r * (1.0 - t) ** 1.2
        return hit
    outer = drop(24.0, 32.0, 13.0, 2.0, 6.0)
    inner = drop(24.0, 37.0, 4.5, 26.0, -2.0)
    return [cut(outer, inner)]


def build_storm_cell():
    """Ordinary storms (air-mass cells, multicells): a cumulus cloud — three puffs on a flat base (2026-10-23)."""
    puffs = [disc(15.0, 27.0, 9.0), disc(26.0, 19.0, 12.0), disc(36.0, 28.0, 8.5)]
    base = rect(6.0, 27.0, 44.0, 37.0)
    return [lambda px, py: (any(d(px, py) for d in puffs) or base(px, py)) and py <= 37.0]


def build_sky_events():
    """Sky events (the celestial events window, 2026-10-27): a solid crescent Moon with a four-point star beside it."""
    moon = cut(disc(20.0, 26.0, 16.0), disc(29.0, 20.0, 14.0))
    star = [tri((38.0, 4.0), (35.5, 13.0), (40.5, 13.0)), tri((38.0, 22.0), (35.5, 13.0), (40.5, 13.0)),
            tri((29.0, 13.0), (38.0, 10.5), (38.0, 15.5)), tri((47.0, 13.0), (38.0, 10.5), (38.0, 15.5))]
    return [moon] + star


def build_sky_solar():
    """A total solar eclipse: the Moon's black disc punched out of the Sun, with corona rays round it."""
    corona = cut(disc(24.0, 24.0, 15.0), disc(24.0, 24.0, 10.0))
    rays = []
    for k in range(8):
        a = math.radians(22.5 + 45.0 * k)
        rays.append(seg(24.0 + 17.0 * math.cos(a), 24.0 + 17.0 * math.sin(a),
                        24.0 + 22.5 * math.cos(a), 24.0 + 22.5 * math.sin(a), 2.2))
    return [corona] + rays


def build_sky_lunar():
    """A lunar eclipse: the full Moon with the Earth's curved shadow biting into it (the shadow's edge a gap)."""
    moon = disc(24.0, 24.0, 18.0)
    return [cut(moon, ring(42.0, 30.0, 20.0, 2.2), disc(42.0, 30.0, 18.0)), cut(disc(42.0, 30.0, 17.0), lambda x, y: not moon(x, y))]


def build_sky_conjunction():
    """A conjunction: two planets close together, a large one and a small ringed one."""
    big = disc(17.0, 28.0, 11.0)
    small = disc(35.0, 15.0, 6.0)
    rng = cut(seg(25.0, 19.0, 45.0, 11.0, 1.8), disc(35.0, 15.0, 7.5))
    return [big, small, rng]


def build_sky_occultation():
    """A lunar occultation: the Moon's disc with a small planet just emerging at its limb."""
    moon = cut(disc(21.0, 25.0, 17.0), disc(39.0, 15.0, 8.0))
    planet = disc(39.0, 15.0, 5.0)
    return [moon, planet]


ICONS = {
    "storm-tornado": build_storm_tornado,
    "storm-hurricane": build_storm_hurricane,
    "storm-haboob": build_storm_haboob,
    "storm-supercell": build_storm_supercell,
    "storm-squall": build_storm_squall,
    "storm-waterspout": build_storm_waterspout,
    "storm-fire": build_storm_fire,
    "storm-cell": build_storm_cell,
    "weather": build_weather,
    "missions": build_missions,
    "report": build_report,
    "bookmark": build_bookmark,
    "film": build_film,
    "photo": build_photo,
    "record": build_record,
    "maximize": build_maximize,
    "observer": build_observer,
    "spin": build_spin,
    "studio": build_studio,
    "track": build_track,
    "sky-events": build_sky_events,
    "sky-solar": build_sky_solar,
    "sky-lunar": build_sky_lunar,
    "sky-conjunction": build_sky_conjunction,
    "sky-occultation": build_sky_occultation,
}

# Drawn in the HUD's "pixel" style: solid silhouettes with hard edges on a 24-px grid, doubled to 48 —
# the look of the gear, the camera, play / pause, the crosshair and the eye. Soft-edged thin strokes
# beside them read as another icon family (the user, 2026-10-03).
PIXEL_STYLE = {"storm-tornado", "storm-hurricane", "storm-haboob", "storm-supercell", "storm-squall",
               "storm-waterspout", "storm-fire", "storm-cell", "weather", "missions", "report", "bookmark", "film", "photo", "record", "maximize", "observer", "spin", "studio",
               "sky-events", "sky-solar", "sky-lunar", "sky-conjunction", "sky-occultation"}


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


def rasterise_pixel(shapes, grid=24, sub=4) -> list[list[int]]:
    """Hard-edged: each grid cell is on when at least half of it is covered, then doubled up to SIZE."""
    cell = SIZE / grid
    on = []
    for gy in range(grid):
        row = []
        for gx in range(grid):
            hits = 0
            for sy in range(sub):
                for sx in range(sub):
                    px = (gx + (sx + 0.5) / sub) * cell
                    py = (gy + (sy + 0.5) / sub) * cell
                    if any(s(px, py) for s in shapes):
                        hits += 1
            row.append(hits * 2 >= sub * sub)
        on.append(row)
    k = SIZE // grid
    return [[255 if on[y // k][x // k] else 0 for x in range(SIZE)] for y in range(SIZE)]


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
        shapes = ICONS[name]()
        pixels = rasterise_pixel(shapes) if name in PIXEL_STYLE else rasterise(shapes)
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
