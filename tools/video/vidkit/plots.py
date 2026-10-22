"""Chart frames for data plots: axes along the box's left and bottom edges (Manim's Axes put each axis
at the other's zero), optional log scales and an inverted y (magnitudes: brighter is up).

    f = ChartFrame(x=(20, 150), y=(4, 10), invert_y=True, width=10, height=5, at=ORIGIN)
    f.add_ticks(x=[30, 60, 90], y=[5, 7, 9], x_fmt="{:.0f}°")
    dot = Dot(f.p(90, 7.2))
"""
from __future__ import annotations

import math

import numpy as np
from manim import DOWN, LEFT, Line, VGroup

from . import theme


class ChartFrame(VGroup):
    def __init__(self, x=(0, 1), y=(0, 1), width=8.0, height=4.5, at=(0, 0, 0), log_x=False, log_y=False,
                 invert_y=False, x_label: str | None = None, y_label: str | None = None, color=theme.MUTED, **kw):
        super().__init__(**kw)
        self.x0, self.x1 = x
        self.y0, self.y1 = y
        self.w, self.h = width, height
        self.log_x, self.log_y, self.invert_y = log_x, log_y, invert_y
        self.origin = np.asarray(at, float) + np.array([-width / 2, -height / 2, 0])
        self.color = color
        self.axes = VGroup(Line(self.origin, self.origin + [width, 0, 0], color=color, stroke_width=2),
                           Line(self.origin, self.origin + [0, height, 0], color=color, stroke_width=2))
        self.add(self.axes)
        self.ticks = VGroup()
        self.add(self.ticks)
        self.labels = VGroup()
        if x_label:
            self.labels.add(theme.text(x_label, 22, color).next_to(self.axes[0], DOWN, buff=0.6))
        if y_label:
            self.labels.add(theme.text(y_label, 22, color).rotate(math.pi / 2).next_to(self.axes[1], LEFT, buff=0.75))
        self.add(self.labels)

    def _u(self, v, a, b, log):
        if log:
            v, a, b = math.log10(max(v, 1e-30)), math.log10(a), math.log10(b)
        return (v - a) / (b - a)

    def p(self, x, y):
        """Scene point of a data point (clamped to the frame)."""
        u = min(max(self._u(x, self.x0, self.x1, self.log_x), -0.02), 1.02)
        v = min(max(self._u(y, self.y0, self.y1, self.log_y), -0.02), 1.02)
        if self.invert_y:
            v = 1 - v
        return self.origin + np.array([u * self.w, v * self.h, 0])

    def add_ticks(self, x=(), y=(), x_fmt="{:g}", y_fmt="{:g}", size=18):
        for xv in x:
            pt = self.p(xv, self.y1 if self.invert_y else self.y0)
            self.ticks.add(Line(pt, pt + [0, -0.1, 0], color=self.color, stroke_width=2))
            lab = x_fmt(xv) if callable(x_fmt) else x_fmt.format(xv)
            self.ticks.add(theme.text(lab, size, self.color).next_to(pt, DOWN, buff=0.18))
        for yv in y:
            pt = self.origin + [0, self.p(self.x0, yv)[1] - self.origin[1], 0]
            self.ticks.add(Line(pt, pt + [-0.1, 0, 0], color=self.color, stroke_width=2))
            lab = y_fmt(yv) if callable(y_fmt) else y_fmt.format(yv)
            self.ticks.add(theme.text(lab, size, self.color).next_to(pt, LEFT, buff=0.18))
        return self

    def polyline(self, xs, ys, color=theme.TEAL, width=4):
        """One shape through the points (split at non-finite values): fast enough to rebuild every frame."""
        from manim import VMobject
        g = VGroup()
        run = []
        for a, b in zip(xs, ys):
            if np.isfinite(a) and np.isfinite(b):
                run.append(self.p(a, b))
            elif len(run) >= 2:
                g.add(VMobject(stroke_color=color, stroke_width=width).set_points_as_corners(run))
                run = []
            else:
                run = []
        if len(run) >= 2:
            g.add(VMobject(stroke_color=color, stroke_width=width).set_points_as_corners(run))
        return g
