#!/usr/bin/env python3
"""Low-resolution UI check (2026-10-21): every window and HUD element at several window sizes.

    python tools/harness/ui_lowres.py run [--sizes 1024x768,1280x720,...] [--out DIR] [--ui-scale S] [--no-auto]
    python tools/harness/ui_lowres.py check <run dir>      # re-check a finished run (no launch)
    python tools/harness/ui_lowres.py sheets <run dir>     # contact sheets per size (needs Pillow)
    python tools/harness/ui_lowres.py gen [--sizes ...]    # print the .satcmd

`run` writes a .satcmd (ONE app: `window WxH` per size; every window opened alone at its default for that size via
`ui reset all`, then `ui dump` + `capture ui=on`), runs it through run.py (the launch gate spaces launches), then
`check`s it. Exit status 1 on any failure. docs/HARNESS.md "Low-resolution UI check".

Checks, per dump:
  * every window (`ui dump`'s `windows`) lies inside the screen;
  * each window covers at most MAX_COVER of the screen's area by default (Settings: MAX_COVER_BIG);
  * no drawn text is cut sideways by its scissor (a cut at a scroll view's top / bottom edge is allowed), lies off
    the screen, or overlaps other text under the same scissor (`ui dump`'s own checks);
  * the HUD elements in HUD_IDS (bottom panels, selection chip, follow bar, "Go to again" chip) do not overlap each
    other and stay on screen.
Standard library only, except `sheets`.
"""
import argparse
import glob
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# 1024x768 / 1280x720 / 1366x768: the low resolutions; 1600x900 and 1920x1009 (a maximized window on a 1080p desktop)
# the reference. A size larger than the desktop cannot be reached (`window` fails): reported as skipped, not checked.
DEFAULT_SIZES = ["1024x768", "1280x720", "1366x768", "1600x900", "1920x1009"]
MAX_COVER = 0.62      # a window at its default may cover at most this share of the screen
MAX_COVER_BIG = 0.80  # the Settings window (a centred, modal-ish panel)
WIN_NAMES = {0: "settings", 1: "controls", 2: "trace", 3: "viewer", 4: "info", 5: "cine", 6: "bookmarks", 7: "reports",
             8: "missions", 12: "weather", 13: "skyevents"}

# HUD elements that must not overlap each other (the bottom panels, the out-of-view selection chip, follow mode's bar,
# the "Go to ... again" chip, the cinematic bar)
HUD_IDS = ("LeftPanel", "RightPanel", "HudPopupPanel", "SelSatChip", "FollowAgainBtn", "FollowHud", "CineHud")

SETTINGS_TABS = ["Display", "Controls", "Sound", "Constellations", "Photometry", "Clouds", "Weather",
                 "Atmosphere", "Terrain", "Night lights", "Ocean", "Beams", "Performance", "Attributions", "Launches", "Sky"]
ISS = 'select const "International Space Station"'


