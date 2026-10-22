"""Why satellites flash, and how the simulator predicts it.

For a viewer who knows satellites can flare but not why. Order: two kinds of reflection -> one flat mirror and its
brightness map -> the formula, term by term -> more complex satellites -> the GPU -> a real pass compared with the
literature (scenes_pass.py). Every number on screen comes from the files `python -m vidkit data sat_lighting` writes.
Render one scene:  python -m vidkit render sat_lighting MirrorMap
"""
from __future__ import annotations

import numpy as np
from manim import (
    DOWN, LEFT, ORIGIN, RIGHT, UP, Arrow, Circle, Create, Dot, FadeIn, FadeOut, GrowArrow, GrowFromCenter, Indicate,
    LaggedStart, Line, MathTex, Polygon, Rectangle, RoundedRectangle, ValueTracker, VGroup, Write, always_redraw,
    linear, smooth,
)

import vidkit
from vidkit import photometry, theme
from vidkit.data import BenchRun, LobeDump
from vidkit.geom import SphereView, live_group, mix_colors, rgb_to_color
from vidkit.maps import BrightnessMap, path_segments, reflect
from vidkit.plots import ChartFrame
from vidkit.theme import ThemedScene, text

DATA = vidkit.project_data()


def dump(model: str) -> LobeDump:
    return LobeDump(DATA / "lobes" / f"{model}.lobes.json")


def bench(name: str) -> BenchRun:
    return BenchRun(sorted((DATA / "bench").glob(f"{name}__*.json"))[0])


def unit(*v):
    v = np.array(v, float)
    return v / np.linalg.norm(v)


def sun_from(lon_deg, lat_deg):
    """A body-frame direction from a map longitude / latitude (degrees)."""
    a, e = np.radians(lon_deg), np.radians(lat_deg)
    return np.array([np.cos(e) * np.cos(a), np.cos(e) * np.sin(a), np.sin(e)])


