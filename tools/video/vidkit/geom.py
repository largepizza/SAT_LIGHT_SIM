"""Flat-shaded 3D drawn as 2D vector shapes, and the unit sphere of directions.

Manim's ThreeDScene renders slowly and shades flatly anyway, so meshes are projected here instead:
triangles are rotated by a view (yaw about +z, then pitch), back faces culled, sorted far to near and
drawn as filled polygons. Build a new VGroup per frame (always_redraw) to animate the view or colours.

View convention: after rotation x is right, z is up, y points into the screen; a face is visible when
its rotated normal has y < 0.
"""
from __future__ import annotations

import numpy as np
from manim import (
    Circle, Dot, ImageMobject, Polygon, VGroup, VMobject, color_to_rgb, rgb_to_color,
)

from . import theme


def view_matrix(yaw_deg: float, pitch_deg: float) -> np.ndarray:
    y, p = np.radians(yaw_deg), np.radians(pitch_deg)
    rz = np.array([[np.cos(y), -np.sin(y), 0], [np.sin(y), np.cos(y), 0], [0, 0, 1]])
    rx = np.array([[1, 0, 0], [0, np.cos(p), -np.sin(p)], [0, np.sin(p), np.cos(p)]])
    return rx @ rz


def _rgb(c) -> np.ndarray:
    return np.asarray(color_to_rgb(c), dtype=float) if not isinstance(c, np.ndarray) else c


def mix_colors(a, b, t: float):
    """Per-item blend of two colour lists (hex or rgb), t in [0, 1]."""
    return [(1 - t) * _rgb(x) + t * _rgb(y) for x, y in zip(a, b)]


class Mesh:
    """Triangles in a body frame. `tris` (N,3,3) metres, `normals` (N,3)."""

    def __init__(self, tris, normals):
        self.tris = np.asarray(tris, dtype=float)
        self.normals = np.asarray(normals, dtype=float)
        lo, hi = self.tris.reshape(-1, 3).min(0), self.tris.reshape(-1, 3).max(0)
        self.center = 0.5 * (lo + hi)
        self.radius = float(np.linalg.norm(self.tris.reshape(-1, 3) - self.center, axis=1).max())

    def draw(self, colors, yaw=30.0, pitch=-20.0, size=3.0, at=(0, 0, 0), light=(-0.4, -0.7, 0.6),
             ambient=0.35, mask=None, outline: float = 0.0) -> VGroup:
        """colors: one per triangle (hex or rgb). size: the model's bounding diameter in scene units.
        mask: optional bool per triangle (False = not drawn)."""
        R = view_matrix(yaw, pitch)
        P = (self.tris - self.center) @ R.T * (size / (2 * self.radius))
        N = self.normals @ R.T
        L = np.asarray(light, float)
        L /= np.linalg.norm(L)
        vis = N[:, 1] < 0
        if mask is not None:
            vis &= np.asarray(mask, bool)
        order = np.argsort(-P[:, :, 1].mean(1))
        shade = ambient + (1 - ambient) * np.clip(N @ L, 0, 1)
        at = np.asarray(at, float)
        g = VGroup()
        for i in order:
            if not vis[i]:
                continue
            c = np.clip(_rgb(colors[i]) * shade[i], 0, 1)
            pts = [np.array([v[0], v[2], 0.0]) + at for v in P[i]]
            hexc = rgb_to_color(c)
            g.add(Polygon(*pts, fill_color=hexc, fill_opacity=1.0, stroke_color=hexc if outline <= 0 else theme.BG,
                          stroke_width=max(outline, 0.6)))
        return g