def views():
    """(name, commands before the dump, commands after) for each view, in order."""
    v = [("hud", [], []), ("hud_sel", [ISS, "wait 5"], [])]
    # The right HUD's Movement / View popups (2026-10-29): open over their buttons, closed again by the same button.
    v.append(("hud_movepop", ["ui click HudMoveBtn", "wait 5"], ["ui click HudMoveBtn", "wait 2"]))
    v.append(("hud_viewpop", ["ui click HudViewBtn", "wait 5"], ["ui click HudViewBtn", "wait 2"]))
    for t in SETTINGS_TABS:
        v.append(("set_" + t.lower().replace(" ", "").replace("&", "and"), [f'ui open settings tab="{t}"', "wait 4"], ["ui close settings"]))
    # Settings search (2026-10-28): a query listing rows from many tabs.
    v.append(("set_search", ['ui open settings tab="Display"', "ui click SettingsSearch", "ui type cloud", "wait 4"],
              ["ui key esc", "ui close settings"]))
    for t in ("upcoming", "ongoing", "kessler"):
        v.append(("ms_" + t, [f"ui open missions tab={t}", "wait 12"], ["ui close missions"]))
    for t in ("radar", "storms", "cyclones", "controls"):
        v.append(("wx_" + t, [f"ui open weather tab={t}", "wait 12"], ["ui close weather"]))
    v.append(("skyevents", ["ui open skyevents", "skyevents events", "wait 12"], ["ui close skyevents"]))
    v += [
        ("info", [ISS, "ui open info", "wait 12"], ["ui close info"]),
        ("viewer", [ISS, "ui open viewer", "wait 12"], ["ui close viewer"]),
        ("info_viewer", [ISS, "ui open info", "ui open viewer", "wait 12"], ["ui close all"]),
        ("trace", [ISS, "ui open trace", "wait 8"], ["ui close trace"]),
        ("bookmarks", ["ui open bookmarks", "wait 8"], ["ui close bookmarks"]),
        ("cine", ["ui open cine", "wait 4"], ["ui close cine"]),
        ("reports", ["ui open reports", "wait 4"], ["ui close reports"]),
        ("viewcontrols", ["ui open viewcontrols", "wait 4"], ["ui close viewcontrols"]),
        ("follow", [ISS, "follow", "wait 20"], ["follow off", "wait 5"]),
        ("tut_look", ["tutorial start 1", "wait 4"], []),
        ("tut_select", ["tutorial step select", "wait 4"], []),
        ("tut_info", [ISS, "tutorial step info", "wait 4"], []),
        ("tut_goto", [ISS, "tutorial step goto", "follow", "wait 20"], ["follow off", "wait 5"]),
        ("tut_trace", [ISS, "tutorial step trace", "wait 4"], []),
        ("tut_time", ["tutorial step time", "wait 4"], []),
        ("tut_missions", ["tutorial step missions", "ui open missions tab=upcoming", "wait 12"], ["ui close missions"]),
        ("tut_ongoing", ["tutorial step ongoing", "ui open missions tab=ongoing", "wait 12"], ["ui close missions"]),
        ("tut_settings", ["tutorial step settings", "wait 4"], ["tutorial skip", "wait 2"]),
    ]
    return v


def gen_script(sizes, ui_scale=None, auto=True, only=None):
    L = ["# generated by tools/harness/ui_lowres.py: every window and HUD element at each window size",
         "preset High", "observer lat=34.05 lon=-118.25 agl=10", "time set 2036-11-22T02:30:00Z", "time pause",
         "camera az=270 el=15 fov=70", "set clouds_v2.coverage 0", "wait settle 20"]
    if ui_scale or not auto:
        L.append(f"ui scale {ui_scale or 1.5} auto={1 if auto else 0}")
    for s in sizes:
        L += [f"# -- {s} --", f"window {s}", "wait 10", "ui close all", "select none", "ui show", "wait 3"]
        for name, pre, post in views():
            if only and not any(name.startswith(o) for o in only):
                continue
            tag = f"{s}_{name}"
            L += ["ui reset all"] + pre + [f"ui dump {tag}", f"capture {tag} ui=on"] + post
        L += ["ui close all", "select none"]
    return "\n".join(L) + "\n"


# Known benign findings: (view-name substring, text substring). Keep this short and explain each.
ALLOW = []


def rect_in(a, b, tol=1.0):
    return a[0] >= b[0] - tol and a[1] >= b[1] - tol and a[0] + a[2] <= b[0] + b[2] + tol and a[1] + a[3] <= b[1] + b[3] + tol


def overlap(a, b):
    ox = min(a[0] + a[2], b[0] + b[2]) - max(a[0], b[0])
    oy = min(a[1] + a[3], b[1] + b[3]) - max(a[1], b[1])
    return ox > 1 and oy > 1


def hidden(e):
    b, c = e["box"], e["clip"]
    return b[0] >= c[0] + c[2] or b[1] >= c[1] + c[3] or b[0] + b[2] <= c[0] or b[1] + b[3] <= c[1]