def lobe_palette(d: LobeDump) -> list[list[np.ndarray]]:
    """A colour per lobe per level, siblings that will merge sharing a hue."""
    n = len(d.levels)
    cols = [None] * n
    last = d.levels[-1]
    cols[-1] = [None] * len(last.area)
    for rank, i in enumerate(np.argsort(-last.area)):
        cols[-1][i] = np.array(vidkit.geom._rgb(theme.cat(rank)))
    for k in range(n - 2, -1, -1):
        par = d.lobe_parent(k, k + 1)
        lv = d.levels[k]
        cols[k] = [None] * len(lv.area)
        for p in np.unique(par):
            kids = np.where(par == p)[0]
            kids = kids[np.argsort(-lv.area[kids])]
            base = cols[k + 1][p]
            for r, c in enumerate(kids):
                t = 0.18 * ((r + 1) // 2)
                cols[k][c] = base if r == 0 else np.clip(base + t * (1 - base) if r % 2 else base * (1 - t), 0, 1)
    return cols


# ───────────────────────────────────────────────────────────────────────────────────────────────────────────
class Title(ThemedScene):
    def construct(self):
        t = theme.title("Why do satellites flash?", 60)
        sub = text("and how a simulator can predict it, for a million of them at once", 30, theme.MUTED)
        VGroup(t, sub).arrange(DOWN, buff=0.45).move_to(UP * 0.3)
        bar = Line(LEFT * 1.2, RIGHT * 1.2, color=theme.ACCENT, stroke_width=5).next_to(sub, DOWN, buff=0.6)
        self.play(Write(t), run_time=1.4)
        self.play(FadeIn(sub, shift=UP * 0.2), GrowFromCenter(bar), run_time=1.0)
        self.wait(2.2)
        self.play(FadeOut(VGroup(t, sub, bar)), run_time=0.8)


class TwoReflections(ThemedScene):
    """Matte vs mirror, a beam sweeping over an observer, and magnitudes."""
    section = (1, "Reflected sunlight")

    def construct(self):
        gy = -2.2
        ground = Line(LEFT * 7, RIGHT * 7, color=theme.RULE, stroke_width=3).set_y(gy)
        sun = VGroup(Circle(radius=0.32, color=theme.SUN, fill_color=theme.SUN, fill_opacity=1),
                     *[Line(ORIGIN, 0.25 * unit(np.cos(a), np.sin(a), 0), color=theme.SUN, stroke_width=3)
                       .shift(0.45 * unit(np.cos(a), np.sin(a), 0)) for a in np.linspace(0, 2 * np.pi, 9)[:-1]])
        sun.move_to(LEFT * 6.0 + UP * 0.2)
        self.add(ground)
        self.say("A satellite makes no light of its own. We only see it because it reflects sunlight.")
        self.play(FadeIn(sun, scale=0.6))

        # Both plates are lit from the low Sun on the left and face down towards the ground (a night pass).
        def plate(at, n, color=theme.TEXT, half=0.7):
            t = np.array([n[1], -n[0], 0])
            return Line(at - half * t, at + half * t, color=color, stroke_width=8)

        # Matte plate: light scattered every way below it (a cosine lobe about its normal).
        pa = LEFT * 2.4 + UP * 1.5
        nA = unit(-0.45, -1.0, 0)
        plateA = plate(pa, nA)
        inA = Arrow(sun.get_center() + 0.5 * unit(*(pa - sun.get_center())), pa, buff=0.05, color=theme.SUN, stroke_width=4)
        tA = np.array([nA[1], -nA[0], 0])
        fan = VGroup(*[Arrow(pa, pa + 1.5 * np.sin(a) * (np.cos(a) * tA + np.sin(a) * nA), buff=0.0, color=theme.SKY,
                             stroke_width=3, max_tip_length_to_length_ratio=0.2)
                       for a in np.linspace(0.3, np.pi - 0.3, 9)])
        labA = text("matte paint", 26, theme.SKY).next_to(plateA, UP, buff=0.3)
        self.play(Create(plateA), FadeIn(labA))
        self.play(GrowArrow(inA))
        self.play(LaggedStart(*[GrowArrow(a) for a in fan], lag_ratio=0.08), run_time=1.2)
        self.say("Matte paint scatters it in every direction: dim, but visible from anywhere below.")
        self.read()

        # Mirror plate: tilted so the reflected beam lands on one spot of the ground.
        pb = RIGHT * 2.0 + UP * 1.5
        spot_x = 3.9
        d_in = unit(*(pb - sun.get_center()))
        d_out = unit(spot_x - pb[0], gy - pb[1], 0)
        nB = unit(*(d_out - d_in))
        plateB = plate(pb, nB)
        labB = text("mirror", 26, theme.SUN).next_to(plateB, UP, buff=0.3)
        inB = Arrow(sun.get_center() + 0.5 * d_in, pb, buff=0.05, color=theme.SUN, stroke_width=4)
        side = np.array([-d_out[1], d_out[0], 0])
        beam = Polygon(pb - 0.05 * side, pb + 0.05 * side, np.array([spot_x + 0.32, gy, 0]),
                       np.array([spot_x - 0.32, gy, 0]), fill_color=theme.SUN, fill_opacity=0.55, stroke_width=0)
        self.play(Create(plateB), FadeIn(labB))
        self.play(GrowArrow(inB))
        self.play(FadeIn(beam), run_time=1.0)
        self.say("A mirror sends almost all of it one way. From most places you see nothing; inside that beam it is dazzling.")
        self.read()

        # An observer walks through the beam's spot: their brightness meter flares.
        x = ValueTracker(spot_x - 3.2)
        person = always_redraw(lambda: VGroup(Dot(radius=0.09, color=theme.TEXT).move_to([x.get_value(), gy + 0.42, 0]),
                                              Line([x.get_value(), gy + 0.33, 0], [x.get_value(), gy, 0],
                                                   color=theme.TEXT, stroke_width=4)))

        def level():
            dx = (x.get_value() - spot_x) / 0.35
            return 0.04 + 0.96 * np.exp(-2.0 * dx * dx)
        mx = 6.1
        meter = always_redraw(lambda: VGroup(
            Rectangle(width=0.4, height=2.0, stroke_color=theme.RULE, stroke_width=1.5).move_to([mx, -0.6, 0]),
            Rectangle(width=0.4, height=2.0 * level(), stroke_width=0, fill_color=theme.SUN, fill_opacity=1)
            .move_to([mx, -1.6 + 1.0 * level(), 0])))
        mlab = text("what they see", 20, theme.MUTED).move_to([mx, 0.65, 0])
        self.play(FadeIn(person), FadeIn(meter), FadeIn(mlab))
        self.say("As the satellite moves, that spot sweeps across the ground. Whoever it passes over sees a flare.")
        self.play(x.animate.set_value(spot_x + 3.0), run_time=5.0, rate_func=linear)
        self.read()
        self.play(*[FadeOut(m) for m in (person, meter, mlab, beam, inB, plateB, labB, inA, fan, plateA, labA, sun, ground)])

        # Magnitudes in one line.
        items = [(-4, "Venus"), (0, "bright stars"), (3, "limit in a city"), (6, "limit in a dark sky")]
        bar = Line(LEFT * 5, RIGHT * 5, color=theme.MUTED, stroke_width=2).shift(UP * 0.4)
        scale = VGroup()
        for m, name in items:
            x0 = -5 + 10 * (m + 5) / 12
            scale.add(Line([x0, 0.3, 0], [x0, 0.5, 0], color=theme.MUTED),
                      text(f"{m:+d}" if m else "0", 26).move_to([x0, 0.9, 0]),
                      text(name, 18, theme.MUTED).move_to([x0, 0.0, 0]))
        arrow = Arrow(RIGHT * 4.6 + DOWN * 0.8, LEFT * 4.6 + DOWN * 0.8, color=theme.SUN, stroke_width=4, buff=0)
        alab = text("brighter", 22, theme.SUN).next_to(arrow, DOWN, buff=0.1)
        self.play(Create(bar), FadeIn(scale), GrowArrow(arrow), FadeIn(alab))
        self.say("Astronomers measure brightness in magnitudes. Smaller numbers are brighter, and 5 magnitudes is 100 times.")
        self.read(0.8)
        self.clear_caption()
        self.play(*[FadeOut(m) for m in self.mobjects])


class MirrorMap(ThemedScene):
    """The Reflect Orbital mirror: its brightness map, the glint following the Sun, a pass crossing the glint."""
    section = (2, "A flat mirror")

    def construct(self):
        d = dump("reflect_orbital")
        lobes = d.levels[0].raw
        yaw = ValueTracker(25.0)
        mesh = always_redraw(lambda: d.mesh.draw(d.material_colors(), yaw=yaw.get_value(), pitch=-30, size=3.4,
                                                 at=LEFT * 3.6 + UP * 0.3))
        self.add(mesh)
        info = VGroup(text("A flat mirror", 30, weight="BOLD"),
                      text("a 55 m square of aluminised film", 24, theme.MUTED),
                      text("one side mirror, the back matte", 24, theme.MUTED)).arrange(DOWN, aligned_edge=LEFT, buff=0.15)
        info.move_to(RIGHT * 2.6 + UP * 0.4)
        self.say("Start with the simplest satellite in the simulator: one flat mirror, the kind Reflect Orbital proposes.")
        self.play(FadeIn(info), yaw.animate(rate_func=linear).set_value(80), run_time=3.0)
        self.read()

        # A globe of viewing directions around the satellite.
        sun = sun_from(0, 55)
        # Turn the globe so the glint faces the viewer (SphereView: a direction faces us when its rotated y < 0).
        g_dir = reflect(sun, [0, 0, 1])
        from vidkit.geom import view_matrix
        best = min(((float((view_matrix(yw, pt) @ g_dir)[1]), yw, pt) for yw in range(-180, 180, 10)
                    for pt in range(-60, 1, 10)))
        sv = SphereView(radius=2.3, at=RIGHT * 2.8 + DOWN * 0.1, yaw=best[1] + 20, pitch=best[2])
        grid = photometry.model_intensity(lobes, sun, photometry.latlon_grid(90, 180))
        hemi = sv.heatmap(grid, px=420, log_range=6)
        outl = sv.outline()
        self.play(FadeOut(info), FadeIn(outl), FadeIn(hemi), yaw.animate(rate_func=linear).set_value(110), run_time=1.5)
        self.say("Imagine a globe around the satellite. Colour each point by how bright the satellite looks from there.")
        self.read()

        bm = BrightnessMap(lobes, width=8.4, decades=6)
        mapg = bm.draw(sun, at=UP * 0.45)
        labels = VGroup(text("direction around the satellite  →", 20, theme.MUTED).next_to(mapg, DOWN, buff=0.15),
                        text("in front of the mirror", 18, theme.MUTED).next_to(mapg, UP, buff=0.1).align_to(mapg, LEFT),
                        text("behind", 18, theme.MUTED).next_to(mapg, DOWN, buff=0.15).align_to(mapg, LEFT))
        self.play(FadeOut(mesh), FadeOut(outl), FadeOut(hemi), run_time=0.6)
        self.play(FadeIn(mapg), FadeIn(labels), run_time=1.2)
        self.say("Unrolled flat, like a world map. Nearly all of it is dark: a mirror is invisible from almost everywhere.")
        self.read()
        g0 = bm.glints(sun)[0][0]
        ring = Circle(radius=0.35, color=theme.ACCENT, stroke_width=4).move_to(bm.point(g0))
        sl = text("Sun", 20, theme.SUN).next_to(bm.point(sun), RIGHT, buff=0.18)
        gl = text("glint", 22, theme.ACCENT).next_to(ring, RIGHT, buff=0.1)
        self.play(Create(ring), FadeIn(gl), FadeIn(sl))
        self.say("Except for one spot: the mirror image of the Sun. That is the glint, about as small as the Sun's disc "
                 "(drawn larger here).")
        self.read()
        self.play(FadeOut(ring), FadeOut(gl), FadeOut(sl))

        # The Sun moves; the glint follows by the law of reflection.
        lon = ValueTracker(0.0)
        lat = ValueTracker(55.0)
        self.remove(mapg)
        live = live_group(lambda: bm.draw(sun_from(lon.get_value(), lat.get_value()), at=UP * 0.45))
        self.add(live)
        self.say("As the Sun moves across the mirror's sky, the glint moves to the opposite side: angle in equals angle out.")
        self.play(lon.animate.set_value(70), lat.animate.set_value(35), run_time=3.5, rate_func=smooth)
        self.play(lon.animate.set_value(-40), lat.animate.set_value(60), run_time=3.5, rate_func=smooth)
        self.read()

        # A pass: the observer's direction sweeps a path across the map; brightness over time below.
        sun = sun_from(-40, 60)
        g = reflect(sun, [0, 0, 1])
        lon_g, lat_g = np.degrees(np.arctan2(g[1], g[0])) % 360, np.degrees(np.arcsin(g[2]))
        ts = np.linspace(0, 1, 900)
        dirs = np.array([sun_from(a, b) for a, b in zip(lon_g - 60 + 120 * ts, lat_g + 7.5 * (ts - 0.5))])
        mags = photometry.mag(photometry.model_intensity(lobes, sun, dirs), 1.0e6)
        self.remove(live)
        self.play(FadeOut(labels), run_time=0.3)
        bm2 = BrightnessMap(lobes, width=6.2, decades=6)
        frozen = bm2.draw(sun, at=UP * 1.75)
        self.add(frozen)
        f = ChartFrame(x=(0, 1), y=(-17, 8), invert_y=True, width=8.0, height=1.6, at=DOWN * 1.25, y_label="magnitude")
        f.add_ticks(y=[-15, -5, 5], size=16)
        tlab = text("time during the pass  →", 18, theme.MUTED).next_to(f, DOWN, buff=0.06)
        self.play(FadeIn(f), FadeIn(tlab))
        self.say("A pass overhead traces a path across this map. Below: how bright the mirror looks at each moment.")
        k = ValueTracker(2)
        trace = always_redraw(lambda: f.polyline(ts[:int(k.get_value())], np.clip(mags[:int(k.get_value())], -17, 8),
                                                 theme.SUN, 3))
        drawn = always_redraw(lambda: path_segments(bm2, dirs[:int(k.get_value())], theme.SKY, 3))
        you = always_redraw(lambda: Dot(bm2.point(dirs[int(k.get_value()) - 1]), radius=0.08, color=theme.SKY))
        self.add(trace, drawn, you)
        self.play(k.animate.set_value(len(ts)), run_time=7.0, rate_func=linear)
        peak = float(np.nanmin(mags))
        pk = text(f"flare: magnitude {peak:.0f} at 1000 km", 22, theme.SUN).next_to(f.p(ts[np.argmin(mags)], -17), UP,
                                                                                    buff=0.05)
        self.play(FadeIn(pk))
        self.say("Only where the path crosses the glint do you see anything, and then for a fraction of a second: a flare.")
        self.read(1.0)
        self.clear_caption()
        self.play(*[FadeOut(m) for m in self.mobjects])


class MirrorLobes(ThemedScene):
    """Facets and lobes, introduced on the mirror."""
    section = (3, "Facets and lobes")

    def construct(self):
        d = dump("reflect_orbital")
        lv = d.levels[0]
        names = []
        for L in lv.raw:
            mat = d.materials[d.tri_material[L["tris"][0]]]["name"]
            names.append({"mirror_film": "mirror side", "film_back": "matte back", "bus_dark": "bus side"}.get(mat, mat))
        bus_cols = [theme.TEAL, theme.VIOLET, "#f08bb4", "#9bd16b", "#ff9f43", "#7fd3f7"]
        cols, bi = [], 0
        for nm in names:
            if nm == "mirror side":
                cols.append(theme.SUN)
            elif nm == "matte back":
                cols.append(theme.SKY)
            else:
                cols.append(bus_cols[bi % len(bus_cols)])
                bi += 1
        self.say("A model is built from flat facets. Facets that face the same way and are made of the same material act as one: a lobe.")
        rows = VGroup()
        for i, (L, nm) in enumerate(zip(lv.raw, names)):
            rows.add(VGroup(Dot(radius=0.08, color=cols[i]), text(nm, 22),
                            text(f"{L['area']:.0f} m²" if L["area"] >= 10 else f"{L['area']:.1f} m²", 22, theme.MUTED)
                            ).arrange(RIGHT, buff=0.3))
        rows.arrange(DOWN, aligned_edge=LEFT, buff=0.16).move_to(LEFT * 4.6 + UP * 0.1)
        hdr = text(f"{len(d.area)} facets  →  {len(lv.raw)} lobes", 30, theme.ACCENT, weight="BOLD").next_to(rows, UP, buff=0.4)
        self.play(FadeIn(hdr), LaggedStart(*[FadeIn(r, shift=RIGHT * 0.1) for r in rows], lag_ratio=0.1), run_time=1.6)
        self.read()
        bm = BrightnessMap(lv.raw, width=7.4, decades=6)
        mg = bm.draw(sun_from(30, 50), at=RIGHT * 2.6 + UP * 0.2, lobe_colors=cols)
        self.play(FadeIn(mg))
        self.say("Each lobe adds its own light; colour shows the brightest. The mirror's glint, and a faint glow from the dark bus.")
        self.read()
        mg2 = bm.draw(sun_from(30, -40), at=RIGHT * 2.6 + UP * 0.2, lobe_colors=cols)
        self.play(FadeOut(mg), FadeIn(mg2))
        self.say("Light the mirror from behind, and its matte back takes over: a broad, even glow instead of a glint.")
        self.read(0.8)
        self.clear_caption()
        self.play(*[FadeOut(m) for m in self.mobjects])


class LobeFormula(ThemedScene):
    """The lobe formula built up term by term, each shown on a single lobe's map."""
    section = (4, "One lobe, term by term")

    def construct(self):
        base = dict(n=[0, 0, 1], area=10.0, diff_area=10.0, albedo=0.3, f0=0.5, alpha=0.08, beckmann=False,
                    transmission=0.0)
        sun = sun_from(0, 45)
        alpha = ValueTracker(0.08)
        show = {"diff": 1.0, "spec": 0.0}
        map_at = RIGHT * 3.4 + DOWN * 0.45

        def lobe_map():
            L = dict(base, alpha=alpha.get_value(), albedo=base["albedo"] * show["diff"], f0=base["f0"] * show["spec"],
                     area=base["area"] * show["spec"])
            return BrightnessMap([L], width=5.8, n_lat=72, n_lon=144, lo=3e-3, hi=None, decades=4).draw(
                sun, at=map_at, glint_marks=False)

        eq = MathTex(r"I", r"=", r"\frac{\rho}{\pi}", r"A", r"(\mathbf n\cdot\mathbf s)", r"(\mathbf n\cdot\mathbf o)",
                     r"+", r"A", r"\,\frac{D\,F\,G}{4}", font_size=56, color=theme.TEXT).move_to(UP * 2.5)
        self.say("How bright does one lobe look, lit by the Sun and seen from one direction? Two parts add up.")
        self.play(Write(eq[0:2]))
        self.read()

        # Diffuse.
        mapm = live_group(lobe_map)
        maplab = text("one lobe facing straight up: its brightness map", 20, theme.MUTED).next_to(map_at + UP * 1.45, UP, buff=0.05)
        for part in eq[2:6]:
            part.set_color(theme.SKY)
        self.play(Write(eq[2:6]), FadeIn(mapm), FadeIn(maplab))
        notes = VGroup(text("A    the lobe's area: bigger is brighter", 22),
                       text("ρ    how much light the surface reflects", 22),
                       text("      (black paint 0.05, white paint 0.8)", 20, theme.MUTED),
                       text("n·s  how squarely it faces the Sun", 22),
                       text("n·o  how squarely it faces you", 22)).arrange(DOWN, aligned_edge=LEFT, buff=0.18)
        notes.move_to(LEFT * 3.6 + DOWN * 0.7)
        self.say("The matte part: the area, times how much light the surface reflects, times how squarely it faces the Sun and you.")
        self.play(LaggedStart(*[FadeIn(n_, shift=RIGHT * 0.1) for n_ in notes], lag_ratio=0.3), run_time=2.0)
        self.read()
        self.say("On the map that is a broad glow: brightest seen straight on, fading to nothing at the edge.")
        self.read()

        # Specular.
        self.play(FadeOut(notes))
        show["spec"], show["diff"] = 1.0, 0.0
        for part in eq[6:9]:
            part.set_color(theme.SUN)
        self.play(Write(eq[6:9]))
        notes2 = VGroup(text("D    how tightly the reflection is focused", 22),
                        text("F    surfaces reflect more at grazing angles", 22),
                        text("G    tiny bumps shading each other", 22)).arrange(DOWN, aligned_edge=LEFT, buff=0.22)
        notes2.move_to(LEFT * 3.6 + DOWN * 0.7)
        self.say("The shiny part is a glint. It appears where the lobe faces exactly halfway between the Sun and you: the mirror direction.")
        self.read()
        self.play(LaggedStart(*[FadeIn(n_, shift=RIGHT * 0.1) for n_ in notes2], lag_ratio=0.3), run_time=1.6)
        rl = always_redraw(lambda: text(f"roughness α = {alpha.get_value():.2f}", 24, theme.SUN)
                           .next_to(map_at + DOWN * 1.5, DOWN, buff=0.12))
        self.add(rl)
        self.say("D sets the glint's size. A rough surface spreads it wide and dim; a smooth one makes it small and bright.")
        self.play(alpha.animate.set_value(0.35), run_time=2.5)
        self.play(alpha.animate.set_value(0.02), run_time=2.5)
        self.read()
        sund = MathTex(r"\alpha^2 \;\to\; \alpha^2+\alpha_\odot^2", font_size=40, color=theme.SUN).next_to(notes2, DOWN, buff=0.35)
        self.play(Write(sund))
        self.say("Even a perfect mirror cannot make a glint smaller than the Sun's disc, so the Sun's size is added to the roughness.")
        self.read()

        # Both, then the sum over lobes.
        show["diff"] = 1.0
        self.play(alpha.animate.set_value(0.08), FadeOut(notes2), FadeOut(sund), FadeOut(rl), run_time=1.0)
        total = MathTex(r"I_{\text{satellite}} = \sum_{\text{lobes}} I_{\text{lobe}}", font_size=48,
                        color=theme.TEXT).move_to(LEFT * 3.6 + DOWN * 0.7)
        self.play(Write(total))
        self.say("A whole satellite's brightness is just the sum over its lobes: a handful of these small formulas.")
        self.read(1.0)
        self.clear_caption()
        self.play(*[FadeOut(m) for m in self.mobjects])


class Starlink(ThemedScene):
    """Starlink V2 Mini: a few lobes, and where on its map observers on the ground can be."""
    section = (5, "A real satellite")

    def construct(self):
        d = dump("starlink_v2_mini")
        lv = d.levels[0]
        mats = [d.materials[d.tri_material[L["tris"][0]]]["name"] for L in lv.raw]
        label = {"black_paint": "black body panels", "dielectric_mirror_gen2": "mirror film, Earth side",
                 "solar_cell": "solar cells", "array_backsheet_dark": "array backs"}
        colour = {"black_paint": theme.cat(3), "dielectric_mirror_gen2": theme.SUN, "solar_cell": theme.SKY,
                  "array_backsheet_dark": theme.VIOLET}
        cols = [colour.get(m, theme.MUTED) for m in mats]
        yaw = ValueTracker(20.0)
        mesh = always_redraw(lambda: d.mesh.draw(d.material_colors(), yaw=yaw.get_value(), pitch=25, size=4.2,
                                                 at=LEFT * 3.7 + UP * 0.4))
        self.add(mesh)
        info = VGroup(text("Starlink V2 Mini", 30, weight="BOLD"),
                      text(f"{len(d.area)} facets, {len(lv.raw)} lobes", 24, theme.MUTED),
                      text("body faces the Earth; arrays turn to the Sun", 24, theme.MUTED)).arrange(DOWN, aligned_edge=LEFT, buff=0.15)
        info.move_to(RIGHT * 2.8 + UP * 0.6)
        self.say("A Starlink is a few more lobes: a flat body facing the Earth, and two big solar arrays that turn towards the Sun.")
        self.play(FadeIn(info), yaw.animate(rate_func=linear).set_value(90), run_time=3.5)
        self.read()

        # Pose: the arrays tilted toward the Sun about +y, within the model's 22-degree limit. +z is toward the Earth.
        sun = sun_from(150, -60)
        ang = np.clip(np.arctan2(-sun[0], -sun[2]), -np.radians(22), np.radians(22))
        R = np.array([[np.cos(ang), 0, np.sin(ang)], [0, 1, 0], [-np.sin(ang), 0, np.cos(ang)]])
        normals = [R @ np.array(L["n"]) if L["group"] == 1 else np.array(L["n"]) for L in lv.raw]
        bm = BrightnessMap(lv.raw, width=8.4, decades=6)
        mg = bm.draw(sun, at=UP * 0.55, normals=normals, lobe_colors=cols)
        self.play(FadeOut(mesh), FadeOut(info))
        self.play(FadeIn(mg))
        uniq = dict(zip(mats, cols))
        keys = VGroup(*[VGroup(Dot(radius=0.07, color=c), text(label.get(m, m), 18)).arrange(RIGHT, buff=0.12)
                        for m, c in uniq.items()]).arrange(RIGHT, buff=0.5).next_to(mg, DOWN, buff=0.25)
        up = text("toward the Earth", 18, theme.MUTED).next_to(mg, UP, buff=0.08).align_to(mg, LEFT)
        self.play(FadeIn(keys), FadeIn(up))
        self.say("Its map has several glints, one for each shiny lobe; colour shows which part of the satellite the light comes from.")
        self.read()
        # Ground observers see only the Earth-facing side: within ~67 degrees of straight down from 550 km.
        frac = (90 - 23) / 180
        band = Rectangle(width=bm.width, height=bm.height * frac, stroke_color=theme.SKY, stroke_width=3)
        band.align_to(mg, UP).set_x(mg.get_x())
        bl = text("every direction it can be seen from, from the ground", 20, theme.SKY).next_to(band, DOWN, buff=0.1)
        self.play(FadeOut(keys), Create(band), FadeIn(bl))
        self.say("From the ground we only see its Earth-facing side: this band. The arrays' glint is on the far side.")
        self.read(1.0)
        self.clear_caption()
        self.play(*[FadeOut(m) for m in self.mobjects])


class FacetsToLobes(ThemedScene):
    """Hubble: curved surfaces, and merging lobes to fit a budget."""
    section = (6, "Curved surfaces")

    def construct(self):
        d = dump("hubble")
        cols = lobe_palette(d)
        mat_cols = d.material_colors()
        yaw = ValueTracker(20.0)
        pitch = -18.0
        state = {"k": 0, "mode": "material"}
        mt = ValueTracker(0.0)
        mx = ValueTracker(0.0)
        mesh_at = LEFT * 3.4 + UP * 0.25
        sph_at, sph_r = RIGHT * 3.3 + UP * 0.25, 2.0

        def tri_colors():
            k, t = state["k"], mt.get_value()
            a = [cols[k][i] for i in d.levels[k].tri_lobe]
            if t > 0 and k + 1 < len(d.levels):
                a = mix_colors(a, [cols[k + 1][i] for i in d.levels[k + 1].tri_lobe], smooth(t))
            return mix_colors(mat_cols, a, mx.get_value())

        mesh = always_redraw(lambda: d.mesh.draw(tri_colors(), yaw=yaw.get_value(), pitch=pitch, size=4.4, at=mesh_at))

        def sphere_group():
            sv = SphereView(radius=sph_r, at=sph_at, yaw=yaw.get_value(), pitch=pitch)
            g = VGroup(sv.outline())
            k, t = state["k"], smooth(mt.get_value())
            lv = d.levels[k]
            amax = d.levels[-1].area.max()
            if state["mode"] != "lobes":
                for i in range(len(d.area)):
                    g.add(sv.dot(d.normals[i], radius=0.025 + 0.06 * np.sqrt(d.area[i] / d.area.max()),
                                 color=rgb_to_color(np.clip(mat_cols[i], 0, 1))))
                return g
            par = d.lobe_parent(k, k + 1) if k + 1 < len(d.levels) else None
            for i in range(len(lv.area)):
                n, r, c, al = lv.normals[i], 0.035 + 0.22 * np.sqrt(lv.area[i] / amax), cols[k][i], lv.alpha[i]
                if par is not None and t > 0:
                    p = par[i]
                    n = (1 - t) * n + t * d.levels[k + 1].normals[p]
                    n /= max(np.linalg.norm(n), 1e-9)
                    r = (1 - t) * r + t * (0.035 + 0.22 * np.sqrt(d.levels[k + 1].area[p] / amax))
                    c = (1 - t) * c + t * cols[k + 1][p]
                    al = (1 - t) * al + t * d.levels[k + 1].alpha[p]
                hexc = rgb_to_color(np.clip(c, 0, 1))
                ring = min(al, 0.9) * sv.radius * 0.55
                if ring > r * 1.2:
                    g.add(Circle(radius=ring, color=hexc, stroke_width=1.5,
                                 stroke_opacity=0.5 if sv.front(n) else 0.15).move_to(sv.project(n)))
                g.add(sv.dot(n, radius=r, color=hexc))
            return g

        sphere = always_redraw(sphere_group)
        mlab = text("the model", 22, theme.MUTED).move_to(mesh_at + DOWN * 2.45)
        slab = text("which way each facet faces", 22, theme.MUTED).move_to(sph_at + DOWN * 2.45)
        counter = always_redraw(lambda: text(f"{len(d.area)} facets" if state["mode"] != "lobes"
                                             else f"{len(d.levels[state['k']].area)} lobes", 32, theme.ACCENT,
                                             weight="BOLD").move_to(sph_at + UP * 2.65))
        self.add(mesh)
        self.say(f"Hubble has curved surfaces. Its tube is drawn with {len(d.area)} small flat facets.")
        self.play(FadeIn(mlab), yaw.animate(rate_func=linear).set_value(60), run_time=2.5)
        self.read()
        self.add(sphere)
        self.say("Mark which way each facet faces as a dot on a globe, sized by its area. The tube's facets form a ring.")
        self.play(FadeIn(slab), FadeIn(counter), yaw.animate(rate_func=linear).set_value(110), run_time=3.0)
        self.read()
        state["mode"] = "lobes"
        self.say(f"Facets facing exactly the same way merge into one lobe, at no cost in accuracy: {len(d.levels[0].area)} lobes.")
        self.play(mx.animate.set_value(1.0), yaw.animate(rate_func=linear).set_value(140), run_time=2.5)
        self.read()
        self.say("Each tube facet stands for a curved strip, so it gets extra roughness (the rings): its glint becomes a band.")
        self.play(yaw.animate(rate_func=linear).set_value(170), run_time=3.0)
        self.read()
        for k in range(len(d.levels) - 1):
            nxt = d.levels[k + 1]
            err = np.percentile(d.errors(k + 1), 95)
            self.say(f"Fewer lobes are faster to compute. Merge the most similar neighbours: {len(nxt.area)} lobes, "
                     f"error up to {err:.2f} magnitudes.")
            self.play(mt.animate.set_value(1.0), yaw.animate(rate_func=linear).set_value(yaw.get_value() + 35), run_time=2.6)
            state["k"] = k + 1
            mt.set_value(0.0)
            self.read(0.0)
        self.say("Too few, and the brightness goes wrong. So how many is enough?")
        self.play(yaw.animate(rate_func=linear).set_value(yaw.get_value() + 30), run_time=2.0)
        self.clear_caption()
        self.play(FadeOut(VGroup(mlab, slab)), FadeOut(counter), FadeOut(mesh), FadeOut(sphere))


class BudgetKnee(ThemedScene):
    section = (7, "How many lobes?")

    def construct(self):
        d = dump("iss")
        cols = lobe_palette(d)
        k48 = next(i for i, lv in enumerate(d.levels) if lv.budget == 48)
        yaw = ValueTracker(-30.0)
        mesh = always_redraw(lambda: d.mesh.draw([cols[k48][i] for i in d.levels[k48].tri_lobe], yaw=yaw.get_value(),
                                                 pitch=-28, size=5.4, at=LEFT * 3.7 + UP * 0.35))
        cap = text(f"{len(d.area):,} facets, coloured by the 48 lobes they become", 22, theme.MUTED)
        cap.move_to(LEFT * 3.7 + DOWN * 2.3)
        self.add(mesh)
        self.say(f"The ISS is the most complex model: {len(d.area):,} facets in {len(d.levels[0].area)} distinct lobes.")
        self.play(FadeIn(cap), yaw.animate(rate_func=linear).set_value(0), run_time=2.0)
        self.read()
        f = ChartFrame(x=(6, 600), y=(0.01, 10), width=5.6, height=3.8, at=RIGHT * 3.6 + UP * 0.45, log_x=True,
                       log_y=True, x_label="number of lobes", y_label="error (magnitudes)")
        f.add_ticks(x=[6, 12, 24, 48, 128, 514], y=[0.01, 0.1, 1, 10])
        self.play(FadeIn(f), run_time=1.0)
        ns, es = [], []
        for k, lv in enumerate(d.levels[1:], start=1):
            ns.append(len(lv.area))
            es.append(np.percentile(d.errors(k), 95))
        o = np.argsort(ns)
        ns, es = np.array(ns)[o], np.array(es)[o]
        dots = VGroup(*[Dot(f.p(n, e), radius=0.07, color=theme.TEAL) for n, e in zip(ns, es)])
        curve = f.polyline(ns, es, theme.TEAL, 3)
        self.say("Compared with adding up every facet one by one: the fewer the lobes, the bigger the error.")
        self.play(LaggedStart(*[FadeIn(x) for x in dots], lag_ratio=0.15), Create(curve),
                  yaw.animate(rate_func=linear).set_value(40), run_time=3.0)
        self.read()
        e48 = np.percentile(d.errors(k48), 95)
        p48 = f.p(48, e48)
        mark = Circle(radius=0.2, color=theme.ACCENT, stroke_width=4).move_to(p48)
        lab = text(f"48 lobes: {e48:.2f} mag", 24, theme.ACCENT, weight="BOLD").next_to(p48, UP + RIGHT, buff=0.12)
        self.say(f"Every model is capped at 48 lobes. Even the ISS then stays within about {e48:.1f} magnitudes.")
        self.play(Create(mark), FadeIn(lab), yaw.animate(rate_func=linear).set_value(80), run_time=2.5)
        self.read(1.0)
        self.clear_caption()
        self.play(*[FadeOut(m) for m in (mesh, cap, f, dots, curve, mark, lab)])


class GpuPipeline(ThemedScene):
    section = (8, "A million at once")

    def box(self, title, lines, color, w=3.1):
        t = text(title, 24, color, weight="BOLD")
        body = VGroup(*[text(s, 20, theme.TEXT) for s in lines]).arrange(DOWN, aligned_edge=LEFT, buff=0.1)
        g = VGroup(t, body).arrange(DOWN, aligned_edge=LEFT, buff=0.2)
        r = RoundedRectangle(corner_radius=0.12, width=w, height=g.height + 0.45, fill_color=theme.PANEL, fill_opacity=1,
                             stroke_color=color, stroke_width=2)
        g.move_to(r)
        return VGroup(r, g)

    def construct(self):
        steps = [("Where is it?", ["its orbit, at this", "moment"], theme.MUTED),
                 ("Which way is it facing?", ["towards the Earth,", "the Sun or a target"], theme.SKY),
                 ("How bright is it?", ["add up its lobes:", "sunlight + earthlight"], theme.ACCENT),
                 ("Can you see it?", ["horizon, Earth's shadow,", "sky glow, haze"], theme.TEAL)]
        boxes = VGroup(*[self.box(a, b, c) for a, b, c in steps]).arrange(RIGHT, buff=0.45).move_to(UP * 1.2)
        arrows = VGroup(*[Arrow(boxes[i].get_right(), boxes[i + 1].get_left(), buff=0.06, color=theme.MUTED,
                                stroke_width=3, max_tip_length_to_length_ratio=0.3) for i in range(3)])
        self.say("Every frame, the graphics card answers four questions for each of 1.4 million satellites.")
        for i, b in enumerate(boxes):
            self.play(FadeIn(b, shift=RIGHT * 0.1), run_time=0.5)
            if i < 3:
                self.play(GrowArrow(arrows[i]), run_time=0.25)
        self.read()
        self.say("Each satellite gets its own tiny program: a few lobes to add up, not thousands of facets. That is what makes it fast.")
        self.play(Indicate(boxes[2], color=theme.ACCENT, scale_factor=1.08), run_time=1.2)
        self.read()
        tiles = VGroup(theme.stat("satellites, every frame", "1.4 million", theme.TEXT, 3.6),
                       theme.stat("10 million (stress test)", "1.7 ms", theme.ACCENT, 3.6),
                       theme.stat("checked against", "an exact CPU copy", theme.TEAL, 3.9)).arrange(RIGHT, buff=0.35)
        tiles.scale(0.9).move_to(DOWN * 1.35)
        self.play(LaggedStart(*[FadeIn(t, shift=UP * 0.2) for t in tiles], lag_ratio=0.25), run_time=1.5)
        self.say("Ten million satellites take under 2 milliseconds. A slower, exact copy on the main processor checks the answers.")
        self.read(1.0)
        self.clear_caption()
        self.play(*[FadeOut(m) for m in self.mobjects])


class BenchmarkTable(ThemedScene):
    section = (10, "Every dataset")

    ROWS = [
        ("mallama2020a_original", "Starlink V1.0 (2020)", "never tuned to this"),
        ("mallama2021_visorsat", "Starlink VisorSat", "two values tuned"),
        ("mallama2025_starlink_v15", "Starlink V1.5", ""),
        ("mallama2023_v2mini", "Starlink V2 Mini", ""),
        ("mallama2025_dtc", "Starlink Direct-to-Cell", "never tuned to this"),
        ("mallama2020b_oneweb", "OneWeb", ""),
        ("mallama2026_amazon_leo", "Amazon Leo", ""),
        ("mallama2025_guowang", "Guowang", "known gap"),
    ]

    def construct(self):
        head = ["satellite", "observed", "simulated", "difference", ""]
        xs = [-4.4, 0.3, 2.1, 3.8, 5.6]
        left = xs[0] - 1.9
        y0 = 2.65
        hdr = VGroup(*[text(h.upper(), 18, theme.MUTED).move_to([x, y0, 0]) for h, x in zip(head, xs)])
        hdr[0].align_to([left, 0, 0], LEFT)
        rule = Line([-6.6, y0 - 0.28, 0], [6.6, y0 - 0.28, 0], color=theme.RULE)
        self.say("The same test for every satellite with published observations: its average brightness, observed and simulated.")
        self.play(FadeIn(hdr), Create(rule))
        rows = VGroup()
        for i, (bid, name, note) in enumerate(self.ROWS):
            try:
                r = bench(bid)
            except IndexError:
                continue
            c = r.metric("mean_m1000")
            y = y0 - 0.7 - i * 0.55
            col = theme.TEAL if c["verdict"] == "pass" else theme.SUN
            rows.add(VGroup(text(name, 24).move_to([xs[0], y, 0]).align_to([left, 0, 0], LEFT),
                            text(f"{c['reference']:.2f}", 24, theme.SKY).move_to([xs[1], y, 0]),
                            text(f"{c['model']:.2f}", 24, theme.TEAL).move_to([xs[2], y, 0]),
                            text(f"{c['model'] - c['reference']:+.2f}", 24, col).move_to([xs[3], y, 0]),
                            text(note, 18, theme.MUTED).move_to([xs[4], y, 0])))
        self.play(LaggedStart(*[FadeIn(r, shift=LEFT * 0.2) for r in rows], lag_ratio=0.18), run_time=2.5)
        self.read()
        self.say("Most land within 0.3 magnitudes, a difference you would struggle to see. Two were never tuned to these observations at all.")
        self.read()
        self.say("Guowang is far off. Its hardware is unpublished, so its model is a guess. It stays in the table, marked.")
        self.read(1.0)
        self.clear_caption()
        self.play(*[FadeOut(m) for m in self.mobjects])


class Outro(ThemedScene):
    def construct(self):
        lines = VGroup(
            text("A satellite is visible because it reflects sunlight.", 30),
            text("Its flat, shiny parts make glints: those are the flares.", 30),
            text("A handful of lobes captures its brightness from every direction.", 30),
            text("The graphics card adds them up for every satellite, every frame,", 30),
            text("and the results are checked against what observers measured.", 30),
        ).arrange(DOWN, aligned_edge=LEFT, buff=0.3).move_to(UP * 0.3)
        self.play(LaggedStart(*[FadeIn(l, shift=RIGHT * 0.2) for l in lines], lag_ratio=0.6), run_time=4.0)
        foot = text("Observations: A. Mallama et al. 2020-2026.  Data exported by the simulator's own tools.", 20,
                    theme.MUTED).to_edge(DOWN, buff=0.6)
        self.play(FadeIn(foot))
        self.wait(4.0)
        self.play(FadeOut(VGroup(lines, foot)), run_time=1.0)


# ── A real pass, compared with the literature ─────────────────────────────────────────────────────────────────
def load_pass(pid="mmt_46071_19195344"):
    """The sim's trace of an observed pass (make_pass.py) with the observations, as arrays in seconds from the
    first observed point. Observed V = Clear - 0.09; both sides above the atmosphere (MMT calibrates on field stars)."""
    import json
    from vidkit import passes
    d = json.loads((DATA / "passes" / f"{pid}.out.json").read_text(encoding="utf-8"))
    p = d["observed"]
    t_obs = np.array([passes.t_j2000(s[0]) for s in p["samples"]])
    t0 = t_obs[0]
    v_obs = np.array([s[1] for s in p["samples"]]) - 0.09
    S = d["samples"]
    t_sim = np.array([s["t_j2000"] for s in S]) - t0
    v_sim = np.array([s["mag"] if s["mag"] is not None else np.nan for s in S])
    t_obs = t_obs - t0
    # 1-second running medians of the observations (the per-frame scatter is 0.1-0.2 mag)
    centres = np.arange(t_obs[0] + 0.5, t_obs[-1] - 0.5, 0.25)
    med = []
    for c in centres:
        sel = np.abs(t_obs - c) <= 0.5
        med.append(np.median(v_obs[sel]) if sel.sum() >= 3 else np.nan)
    return dict(d=d, p=p, t_obs=t_obs, v_obs=v_obs, t_sim=t_sim, v_sim=v_sim, t_med=centres, v_med=np.array(med),
                samples=S, t0=t0)


def runs(mask):
    """Index arrays of the True runs of a mask (to break a line across data gaps)."""
    out, start = [], None
    for i, m in enumerate(list(mask) + [False]):
        if m and start is None:
            start = i
        elif not m and start is not None:
            if i - start >= 2:
                out.append(np.arange(start, i))
            start = None
    return out


class PassIntro(ThemedScene):
    section = (9, "Does it match the sky?")

    def construct(self):
        P = load_pass()
        p = P["p"]
        head = VGroup(text(f"{p['satellite']}, a Starlink VisorSat", 32, weight="BOLD"),
                      text("2 May 2021, 23:53 UTC, from Nizhny Arkhyz, Russia", 26, theme.MUTED),
                      text("filmed by the Mini-MegaTORTORA camera array, 10 frames a second", 26, theme.MUTED)
                      ).arrange(DOWN, aligned_edge=LEFT, buff=0.15).to_edge(UP, buff=1.0).shift(LEFT * 1.0)
        self.say("Averages are one test. A harder one: replay a single real pass, second by second.")
        self.play(FadeIn(head, shift=UP * 0.1))
        self.read()
        f = ChartFrame(x=(-3, 51), y=(5.2, 8.0), invert_y=True, width=10.5, height=2.5, at=DOWN * 0.35 + LEFT * 0.4,
                       x_label="seconds", y_label="magnitude")
        f.add_ticks(x=range(0, 51, 10), y=[5.5, 6.5, 7.5], y_fmt="{:.1f}")
        self.play(FadeIn(f))
        dots = VGroup(*[Dot(f.p(t, v), radius=0.03, color=theme.SKY, fill_opacity=0.8)
                        for t, v in zip(P["t_obs"], P["v_obs"])])
        self.say("Each dot is one frame from the camera. The satellite slowly brightens as it comes closer...")
        self.play(LaggedStart(*[FadeIn(x) for x in dots], lag_ratio=0.004), run_time=3.0)
        self.read()
        tf = float(P["t_obs"][np.argmin(P["v_obs"])])
        ring = Circle(radius=0.45, color=theme.ACCENT, stroke_width=4).move_to(f.p(tf, 5.75))
        fl = text("a flare: 1.3 magnitudes, 1.5 s", 22, theme.ACCENT).next_to(ring, UP, buff=0.08)
        self.play(Create(ring), FadeIn(fl))
        self.say("...then flares: 3.5 times brighter for a second and a half (Mallama, 2021).")
        self.read()
        self.say("We put the same satellite into the simulator: its real orbit, seen from the same place.")
        self.read(0.6)
        self.clear_caption()
        self.play(*[FadeOut(m) for m in self.mobjects])


class PassCompare(ThemedScene):
    section = (9, "Does it match the sky?")

    def construct(self):
        P = load_pass()
        f = ChartFrame(x=(-3, 51), y=(5.0, 8.0), invert_y=True, width=10.5, height=2.2, at=UP * 1.35 + RIGHT * 0.3,
                       y_label="magnitude")
        f.add_ticks(y=[5.5, 6.5, 7.5], y_fmt="{:.1f}")
        dots = VGroup(*[Dot(f.p(t, v), radius=0.025, color=theme.SKY, fill_opacity=0.45)
                        for t, v in zip(P["t_obs"], P["v_obs"])])
        ok = np.isfinite(P["v_med"])
        med = VGroup(*[f.polyline(P["t_med"][seg], P["v_med"][seg], theme.SKY, 4) for seg in runs(ok)])
        sim_sel = (P["t_sim"] >= -3) & (P["t_sim"] <= 51)
        sim = f.polyline(P["t_sim"][sim_sel], P["v_sim"][sim_sel], theme.TEAL, 5)
        key = VGroup(VGroup(Line(ORIGIN, RIGHT * 0.5, color=theme.SKY, stroke_width=4),
                            text("observed (1-second median)", 20)).arrange(RIGHT, buff=0.15),
                     VGroup(Line(ORIGIN, RIGHT * 0.5, color=theme.TEAL, stroke_width=5),
                            text("simulator", 20)).arrange(RIGHT, buff=0.15)).arrange(RIGHT, buff=0.6).next_to(f, UP, buff=0.15)
        self.play(FadeIn(f), FadeIn(dots), FadeIn(key))
        self.play(Create(med), run_time=1.5)
        self.say("Blue: what the camera recorded. Green: what the simulator predicts for the same moments.")
        self.play(Create(sim), run_time=2.0)
        self.read()

        # The difference, simulator minus observed, on the 1-second medians.
        sim_at = np.interp(P["t_med"], P["t_sim"], P["v_sim"])
        diff = sim_at - P["v_med"]
        g = ChartFrame(x=(-3, 51), y=(-2.0, 1.0), invert_y=True, width=10.5, height=1.3, at=DOWN * 0.95 + RIGHT * 0.3,
                       x_label="seconds", y_label="sim − obs")
        g.add_ticks(x=range(0, 51, 10), y=[-1.5, -0.5, 0.5], y_fmt="{:+.1f}")
        zero = Line(g.p(-3, 0), g.p(51, 0), color=theme.MUTED, stroke_width=1.5)
        band = Rectangle(width=g.w, height=abs(g.p(0, 0.3)[1] - g.p(0, -0.3)[1]), stroke_width=0, fill_color=theme.TEAL,
                         fill_opacity=0.12).move_to(g.p(24, 0))
        dl = VGroup(*[g.polyline(P["t_med"][seg], diff[seg], theme.SUN, 3) for seg in runs(ok)])
        self.play(FadeIn(g), FadeIn(band), Create(zero))
        self.play(Create(dl), run_time=1.5)
        tf = float(P["t_med"][np.nanargmin(np.where(ok, P["v_med"], 99))])
        quiet = ok & (np.abs(P["t_med"] - tf) > 2.5)
        bias = float(np.nanmean(diff[quiet]))
        self.say(f"Away from the flare the simulator is about {abs(bias):.1f} magnitudes "
                 f"{'brighter' if bias < 0 else 'fainter'}, and brightens a little faster as it approaches.")
        self.read()
        ring = Circle(radius=0.4, color=theme.ACCENT, stroke_width=4).move_to(f.p(tf, 5.75))
        self.play(Create(ring))
        self.say("But the flare is missing: the simulator predicts no flash at that moment. Why?")
        self.read(1.0)
        self.clear_caption()
        self.play(*[FadeOut(m) for m in self.mobjects])


class PassMap(ThemedScene):
    """The pass on the satellite's brightness map: the observer's path never comes near a glint."""
    section = (9, "Does it match the sky?")

    def construct(self):
        P = load_pass()
        S = P["samples"]
        d = dump("starlink_visorsat")
        lv = d.levels[0].raw
        mats = [d.materials[d.tri_material[L["tris"][0]]]["name"] for L in lv]
        colour = {"bus_metal": theme.TEXT, "antenna_white": theme.cat(4), "bus_edge_black": theme.cat(3),
                  "visor_black": theme.VIOLET, "solar_cell": theme.SKY, "array_back_dark": theme.cat(2)}
        cols = [colour.get(m, theme.MUTED) for m in mats]
        k = int(np.argmin(np.abs(P["t_sim"] - 24)))
        sun = np.array(S[k]["sun_body"])
        normals = [np.array(n) for n in S[k]["lobe_normals_body"]]
        bm = BrightnessMap(lv, width=7.6, decades=5)
        mg = bm.draw(sun, at=UP * 0.85, normals=normals, lobe_colors=cols)
        up = text("toward the Earth", 18, theme.MUTED).next_to(mg, UP, buff=0.08).align_to(mg, LEFT)
        self.say("The VisorSat's brightness map in its published orientation: body facing the Earth.")
        self.play(FadeIn(mg), FadeIn(up))
        self.read()
        sel = (P["t_sim"] >= P["t_obs"][0]) & (P["t_sim"] <= P["t_obs"][-1])
        dirs = np.array([s["obs_body"] for s, m in zip(S, sel) if m])
        path = path_segments(bm, dirs, theme.SKY, 4)
        self.play(Create(path), run_time=2.5)
        a = Dot(bm.point(dirs[0]), radius=0.06, color=theme.SKY)
        b = Dot(bm.point(dirs[-1]), radius=0.06, color=theme.SKY)
        pl = text("blue: the observer's path during the pass", 20, theme.SKY).next_to(mg, DOWN, buff=0.15)
        self.play(FadeIn(a), FadeIn(b), FadeIn(pl))
        # The nearest glint at the flare's moment.
        tf = float(P["t_obs"][np.argmin(P["v_obs"])])
        kf = int(np.argmin(np.abs(P["t_sim"] - tf)))
        o = np.array(S[kf]["obs_body"])
        gl = bm.glints(np.array(S[kf]["sun_body"]), [np.array(n) for n in S[kf]["lobe_normals_body"]])
        best = min(((float(np.degrees(np.arccos(np.clip(r @ o, -1, 1)))), r) for r, _, _ in gl), key=lambda x: x[0])
        line = Line(bm.point(o), bm.point(best[1]), color=theme.ACCENT, stroke_width=3)
        lab = text(f"red: {best[0]:.0f}° to the nearest glint", 20, theme.ACCENT).next_to(pl, DOWN, buff=0.08)
        self.play(Create(line), FadeIn(lab))
        self.say(f"The path stays {best[0]:.0f} degrees from the nearest glint: in this orientation, nothing can flash towards this observer.")
        self.read()
        self.say("To flash here, a surface must be turned about 37 degrees from that, and be mirror-smooth to flash so briefly.")
        self.read()
        self.say("Shape and materials set the steady brightness. Flares depend on its exact orientation, which is not published.")
        self.read(1.0)
        self.clear_caption()
        self.play(*[FadeOut(m) for m in self.mobjects])


class PassOthers(ThemedScene):
    """Two more observed passes: the same picture."""
    section = (9, "Does it match the sky?")

    def construct(self):
        ids = [("mmt_45713_19105664", "STARLINK-1436 (VisorSat), 23 April 2021"),
               ("mmt_44714_18902994", "STARLINK-1008 (original design), 31 December 2020")]
        charts = VGroup()
        for i, (pid, title) in enumerate(ids):
            P = load_pass(pid)
            x1 = float(P["t_obs"][-1]) + 1
            win = (P["t_sim"] >= -1) & (P["t_sim"] <= x1)
            lo = min(np.nanmin(P["v_obs"]), np.nanmin(P["v_sim"][win])) - 0.25
            hi = max(np.nanmax(P["v_obs"]), np.nanmax(P["v_sim"][win])) + 0.25
            f = ChartFrame(x=(-1, x1), y=(lo, hi), invert_y=True, width=5.2, height=2.6,
                           at=(LEFT * 3.3 if i == 0 else RIGHT * 3.5) + UP * 0.45, x_label="seconds")
            f.add_ticks(x=range(0, int(x1), 10 if x1 > 30 else 5),
                        y=[round(v, 1) for v in np.linspace(lo + 0.2, hi - 0.2, 3)], y_fmt="{:.1f}")
            dots = VGroup(*[Dot(f.p(t, v), radius=0.02, color=theme.SKY, fill_opacity=0.5)
                            for t, v in zip(P["t_obs"], P["v_obs"])])
            sim = f.polyline(P["t_sim"][win], P["v_sim"][win], theme.TEAL, 4)
            charts.add(VGroup(f, dots, sim, text(title, 20).next_to(f, UP, buff=0.2)))
        self.say("Two more passes, the same story: the level within a few tenths to a magnitude, the flares missed.")
        self.play(LaggedStart(*[FadeIn(c) for c in charts], lag_ratio=0.4), run_time=2.0)
        self.read(1.5)
        self.clear_caption()
        self.play(*[FadeOut(m) for m in self.mobjects])