class SphereView:
    """Orthographic view of the unit sphere of directions (where facet normals live).

    project(v) -> scene point; front(v) -> whether v faces the viewer."""

    def __init__(self, radius=1.8, at=(0, 0, 0), yaw=30.0, pitch=-20.0):
        self.radius, self.at = radius, np.asarray(at, float)
        self.R = view_matrix(yaw, pitch)

    def rot(self, v):
        return np.asarray(v, float) @ self.R.T

    def project(self, v):
        r = self.rot(v)
        return self.at + self.radius * np.array([r[..., 0], r[..., 2], 0.0 * r[..., 0]]).T

    def front(self, v):
        return self.rot(v)[..., 1] < 0

    def outline(self, color=theme.RULE) -> VGroup:
        g = VGroup(Circle(radius=self.radius, color=color, stroke_width=2).move_to(self.at))
        for lat in (-60, -30, 0, 30, 60):
            g.add(*self._curve([(np.cos(np.radians(lat)) * np.cos(t), np.cos(np.radians(lat)) * np.sin(t),
                                 np.sin(np.radians(lat))) for t in np.linspace(0, 2 * np.pi, 97)], color))
        for lon in range(0, 180, 30):
            g.add(*self._curve([(np.cos(t) * np.cos(np.radians(lon)), np.cos(t) * np.sin(np.radians(lon)), np.sin(t))
                                for t in np.linspace(0, 2 * np.pi, 97)], color))
        return g

    def _curve(self, pts, color):
        """Front half solid, back half faint."""
        pts = np.array(pts)
        out, run, runFront = [], [], None
        for p in pts:
            f = bool(self.front(p))
            if runFront is None or f == runFront:
                run.append(p)
            else:
                out.append((runFront, run))
                run = [run[-1], p]
            runFront = f
        out.append((runFront, run))
        mobs = []
        for f, r in out:
            if len(r) < 2:
                continue
            m = VMobject(stroke_color=color, stroke_width=1.2 if f else 0.6, stroke_opacity=0.9 if f else 0.25)
            m.set_points_as_corners([self.project(p) for p in r])
            mobs.append(m)
        return mobs

    def dot(self, v, radius=0.06, color=theme.TEXT) -> Dot:
        f = bool(self.front(v))
        return Dot(self.project(v), radius=radius, color=color, fill_opacity=1.0 if f else 0.3)

    def heatmap(self, grid: np.ndarray, px: int = 360, cmap=None, log_range: float = 4.0) -> ImageMobject:
        """grid: (n_lat, n_lon) values over lat -90..90 (rows) and lon 0..360 (cols), cell centred, as in
        SatModelTool's dump. Drawn on the front hemisphere, log scaled over `log_range` decades below the max."""
        cmap = cmap or heat_colormap
        n_lat, n_lon = grid.shape
        u = np.linspace(-1, 1, px)
        X, Z = np.meshgrid(u, -u)
        inside = X ** 2 + Z ** 2 <= 1
        Y = -np.sqrt(np.clip(1 - X ** 2 - Z ** 2, 0, 1))  # front: rotated y < 0
        d = np.stack([X, Y, Z], -1) @ self.R  # back to the body frame (R orthonormal)
        lat = np.degrees(np.arcsin(np.clip(d[..., 2], -1, 1)))
        lon = np.degrees(np.arctan2(d[..., 1], d[..., 0])) % 360
        fi = np.clip((lat + 90) / (180 / n_lat) - 0.5, 0, n_lat - 1)
        fj = (lon / (360 / n_lon) - 0.5) % n_lon
        i0, j0 = np.floor(fi).astype(int), np.floor(fj).astype(int)
        i1, j1 = np.minimum(i0 + 1, n_lat - 1), (j0 + 1) % n_lon
        ti, tj = fi - i0, fj - j0
        v = ((1 - ti) * (1 - tj) * grid[i0, j0] + (1 - ti) * tj * grid[i0, j1]
             + ti * (1 - tj) * grid[i1, j0] + ti * tj * grid[i1, j1])
        vmax = max(grid.max(), 1e-30)
        x = np.clip(1 + np.log10(np.maximum(v, 1e-30) / vmax) / log_range, 0, 1)
        rgb = cmap(x)
        img = np.zeros((px, px, 4), dtype=np.uint8)
        img[..., :3] = (rgb * 255).astype(np.uint8)
        img[..., 3] = np.where(inside, 255, 0)
        m = ImageMobject(img)
        m.height = 2 * self.radius
        m.move_to(self.at)
        return m


_HEAT = np.array([[0.04, 0.05, 0.07], [0.20, 0.08, 0.35], [0.62, 0.15, 0.38], [0.93, 0.40, 0.15],
                  [0.98, 0.78, 0.30], [1.00, 0.98, 0.85]])


def heat_colormap(x: np.ndarray) -> np.ndarray:
    """Dark -> violet -> red -> gold -> white, x in [0, 1]."""
    x = np.clip(x, 0, 1) * (len(_HEAT) - 1)
    i = np.minimum(np.floor(x).astype(int), len(_HEAT) - 2)
    t = (x - i)[..., None]
    return _HEAT[i] * (1 - t) + _HEAT[i + 1] * t


def heat_image(grid: np.ndarray, width: float, lo: float | None = None, hi: float | None = None, cmap=None,
               upscale: int = 8) -> ImageMobject:
    """A lat/lon grid (rows lat -90..90, cols lon 0..360) as an equirectangular image, log scaled from `lo`
    (default: the 2nd percentile) to `hi` (default: the max). North up. Share lo / hi between maps you compare."""
    cmap = cmap or heat_colormap
    g = np.asarray(grid, float)
    lo = lo if lo is not None else max(np.percentile(g[g > 0], 2), 1e-30)
    hi = hi if hi is not None else g.max()
    x = np.clip((np.log10(np.maximum(g, 1e-30)) - np.log10(lo)) / (np.log10(hi) - np.log10(lo)), 0, 1)
    if upscale > 1:  # smooth (bilinear) upsampling of the scaled values, wrapping in longitude
        from scipy.ndimage import zoom
        x = np.clip(zoom(np.concatenate([x[:, -1:], x, x[:, :1]], 1), upscale, order=1)[:, upscale:-upscale], 0, 1)
    rgb = (cmap(x[::-1]) * 255).astype(np.uint8)
    m = ImageMobject(rgb)
    m.width = width
    return m


def latlon_to_xy(img: ImageMobject, v) -> np.ndarray:
    """Scene point of a body-frame direction on a heat_image."""
    v = np.asarray(v, float)
    lat = np.degrees(np.arcsin(np.clip(v[2], -1, 1)))
    lon = np.degrees(np.arctan2(v[1], v[0])) % 360
    c = img.get_center()
    return c + np.array([(lon / 360 - 0.5) * img.width, (lat / 180) * img.height, 0])


def live_group(build):
    """Like always_redraw, for groups that contain images (ImageMobject cannot `become` another, so always_redraw
    raises NotImplementedError on them): a Group whose children are swapped for build()'s every frame."""
    from manim import Group

    g = Group(*build().submobjects)

    def update(m):
        new = build()
        m.remove(*m.submobjects)
        m.add(*new.submobjects)

    g.add_updater(update)
    return g
