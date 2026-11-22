# A contact sheet of lightning channels from BoltTool dumps, without the app (issue_20261121_163317).
#   python tools/bolt_tool/sheet.py [--exe build/Release/BoltTool.exe] [--seeds 7,21,33,58] [--target 12] -o sheet.png
# Each tile is one tree seen from the side (a ground stroke: across its slant) and, for the sheets, from below.
# Needs Pillow (tools/harness/.venv has it).
import argparse, json, math, os, subprocess
from PIL import Image, ImageDraw

def dump(exe, kind, seed, target, extra):
    out = subprocess.run([os.path.abspath(exe), "dump", f"kind={kind}", f"seed={seed}", f"target={target}"] + extra,
                         capture_output=True, text=True, check=True).stdout
    return json.loads(out)

def tile(d, view, size=420):
    segs = d["segs"]
    im = Image.new("RGB", (size, size), (8, 9, 14))
    if not segs:
        return im
    def proj(p):
        x, y, z = p
        if view == "below":
            return x, -y
        if view == "side":   # across the horizontal direction of the tree's longest extent
            return x * view_c + y * view_s, -z
        return x, -z
    # the side view's axis: along the largest horizontal spread
    sx = [s[0] for s in segs] + [s[3] for s in segs]
    sy = [s[1] for s in segs] + [s[4] for s in segs]
    ang = math.atan2(max(sy) - min(sy), max(sx) - min(sx)) if (max(sy) - min(sy)) > (max(sx) - min(sx)) else 0.0
    global view_c, view_s
    if d["kind"] == 1:
        gx, gy = segs[-1][3], segs[-1][4]
        # a ground stroke: the trunk's last piece is not last in the list; use the lowest point
        low = min(segs, key=lambda s: s[5])
        ang = math.atan2(low[4], low[3])
    view_c, view_s = math.cos(ang), math.sin(ang)
    pts = [proj(s[0:3]) for s in segs] + [proj(s[3:6]) for s in segs]
    x0, x1 = min(p[0] for p in pts), max(p[0] for p in pts)
    y0, y1 = min(p[1] for p in pts), max(p[1] for p in pts)
    sc = (size - 24) / max(x1 - x0, y1 - y0, 1.0)
    ox = 12 + 0.5 * ((size - 24) - sc * (x1 - x0))
    oy = 12 + 0.5 * ((size - 24) - sc * (y1 - y0))
    dr = ImageDraw.Draw(im)
    for order in (3, 2, 1, 0):
        for s in segs:
            if s[7] != order:
                continue
            a, b = proj(s[0:3]), proj(s[3:6])
            w = min(1.0, (0.35 + 0.9 * s[6]) * (1.0 if order == 0 else 0.9))
            c = (int(200 * w + 30), int(210 * w + 30), int(255 * w))
            dr.line([(ox + sc * (a[0] - x0), oy + sc * (a[1] - y0)), (ox + sc * (b[0] - x0), oy + sc * (b[1] - y0))],
                    fill=c, width=3 if order == 0 else (2 if order == 1 else 1))
    span = max(x1 - x0, y1 - y0)
    dr.text((6, 4), f"{['ic','cg','sprite','spider'][d['kind']]} s{d['seed']} {view}  {span/1000:.1f} km  side {d['side_branches']}  "
                    f"v {d['verts']}", fill=(150, 150, 150))
    return im

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default="build/Release/BoltTool.exe")
    ap.add_argument("--seeds", default="7,21,33,58")
    ap.add_argument("--target", default="12")
    ap.add_argument("--kinds", default="cg,spider,ic")
    ap.add_argument("--extra", default="")
    ap.add_argument("-o", "--out", default="bolt_sheet.png")
    a = ap.parse_args()
    seeds = [int(s) for s in a.seeds.split(",")]
    rows = []
    for kind in a.kinds.split(","):
        views = ["side"] if kind == "cg" else ["below", "side"]
        for view in views:
            rows.append([tile(dump(a.exe, kind, s, a.target, a.extra.split()), view) for s in seeds])
    W = sum(t.size[0] for t in rows[0]) + 4 * (len(rows[0]) + 1)
    H = sum(r[0].size[1] for r in rows) + 4 * (len(rows) + 1)
    sheet = Image.new("RGB", (W, H), (30, 30, 30))
    y = 4
    for r in rows:
        x = 4
        for t in r:
            sheet.paste(t, (x, y))
            x += t.size[0] + 4
        y += r[0].size[1] + 4
    sheet.save(a.out)
    print(a.out, sheet.size)

if __name__ == "__main__":
    main()
