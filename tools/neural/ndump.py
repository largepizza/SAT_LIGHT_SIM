"""Read and convert the neural-light feature dumps (harness `ndump write`, the -DCV2_NDUMP march variant).

A dump is captures/<name>.f32 (h x w x k float32, row-major; k = 56 since 2026-11-12, 48 before) + <name>.json (shape, channel names, the state).
Channels 0-14 are the lighting TARGETS summed over `count` visits (divide by channel 14); 15-47 are the last visit's
FEATURES. The layout is the shader's (cloud_v2_march.comp, CV2_NDUMP) and the harness's (`layout` in the .json).

    python tools/neural/ndump.py info  captures/x.f32          # shape, visits, per-channel ranges
    python tools/neural/ndump.py pack  captures/x.f32 -o data/x.npz [--delete]   # fp16 npz: targets averaged
    python tools/neural/ndump.py sheet captures/x.f32 -o x.png  # a contact sheet of the channels (needs pillow)

Stdlib + numpy (the pack); pillow for the sheet.
"""
import argparse, json, os, sys
import numpy as np

K = 56                  # the current record width; a dump's own k (meta) is what the reader uses
TARGET_CH = 15          # 0..14 summed over visits
COUNT_CH = 14

def load(path):
    """-> (arr h x w x k float32, meta dict). Targets are returned SUMMED (as written)."""
    meta_path = os.path.splitext(path)[0] + ".json"
    with open(meta_path, encoding="utf-8") as f:
        meta = json.load(f)
    w, h, k = int(meta["w"]), int(meta["h"]), int(meta["k"])
    a = np.fromfile(path, dtype=np.float32)
    if a.size != w * h * k:
        sys.exit(f"{path}: {a.size} floats, expected {w}x{h}x{k}")
    return a.reshape(h, w, k), meta

def averaged(a):
    """Targets divided by the visit count (0 where never visited); features untouched."""
    out = a.copy()
    cnt = np.maximum(a[..., COUNT_CH:COUNT_CH + 1], 1.0)
    out[..., :COUNT_CH] = a[..., :COUNT_CH] / cnt
    return out

def state_summary(meta):
    """The few state fields the dataset is stratified on (the full state stays in the .json)."""
    st = meta.get("state", {}) or {}
    def g(*ks, default=None):
        d = st
        for k in ks:
            if not isinstance(d, dict) or k not in d:
                return default
            d = d[k]
        return d
    return {
        "sun_el_deg": g("sun", "elevation_deg", default=g("sun", "el_deg")),
        "moon_el_deg": g("moon", "elevation_deg", default=g("moon", "el_deg")),
        "eye_h_m": g("observer", "height_m", default=g("observer", "alt_m")),
        "lat": g("observer", "lat_deg", default=g("observer", "lat")),
        "lon": g("observer", "lon_deg", default=g("observer", "lon")),
        "cam_az": g("camera", "az_deg"), "cam_el": g("camera", "el_deg"), "fov": g("camera", "fov_y_deg"),
        "time": g("time", "iso", default=g("time", "utc")),
    }

def cmd_info(a):
    arr, meta = load(a.path)
    av = averaged(arr)
    names = meta["layout"]
    cnt = arr[..., COUNT_CH]
    print(f"{a.path}: {arr.shape[1]}x{arr.shape[0]} x {K}, frames {meta.get('frames')}, visits mean {cnt.mean():.1f} "
          f"min {cnt.min():.0f} max {cnt.max():.0f}; cloud pixels (w_acc > 1e-4): {(av[..., 23] > 1e-4).mean() * 100:.1f}%")
    print(json.dumps(state_summary(meta)))
    for i, n in enumerate(names):
        c = av[..., i]
        print(f"  {i:2d} {n:12s} min {c.min():12.5g} mean {c.mean():12.5g} max {c.max():12.5g}")

def cmd_pack(a):
    arr, meta = load(a.path)
    av32 = averaged(arr)
    # Distances (m) overflow fp16 past 65 km: the t channels go in km, heights too, sigma in /km. Indices from the layout.
    names = meta["layout"]
    for i, n in enumerate(names):
        if n in ("mean_t", "t_first", "t_od025", "t_od1", "t_od4", "t_od10", "t_last", "t_half", "x_t"):
            av32[..., i] = np.where(av32[..., i] >= 0.0, av32[..., i] * 1e-3, -1.0)
        elif n in ("x_h", "x_top_h"):
            av32[..., i] *= 1e-3
        elif n == "x_sigma":
            av32[..., i] *= 1e3
        elif n == "x_fp":
            av32[..., i] = np.minimum(av32[..., i], 6e4)   # metres; fp16 tops out at 65504 (a 900-km orbit view exceeds it)
        elif n in ("out_r", "out_g", "out_b", "count"):
            av32[..., i] = 0.0      # not trained on (the march's total colour includes fog and shafts; the count is in meta): compress away
    av = av32.astype(np.float16)
    meta_small = {k: v for k, v in meta.items() if k != "state"}
    meta_small["summary"] = state_summary(meta)
    meta_small["units"] = "t/h in km, sigma /km, targets averaged over visits"
    out = a.out or os.path.splitext(a.path)[0] + ".npz"
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    np.savez_compressed(out, data=av, meta=json.dumps(meta_small), state=json.dumps(meta.get("state", {})))
    if a.delete:
        os.remove(a.path)
    print(f"packed {out} ({os.path.getsize(out) / 1e6:.1f} MB)")

def cmd_sheet(a):
    from PIL import Image
    arr, meta = load(a.path)
    av = averaged(arr)
    names = meta["layout"]
    h, w = av.shape[:2]
    K = av.shape[2]
    cols = 8
    rows = (K + cols - 1) // cols
    sw, sh = w // a.div, h // a.div
    sheet = Image.new("L", (cols * sw, rows * sh))
    for i in range(K):
        c = av[::a.div, ::a.div, i]
        lo, hi = np.percentile(c, 1), np.percentile(c, 99)
        img = np.clip((c - lo) / max(hi - lo, 1e-9), 0, 1) * 255
        sheet.paste(Image.fromarray(img.astype(np.uint8)), ((i % cols) * sw, (i // cols) * sh))
    sheet.save(a.out)
    print(f"{a.out}: {cols}x{rows} tiles, order = layout; " + ", ".join(f"{i}:{n}" for i, n in enumerate(names)))

def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = p.add_subparsers(dest="cmd", required=True)
    s = sp.add_parser("info"); s.add_argument("path"); s.set_defaults(fn=cmd_info)
    s = sp.add_parser("pack"); s.add_argument("path"); s.add_argument("-o", "--out"); s.add_argument("--delete", action="store_true"); s.set_defaults(fn=cmd_pack)
    s = sp.add_parser("sheet"); s.add_argument("path"); s.add_argument("-o", "--out", required=True); s.add_argument("--div", type=int, default=4); s.set_defaults(fn=cmd_sheet)
    a = p.parse_args()
    a.fn(a)

if __name__ == "__main__":
    main()