def allowed(name, text):
    return any(v in name and s in text for v, s in ALLOW)


def check_dump(path):
    d = json.load(open(path))
    W, H = d["width"], d["height"]
    name = os.path.basename(path).replace(".layout.json", "")
    out = {"view": name, "size": f"{int(W)}x{int(H)}", "ui_scale": d.get("ui_scale"), "fail": [], "windows": {}}
    want = name.split("_")[0]
    if "x" in want and want != out["size"]:
        out["skipped"] = f"window size {want} not reached (stayed {out['size']}: larger than the desktop?)"
        return out
    wl = d.get("windows")
    if wl is None:  # a dump from before 2026-10-21: the window rects without their ids
        wl = [{"win": -1, "box": it["box"], "cover": it["box"][2] * it["box"][3] / (W * H),
               "inside": rect_in(it["box"], [0, 0, W, H], 0.5)}
              for it in d["items"] if it.get("id") == "GenWin" and it["kind"] == "rect"]
        d["windows"] = wl
    for w in wl:
        b, k = w["box"], WIN_NAMES.get(w["win"], str(w["win"]))
        out["windows"][k] = {"box": [round(x) for x in b], "cover": round(w["cover"], 3),
                             "w_frac": round(b[2] / W, 3), "h_frac": round(b[3] / H, 3)}
        if not w["inside"]:
            out["fail"].append(f"window {k} {[round(x) for x in b]} exceeds the {int(W)}x{int(H)} screen")
        lim = MAX_COVER_BIG if w["win"] == 0 else MAX_COVER
        if w["cover"] > lim + 1e-3:
            out["fail"].append(f"window {k} covers {w['cover']:.0%} of the screen (max {lim:.0%})")
    for e in d.get("text_clipped", []):
        if e.get("hidden", hidden(e)) or allowed(name, e["text"]):
            continue  # wholly outside its scissor: not drawn (a scroll view's rows past its edge)
        b, c = e["box"], e["clip"]
        if b[0] >= c[0] - 0.5 and b[0] + b[2] <= c[0] + c[2] + 0.5:
            continue  # cut at the top / bottom only: the row at a scroll view's edge
        out["fail"].append(f"text clipped: '{e['text'][:60]}' {[round(x) for x in b]} clip {[round(x) for x in c]}")
    hid = {e["i"] for e in d.get("text_clipped", []) if e.get("hidden", hidden(e))}
    for e in d.get("text_offscreen", []):
        if e.get("i") in hid:
            continue  # (a dump from before 2026-10-21 listed scrolled-away rows as off screen)
        if not allowed(name, e["text"]):
            out["fail"].append(f"text offscreen: '{e['text'][:60]}' {[round(x) for x in e['box']]}")
    # which window (if any) a text belongs to: the last window rect drawn before it that contains it
    wins = [(it["i"], it["box"]) for it in d["items"] if it.get("id") == "GenWin" and it["kind"] == "rect"]
    idx = {(it.get("text"), tuple(round(x) for x in it["box"])): it["i"] for it in d["items"] if it["kind"] == "text"}

    def owner(text, box):
        i = idx.get((text, tuple(round(x) for x in box)), -1)
        own = -1
        for wi, wb in wins:
            if wi < i and rect_in(box, wb):
                own = wi
        return own

    for e in d.get("text_overlaps", []):
        t = e.get("a", "") + " / " + e.get("b", "")
        if owner(e.get("a"), e["a_box"]) != owner(e.get("b"), e["b_box"]):
            continue  # in different windows, or a window over the HUD: layering, not layout
        if not allowed(name, t):
            out["fail"].append(f"text overlap: '{t[:70]}' {[round(x) for x in e['a_box']]}")
    boxes = {it.get("id"): it["box"] for it in d["items"] if it.get("id") in HUD_IDS and it["kind"] == "rect"}
    ks = sorted(boxes)
    for i in range(len(ks)):
        for j in range(i + 1, len(ks)):
            if overlap(boxes[ks[i]], boxes[ks[j]]):
                out["fail"].append(f"HUD {ks[i]} overlaps {ks[j]}")
    for k in ks:
        if k in boxes and not rect_in(boxes[k], [0, 0, W, H]):
            out["fail"].append(f"HUD {k} {[round(x) for x in boxes[k]]} is off screen")
    return out


