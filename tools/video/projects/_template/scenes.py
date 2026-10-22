"""Template scenes. One class per timeline "scene" entry; render with
    python -m vidkit render <project> Hello
Data written by the project's `data` steps is under vidkit.project_data().
"""
from manim import DOWN, UP, FadeIn, FadeOut, ValueTracker, VGroup, Write, always_redraw, linear

import vidkit
from vidkit import theme
from vidkit.data import LobeDump
from vidkit.theme import ThemedScene, text


class Hello(ThemedScene):
    section = (1, "Template")

    def construct(self):
        t = theme.title("A technical video")
        sub = text("vidkit: Manim scenes + engine clips + ffmpeg", 28, theme.MUTED)
        VGroup(t, sub).arrange(DOWN, buff=0.4)
        self.play(Write(t), FadeIn(sub, shift=UP * 0.2))
        self.say("Captions go along the bottom with self.say().")

        dump = vidkit.project_data() / "lobes" / "starlink_v2_mini.lobes.json"
        if dump.exists():
            d = LobeDump(dump)
            yaw = ValueTracker(0)
            mesh = always_redraw(lambda: d.mesh.draw(d.material_colors(), yaw=yaw.get_value(), size=3.5, at=DOWN * 0.8))
            self.play(FadeOut(VGroup(t, sub)))
            self.add(mesh)
            self.say(f"{d.name}: {len(d.area)} facets, drawn from SatModelTool's dump.")
            self.play(yaw.animate(rate_func=linear).set_value(180), run_time=4)
        self.wait(1)
