"""Brightness maps: how bright a satellite looks from every direction around it, for a given Sun.

A map is an equirectangular image over the observer's direction in the satellite's body frame (longitude across,
latitude up), computed with vidkit.photometry from a model's lobes. Glints narrower than a map pixel (a mirror's is
about the size of the Sun) are drawn as marked spots at their exact mirror directions, so they never vanish between
pixels.

    m = BrightnessMap(lobes, width=6.0)
    g = m.draw(sun_body, at=ORIGIN)              # Group: image, frame, glint marks
    p = m.point(direction)                       # where a body-frame direction falls on the drawn map
"""
from __future__ import annotations

import numpy as np
from manim import Circle, Dot, Group, ImageMobject, RoundedRectangle, VGroup

from . import geom, photometry, theme


def reflect(s, n):
    """The mirror direction: where light from direction s leaves a surface with normal n."""
    s, n = np.asarray(s, float), np.asarray(n, float)
    return 2 * (s @ n) * n - s


def dir_to_lonlat(v):
    v = np.asarray(v, float)
    v = v / np.linalg.norm(v)
    return (np.degrees(np.arctan2(v[1], v[0])) % 360, np.degrees(np.arcsin(np.clip(v[2], -1, 1))))


class BrightnessMap:
    def __init__(self, lobes, width=6.0, n_lat=90, n_lon=180, lo=None, hi=None, decades=5.0):
        self.lobes = lobes
        self.width, self.height = width, width / 2
        self.n_lat, self.n_lon = n_lat, n_lon
        self.O = photometry.latlon_grid(n_lat, n_lon)
        self.lo, self.hi, self.decades = lo, hi, decades
        self.center = np.zeros(3)

    def grid(self, sun, normals=None, parts=False):
        lobes = self.lobes if normals is None else [dict(L, n=list(n)) for L, n in zip(self.lobes, normals)]
        return photometry.model_intensity(lobes, sun, self.O, per_lobe=parts)

    def point(self, v):
        lon, lat = dir_to_lonlat(v)
        return self.center + np.array([(lon / 360 - 0.5) * self.width, (lat / 180) * self.height, 0])

    def glints(self, sun, normals=None, min_intensity=0.0):
        """(direction, lobe index, peak intensity) of each lobe's mirror direction that faces both ways."""
        out = []
        for li, L in enumerate(self.lobes):
            n = np.asarray(L["n"] if normals is None else normals[li], float)
            if n @ sun <= 0.02 or L["f0"] * L["area"] <= 0:
                continue
            r = reflect(sun, n)
            I = float(photometry.model_intensity([dict(L, n=list(n))], sun, r[None, :])[0])
            if I > min_intensity:
                out.append((r, li, I))
        return out

    def draw(self, sun, at=(0, 0, 0), normals=None, glint_marks=True, sun_mark=True, scale=None,
             lobe_colors=None) -> Group:
        """The map for Sun direction `sun` (body frame). `scale` = (lo, hi) to share a colour scale between maps.
        `lobe_colors`: colour the map by the brightest lobe at each direction (hue) and brightness (value)."""
        self.center = np.asarray(at, float)
        tot, parts = self.grid(sun, normals, parts=True)
        lo, hi = scale or (self.lo, self.hi)
        hi = hi if hi is not None else max(tot.max(), 1e-9)
        lo = lo if lo is not None else hi * 10 ** (-self.decades)
        x = np.clip((np.log10(np.maximum(tot, 1e-30)) - np.log10(lo)) / (np.log10(hi) - np.log10(lo)), 0, 1)
        if lobe_colors is not None:
            dom = parts.argmax(0)
            pal = np.array([geom._rgb(c) for c in lobe_colors])
            rgb = pal[dom] * (0.08 + 0.92 * x[..., None])
        else:
            rgb = geom.heat_colormap(x)
        img = (np.clip(rgb[::-1], 0, 1) * 255).astype(np.uint8)
        k = max(1, int(round(1200 / self.n_lon)))
        img = np.repeat(np.repeat(img, k, 0), k, 1)
        im = ImageMobject(img)
        im.width = self.width
        im.move_to(self.center)
        frame = RoundedRectangle(corner_radius=0.03, width=self.width, height=self.height, stroke_color=theme.RULE,
                                 stroke_width=1.5).move_to(self.center)
        g = Group(im, frame)
        if glint_marks:
            for r, li, I in self.glints(sun, normals, min_intensity=lo):
                c = theme.SUN if lobe_colors is None else lobe_colors[li]
                g.add(Circle(radius=0.09, color=c, stroke_width=2.5).move_to(self.point(r)),
                      Dot(self.point(r), radius=0.035, color=theme.TEXT))
        if sun_mark:
            g.add(Dot(self.point(sun), radius=0.08, color=theme.SUN), Circle(radius=0.13, color=theme.SUN,
                                                                             stroke_width=1.5).move_to(self.point(sun)))
        return g


def path_segments(m: BrightnessMap, dirs, color=theme.SKY, width=3):
    """A path of body-frame directions drawn on the map as few shapes, split where it wraps around in longitude."""
    from manim import VMobject
    g = VGroup()
    run, prev = [], None
    for d in dirs:
        p = m.point(d)
        if prev is not None and abs(p[0] - prev[0]) > m.width / 2:
            if len(run) >= 2:
                g.add(VMobject(stroke_color=color, stroke_width=width).set_points_as_corners(run))
            run = []
        run.append(p)
        prev = p
    if len(run) >= 2:
        g.add(VMobject(stroke_color=color, stroke_width=width).set_points_as_corners(run))
    return g