def cmd_check(a):
    dumps = sorted(glob.glob(os.path.join(a.run, "captures", "*.layout.json")))
    if not dumps:
        sys.exit(f"no layout dumps in {a.run}/captures")
    res = [check_dump(p) for p in dumps]
    nfail = nskip = 0
    for r in res:
        if r.get("skipped"):
            nskip += 1
            print(f"skip {r['view']:<34} {r['skipped']}")
            continue
        cov = ", ".join(f"{k} {v['cover']:.0%} ({v['w_frac']:.0%} x {v['h_frac']:.0%})" for k, v in r["windows"].items())
        print(f"{'FAIL' if r['fail'] else 'ok  '} {r['view']:<34} scale={r['ui_scale']:.2f} {cov}")
        for f in r["fail"]:
            print("       - " + f)
        nfail += bool(r["fail"])
    json.dump(res, open(os.path.join(a.run, "ui_lowres_report.json"), "w"), indent=1)
    print(f"\n{len(res)} views, {nfail} with failures, {nskip} skipped -> {os.path.join(a.run, 'ui_lowres_report.json')}")
    return 1 if nfail else 0


def cmd_sheets(a):
    caps = glob.glob(os.path.join(a.run, "captures", "*.png"))
    sizes = sorted({os.path.basename(p).split("_")[0] for p in caps if "x" in os.path.basename(p).split("_")[0]})
    for s in sizes:
        ps = sorted(p for p in caps if os.path.basename(p).startswith(s + "_"))
        o = os.path.join(a.run, f"sheet_{s}.png")
        subprocess.call([sys.executable, os.path.join(HERE, "imgtools.py"), "sheet", *ps, "-o", o, "--cols", "6",
                         "--width", str(a.width)])
    return 0


def cmd_run(a):
    sizes = a.sizes.split(",")
    out = a.out or os.path.join("harness_runs", "ui_lowres")
    os.makedirs(out, exist_ok=True)
    script = os.path.join(out, "ui_lowres.satcmd")
    with open(script, "w", encoding="utf-8") as f:
        f.write(gen_script(sizes, a.ui_scale, not a.no_auto, a.only.split(",") if a.only else None))
    rc = subprocess.call([sys.executable, os.path.join(HERE, "run.py"), script, "--out", os.path.join(out, "run"),
                          "--window", sizes[0], "--quiet", "--timeout", "1800"] + (["--exe", a.exe] if a.exe else []))
    a.run = os.path.join(out, "run")
    return cmd_check(a) or (1 if rc else 0)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = p.add_subparsers(dest="cmd", required=True)
    r = sp.add_parser("run")
    r.add_argument("--sizes", default=",".join(DEFAULT_SIZES))
    r.add_argument("--out")
    r.add_argument("--ui-scale", type=float, help="the UI scale SETTING (default: the app's, 1.5)")
    r.add_argument("--no-auto", action="store_true", help="switch off the window-height factor (display.ui_auto_scale)")
    r.add_argument("--exe", help="another build's exe (a before / after comparison)")
    r.add_argument("--only", help="comma-separated view-name prefixes (e.g. wx_,ms_): only those views")
    c = sp.add_parser("check")
    c.add_argument("run")
    s = sp.add_parser("sheets")
    s.add_argument("run")
    s.add_argument("--width", type=int, default=420)
    g = sp.add_parser("gen")
    g.add_argument("--sizes", default=",".join(DEFAULT_SIZES))
    a = p.parse_args()
    if a.cmd == "gen":
        print(gen_script(a.sizes.split(",")), end="")
        return 0
    return {"run": cmd_run, "check": cmd_check, "sheets": cmd_sheets}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
