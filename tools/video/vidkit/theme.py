"""The look shared by every scene: palette, type, and a few standard layout pieces.

Scenes call `apply()` once (or subclass `ThemedScene`) so a whole video has one background, one font
and one set of colours. Swap the values here to restyle every project.
"""
from __future__ import annotations

from manim import (
    DOWN, LEFT, RIGHT, UL, UP, UR, Create, FadeIn, FadeOut, Line, MovingCameraScene, Rectangle,
    RoundedRectangle, Text, VGroup, Write, config,
)

# Palette — dark, low-saturation base with a red accent (the app's HUD) and a few data colours.
BG = "#0b0d12"
PANEL = "#151923"
RULE = "#2a3040"
TEXT = "#e8e6e1"
MUTED = "#8a90a0"
ACCENT = "#e0453a"   # the HUD's red: emphasis, the thing being explained
SUN = "#f2c14e"      # sunlight, glints, the "bright" end of a scale
SKY = "#5fa8ff"      # observer, data from observations
TEAL = "#3fc1a5"     # model / simulation results
VIOLET = "#a78bfa"
# Categorical colours for groups / lobes / materials, in order. Distinct in both lightness and hue.
CATEGORICAL = ["#5fa8ff", "#f2c14e", "#3fc1a5", "#e0453a", "#a78bfa", "#f08bb4", "#9bd16b",
               "#ff9f43", "#7fd3f7", "#c9a27e", "#d6d6d6", "#6c7bff"]

FONT = "Segoe UI"
MONO = "Consolas"


def apply():
    config.background_color = BG


def cat(i: int) -> str:
    return CATEGORICAL[i % len(CATEGORICAL)]


def text(s: str, size: float = 30, color: str = TEXT, weight: str = "NORMAL", mono: bool = False, **kw) -> Text:
    return Text(s, font=MONO if mono else FONT, font_size=size, color=color, weight=weight, **kw)


def title(s: str, size: float = 54) -> Text:
    return text(s, size, weight="BOLD")


def section_label(number: int | str, name: str) -> VGroup:
    """Small caps label at the top left: '02  FACETS TO LOBES' with the accent number."""
    n = text(f"{number:02d}" if isinstance(number, int) else str(number), 22, ACCENT, weight="BOLD")
    t = text(name.upper(), 22, MUTED)
    g = VGroup(n, t).arrange(RIGHT, buff=0.25)
    return g.to_corner(UL, buff=0.45)


def wrap(s: str, width: int = 64) -> str:
    """Break a sentence into lines of at most ~width characters (balanced, not greedy-ragged)."""
    import textwrap
    lines = textwrap.wrap(s, width)
    if len(lines) == 2:  # balance two lines so the second is not a stub
        words, best = s.split(), None
        for i in range(1, len(words)):
            a, b = " ".join(words[:i]), " ".join(words[i:])
            score = max(len(a), len(b))
            if best is None or score < best[0]:
                best = (score, [a, b])
        lines = best[1]
    return "\n".join(lines)


def caption(s: str, size: float = 28) -> Text:
    """Narration along the bottom of the frame: wrapped to two lines, shrunk only if it still does not fit."""
    w = wrap(s)
    if w.count("\n") > 1:  # more than two lines reads as a paragraph: shorten it
        import sys
        print(f"[vidkit] caption longer than two lines: {s!r}", file=sys.stderr, flush=True)
    t = text(w, size, TEXT, line_spacing=0.9)
    if t.width > config.frame_width - 1.0:
        t.scale_to_fit_width(config.frame_width - 1.0)
    return t.to_edge(DOWN, buff=0.35)


def counter(value: float, fmt: str = "{:,.0f}", size: float = 100, color: str = TEXT) -> Text:
    """A number in the theme's font (DecimalNumber is set in LaTeX's serif). Use with always_redraw."""
    return text(fmt.format(value), size, color, weight="BOLD")


def stat(label: str, value: str, color: str = TEXT, width: float = 2.6) -> VGroup:
    """A label over a large value in a dark inset (the app's stat tile)."""
    v = text(value, 40, color, weight="BOLD")
    lab = text(label.upper(), 16, MUTED)
    box = RoundedRectangle(corner_radius=0.08, width=max(width, v.width + 0.5), height=1.35,
                           fill_color=PANEL, fill_opacity=1, stroke_color=RULE, stroke_width=1)
    lab.move_to(box.get_top() + DOWN * 0.3)
    v.move_to(box.get_center() + DOWN * 0.15)
    return VGroup(box, lab, v)


def card(content, pad: float = 0.35, color: str = PANEL) -> VGroup:
    box = RoundedRectangle(corner_radius=0.1, width=content.width + 2 * pad, height=content.height + 2 * pad,
                           fill_color=color, fill_opacity=1, stroke_color=RULE, stroke_width=1)
    box.move_to(content)
    return VGroup(box, content)


def source_note(s: str) -> Text:
    """A citation in the bottom right corner."""
    return text(s, 16, MUTED).to_corner(DOWN + RIGHT, buff=0.3)


class ThemedScene(MovingCameraScene):
    """Base scene: the theme's background, plus helpers for the section label and captions."""

    section: tuple[int, str] | None = None

    def setup(self):
        apply()
        self.camera.background_color = BG
        self._caption = None
        if self.section:
            self.add(section_label(*self.section))

    READ_WPS = 2.6  # words per second a viewer reads comfortably

    def say(self, s: str, run_time: float = 0.6):
        """Replace the bottom caption. Pair with read() to hold until it has been read."""
        self.read(0)  # the previous caption gets its reading time first
        new = caption(s)
        if self._caption is None:
            self.play(FadeIn(new, shift=UP * 0.1), run_time=run_time)
        else:
            self.play(FadeOut(self._caption, shift=UP * 0.1), FadeIn(new, shift=UP * 0.1), run_time=run_time)
        self._caption = new
        self._caption_t = self.renderer.time
        self._caption_words = len(s.split())

    def read(self, extra: float = 0.3):
        """Wait until the current caption has been on screen long enough to read (minus time already spent)."""
        if self._caption is None:
            return
        need = 1.2 + self._caption_words / self.READ_WPS + extra
        left = need - (self.renderer.time - self._caption_t)
        if left > 0.05:
            self.wait(left)

    def clear_caption(self, run_time: float = 0.4):
        if self._caption is not None:
            self.read(0)
            self.play(FadeOut(self._caption), run_time=run_time)
            self._caption = None
