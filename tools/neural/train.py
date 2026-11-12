"""Train and evaluate the neural cloud-lighting net on the collected dumps (tools/neural/collect.py -> data/views/*.npz).

The net maps per-pixel primary-ray FEATURES (channels 15-47 of the record: the ray's optical depth and crossings, and
the interaction sample's one light march, far side, sky probe, Sun-volume transmittance, medium and geometry) to the
march's lighting TARGETS: the key light and the sky ambient, each normalised by its illuminant colour at the
interaction sample (the paper's linearity in light: I = key_col * I_l + sky_col * I_e). The rest (bounce, night, beams)
stays marched. Inputs and outputs are the march's own grid (half res).

    .venv/Scripts/python tools/neural/train.py stats  data/views            # channel ranges -> normalisation sanity
    .venv/Scripts/python tools/neural/train.py train  data/views --out runs/u32 --steps 20000
    .venv/Scripts/python tools/neural/train.py eval   runs/u32/best.pt data/views --sheet runs/u32/val.png
    .venv/Scripts/python tools/neural/train.py bench  runs/u32/best.pt --size 800x450   # inference time (upper bound)

Paths are relative to tools/neural. Run with the venv's python (torch).
"""
import argparse, glob, json, math, os, random, sys, time
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))

# ───────────────────────── the record ─────────────────────────
L = ["key_r", "key_g", "key_b", "amb_r", "amb_g", "amb_b", "rest_r", "rest_g", "rest_b", "out_r", "out_g", "out_b", "T",
     "mean_t", "count", "od_total", "t_first", "t_od025", "t_od1", "t_od4", "t_od10", "t_last", "t_half", "w_acc",
     "x_od_light", "x_od_back", "x_od_up", "x_stv_t", "x_h", "x_sigma", "x_thin", "x_rain", "x_ice", "x_hf", "x_cos_k",
     "x_hv", "x_top_h", "x_fp", "x_key_r", "x_key_g", "x_key_b", "x_sky_r", "x_sky_g", "x_sky_b", "x_t", "x_moon_key",
     "x_dayness", "x_T",
     "x_key_s_r", "x_key_s_g", "x_key_s_b", "x_amb_s_r", "x_amb_s_g", "x_amb_s_b", "x_amb_vis", "x_amb_low"]   # 48.. since 2026-11-12
I = {n: i for i, n in enumerate(L)}
EPS = 1e-6

def features(d, meta=None):
    """d: (h, w, 56) float32 (packed units: t/h km, sigma /km) -> (C, h, w) float32 network input.
    meta: the view's packed meta (its `summary` gives the key light's elevation, a constant plane)."""
    def ch(n): return d[..., I[n]]
    def lg(x, s): return np.log1p(np.maximum(x, 0.0)) / s
    def tk(n):                               # a distance in km, -1 = none -> (value, flag)
        t = ch(n); ok = (t >= 0.0).astype(np.float32)
        return lg(t, 5.0) * ok, ok
    cols = []
    cols.append(lg(ch("od_total"), 3.0))
    for n in ("t_first", "t_od025", "t_od1", "t_od4", "t_od10", "t_last", "t_half", "x_t"):
        v, ok = tk(n); cols += [v, ok]
    cols.append(ch("w_acc"))
    for n in ("x_od_light", "x_od_back", "x_od_up"):
        cols.append(lg(ch(n), 3.0))
    cols.append(ch("x_stv_t"))
    cols.append(ch("x_h") / 15.0); cols.append(ch("x_top_h") / 15.0)
    cols.append(lg(ch("x_sigma"), 5.0))
    for n in ("x_thin", "x_rain", "x_ice", "x_hf", "x_cos_k", "x_hv", "x_dayness", "x_moon_key", "x_T"):
        cols.append(ch(n))
    cols.append(lg(np.nan_to_num(ch("x_fp"), posinf=6e4), 8.0))     # metres; fp16 overflowed past 65 km in early packs
    kl = ch("x_key_r") * 0.2126 + ch("x_key_g") * 0.7152 + ch("x_key_b") * 0.0722
    sl = ch("x_sky_r") * 0.2126 + ch("x_sky_g") * 0.7152 + ch("x_sky_b") * 0.0722
    cols.append(lg(kl, 8.0)); cols.append(lg(sl, 8.0))
    cols.append(np.log1p(sl / np.maximum(kl, EPS)) / 4.0)     # sky-to-Sun ratio (twilight, night)
    # The march's own terms at the interaction sample (2026-11-12): the same log space as the targets.
    for n in ("x_key_s_r", "x_key_s_g", "x_key_s_b", "x_amb_s_r", "x_amb_s_g", "x_amb_s_b"):
        cols.append(np.log1p(np.maximum(ch(n), 0.0) * Y_SCALE) / Y_NORM)
    cols.append(ch("x_amb_vis")); cols.append(ch("x_amb_low"))
    # The key light's elevation (2026-11-12): how the light meets the layer is not in any per-sample term.
    sm = (meta or {}).get("summary", {}) if isinstance(meta, dict) else {}
    sun = sm.get("sun_el_deg"); moon = sm.get("moon_el_deg")
    el = (moon if (ch("x_moon_key").mean() > 0.5 and moon is not None) else sun)
    el = 0.0 if el is None else float(el)
    cols.append(np.full(d.shape[:2], np.sin(np.radians(el)), np.float32))
    return np.stack(cols, 0).astype(np.float32)

N_IN = 1 + 16 + 1 + 3 + 1 + 2 + 1 + 9 + 1 + 3 + 8 + 1

def targets(d):
    """-> (6, h, w): the key light and the sky ambient radiance (linear, the march's units: day cloud ~0.3-4)."""
    return d[..., 0:6].transpose(2, 0, 1).astype(np.float32)

# The loss space: a scaled log so that night cloud (1e-4..1e-2) and day cloud (0.1..4) both resolve; ~[0, 1.1].
# (Dividing by the illuminant colour, as the paper does, blew up where the sky colour is ~0 at night: the ambient has
# night terms that are not proportional to it. The net gets the illuminant colours as inputs instead.)
Y_SCALE = 100.0
Y_NORM = math.log1p(400.0)

def enc_y(y):
    return np.log1p(np.maximum(y, 0.0) * Y_SCALE) / Y_NORM

def dec_y_t(p):   # torch; the net's output is unbounded, the radiance is clamped to [0, ~e^15/100]
    import torch
    return torch.expm1(p.clamp(0.0, 2.5) * Y_NORM) / Y_SCALE

# ───────────────────────── data ─────────────────────────
def list_views(data_dir):
    fs = sorted(glob.glob(os.path.join(data_dir, "*.npz")))
    if not fs:
        sys.exit(f"no .npz in {data_dir}")
    return fs

def split(files, val_frac=0.1, seed=0):
    """Held out by SOURCE (the view's base name before the variant index) so perturbed siblings do not leak."""
    import hashlib
    srcs = sorted({os.path.basename(f).split("_")[0] for f in files})
    # A stable choice: a source is validation by its name's hash, so the split holds as the dataset grows.
    def hval(s): return int(hashlib.md5(f"{seed}:{s}".encode()).hexdigest()[:8], 16) / 2**32
    val = {s for s in srcs if hval(s) < val_frac}
    if not val and srcs: val = {srcs[0]}
    tr = [f for f in files if os.path.basename(f).split("_")[0] not in val]
    va = [f for f in files if os.path.basename(f).split("_")[0] in val]
    return tr, va

def load_npz(f):
    z = np.load(f)
    d = z["data"].astype(np.float32)
    if d.shape[-1] < len(L):      # a 48-float view (before 2026-11-12): the march's sample terms are absent -> zeros (a baseline)
        d = np.concatenate([d, np.zeros(d.shape[:2] + (len(L) - d.shape[-1],), np.float32)], -1)
    return d, json.loads(str(z["meta"]))

class Cache:
    """Views decoded once (features + targets), kept in RAM (fp16, ~45 ch x 360k px x 2 B = 32 MB a view)."""
    def __init__(self, files, limit_mb=12000):
        self.items = []
        used = 0
        for f in files:
            try: d, meta = load_npz(f)
            except Exception as e:                      # a view the collector is still writing, or a truncated one
                print(f"skip {os.path.basename(f)}: {e}"); continue
            x = features(d, meta).astype(np.float16); y = enc_y(targets(d)).astype(np.float16)
            m = (d[..., I["w_acc"]] > 1e-4).astype(np.float16)
            self.items.append((x, y, m, os.path.basename(f)))
            used += (x.nbytes + y.nbytes + m.nbytes) / 1e6
            if used > limit_mb:
                print(f"cache limit {limit_mb} MB reached at {len(self.items)} views"); break
    def __len__(self): return len(self.items)

def crops(cache, batch, size, rng):
    import torch
    xs, ys, ms = [], [], []
    for _ in range(batch):
        x, y, m, _ = cache.items[rng.randrange(len(cache))]
        h, w = x.shape[1:]
        # Favour cloud: 3 draws, keep the crop with the most cloud.
        best = None
        for _ in range(3):
            r0 = rng.randrange(0, max(1, h - size + 1)); c0 = rng.randrange(0, max(1, w - size + 1))
            mm = m[r0:r0 + size, c0:c0 + size]
            s = float(mm.astype(np.float32).mean())
            if best is None or s > best[0]:
                best = (s, r0, c0)
        _, r0, c0 = best
        xs.append(x[:, r0:r0 + size, c0:c0 + size]); ys.append(y[:, r0:r0 + size, c0:c0 + size]); ms.append(m[None, r0:r0 + size, c0:c0 + size])
    f = lambda a: torch.from_numpy(np.stack(a, 0).astype(np.float32))
    return f(xs), f(ys), f(ms)

# ───────────────────────── model ─────────────────────────
def build_model(width=32, depth=4, attn=False):
    import torch, torch.nn as nn, torch.nn.functional as F

    class Res(nn.Module):
        def __init__(s, c):
            super().__init__()
            s.a = nn.Conv2d(c, c, 3, padding=1); s.b = nn.Conv2d(c, c, 3, padding=1); s.n = nn.GroupNorm(8, c)
        def forward(s, x):
            return x + s.b(F.silu(s.n(s.a(x))))

    class Attn(nn.Module):   # one global self-attention block at the bottleneck (cheap there: the map is tiny)
        def __init__(s, c, heads=4):
            super().__init__(); s.n = nn.GroupNorm(8, c); s.qkv = nn.Conv2d(c, 3 * c, 1); s.o = nn.Conv2d(c, c, 1); s.h = heads
        def forward(s, x):
            b, c, h, w = x.shape
            q, k, v = s.qkv(s.n(x)).reshape(b, 3, s.h, c // s.h, h * w).unbind(1)
            a = F.scaled_dot_product_attention(q.transpose(-1, -2), k.transpose(-1, -2), v.transpose(-1, -2))
            return x + s.o(a.transpose(-1, -2).reshape(b, c, h, w))

    class UNet(nn.Module):
        def __init__(s):
            super().__init__()
            ch = [width * min(2 ** i, 8) for i in range(depth + 1)]
            s.inp = nn.Conv2d(N_IN, ch[0], 3, padding=1)
            s.down = nn.ModuleList(); s.up = nn.ModuleList(); s.skipc = ch
            for i in range(depth):
                s.down.append(nn.Sequential(Res(ch[i]), nn.Conv2d(ch[i], ch[i + 1], 3, stride=2, padding=1)))
            s.mid = nn.Sequential(Res(ch[depth]), Attn(ch[depth]) if attn else nn.Identity(), Res(ch[depth]))
            for i in reversed(range(depth)):
                s.up.append(nn.ModuleDict({"c": nn.Conv2d(ch[i + 1] + ch[i], ch[i], 3, padding=1), "r": Res(ch[i])}))
            s.out = nn.Conv2d(ch[0], 6, 3, padding=1)
        def forward(s, x):
            h = s.inp(x); skips = []
            for d in s.down:
                skips.append(h); h = d(h)
            h = s.mid(h)
            for u in s.up:
                sk = skips.pop()
                h = F.interpolate(h, size=sk.shape[-2:], mode="nearest")
                h = u["r"](F.silu(u["c"](torch.cat([h, sk], 1))))
            return s.out(h)     # log1p-space radiance; linear head (a softplus head died in the warmup: once its
                                # pre-activation went below ~-10 the gradient was ~0 and Adam could not bring it back)
    return UNet()

def pad_to(x, m=16):
    import torch.nn.functional as F
    h, w = x.shape[-2:]
    ph, pw = (-h) % m, (-w) % m
    return F.pad(x, (0, pw, 0, ph), mode="replicate"), (h, w)

# ───────────────────────── commands ─────────────────────────
def cmd_stats(a):
    files = list_views(a.data)
    print(f"{len(files)} views")
    acc = {}
    for f in files[:: max(1, len(files) // 40)]:
        d, meta = load_npz(f)
        m = d[..., I["w_acc"]] > 1e-4
        for i, n in enumerate(L):
            c = d[..., i][m] if m.any() else d[..., i].ravel()
            if c.size == 0: continue
            s = acc.setdefault(n, [1e30, -1e30, 0.0, 0])
            s[0] = min(s[0], float(c.min())); s[1] = max(s[1], float(c.max())); s[2] += float(np.abs(c).mean()); s[3] += 1
    for n, s in acc.items():
        print(f"  {n:12s} min {s[0]:11.4g} max {s[1]:11.4g} mean|.| {s[2] / max(s[3], 1):11.4g}")
    d, meta = load_npz(files[0])
    x = features(d); print("features", x.shape, "finite", np.isfinite(x).all(), "range", x.min(), x.max())
    y = targets(d); print("targets", y.shape, "range", y.min(), y.max(), "cloud px", (d[..., I['w_acc']] > 1e-4).mean())

def cmd_train(a):
    import torch, torch.nn.functional as F
    torch.backends.cudnn.benchmark = True
    dev = "cuda" if torch.cuda.is_available() else "cpu"
    files = list_views(a.data)
    tr, va = split(files, a.val_frac, a.seed)
    print(f"{len(tr)} train views, {len(va)} val views; loading ...", flush=True)
    t0 = time.time()
    ctr = Cache(tr, a.cache_mb); cva = Cache(va[: a.val_views], a.cache_mb // 4)
    print(f"loaded in {time.time() - t0:.0f} s", flush=True)
    model = build_model(a.width, a.depth, a.attn).to(dev)
    nparam = sum(p.numel() for p in model.parameters())
    print(f"model: width {a.width} depth {a.depth} attn {a.attn}: {nparam / 1e6:.2f} M params", flush=True)
    opt = torch.optim.AdamW(model.parameters(), lr=a.lr, weight_decay=1e-4)
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, max_lr=a.lr, total_steps=a.steps, pct_start=0.1, final_div_factor=100)
    scaler = torch.amp.GradScaler(enabled=False)      # bf16 needs no loss scaling
    bad = 0; good_state = None; good_step = 0
    if a.resume:
        ck = torch.load(a.resume, map_location=dev); model.load_state_dict(ck["model"])
        print(f"resumed weights from {a.resume} (step {ck.get('step')}, val {ck.get('val', {}).get('levels')})", flush=True)
    rng = random.Random(a.seed)
    os.makedirs(a.out, exist_ok=True)
    best = 1e30; log = open(os.path.join(a.out, "log.txt"), "a")
    def loss_fn(p, y, m):
        wgt = 0.15 + 0.85 * m                   # clear pixels count a little (the net must output 0 there)
        l1 = (torch.abs(p - y) * wgt).mean()    # the log space == the paper's log term
        lin = (torch.abs(dec_y_t(p) - dec_y_t(y)) * wgt).mean()
        return 0.6 * l1 + 0.4 * lin / (1.0 + lin.detach())   # the linear term, tamed by its own scale
    t0 = time.time()
    for step in range(1, a.steps + 1):
        model.train()
        x, y, m = crops(ctr, a.batch, a.crop, rng)
        x, y, m = x.to(dev), y.to(dev), m.to(dev)
        with torch.autocast(device_type=dev, dtype=torch.bfloat16, enabled=(dev == "cuda")):
            p = model(x)
        loss = loss_fn(p.float(), y, m)
        if not torch.isfinite(loss):
            bad += 1
            print(f"step {step}: non-finite loss, skipped ({bad} in a row)", flush=True)
            if bad >= 3 and good_state is not None:     # the weights themselves went non-finite: back to the last good ones
                model.load_state_dict(good_state); opt.state.clear(); bad = 0
                print(f"step {step}: weights restored from step {good_step}", flush=True)
            continue
        bad = 0
        if step % 200 == 0:
            good_state = {k: v.detach().clone() for k, v in model.state_dict().items()}; good_step = step
        opt.zero_grad(set_to_none=True)
        scaler.scale(loss).backward()
        scaler.unscale_(opt); torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
        scaler.step(opt); scaler.update(); sched.step()
        if step % 50 == 0:
            print(f"step {step} loss {loss.item():.4f} lr {sched.get_last_lr()[0]:.2e} {(time.time() - t0) / step * 1000:.0f} ms/step", flush=True)
        if step % a.eval_every == 0 or step == a.steps:
            v = evaluate(model, cva, dev)
            msg = f"step {step} val: " + " ".join(f"{k} {v[k]:.4f}" for k in v)
            print(msg, flush=True); log.write(msg + "\n"); log.flush()
            torch.save({"model": model.state_dict(), "width": a.width, "depth": a.depth, "attn": a.attn, "n_in": N_IN, "step": step, "val": v},
                       os.path.join(a.out, "last.pt"))
            if v["mae_log"] < best:
                best = v["mae_log"]
                torch.save({"model": model.state_dict(), "width": a.width, "depth": a.depth, "attn": a.attn, "n_in": N_IN, "step": step, "val": v},
                           os.path.join(a.out, "best.pt"))
    print("done; best val mae_log", best)

def evaluate(model, cache, dev):
    import torch
    model.eval()
    tot = {"mae_log": 0.0, "mae_log_key": 0.0, "mae_log_amb": 0.0, "rel_lin_key": 0.0, "rel_lin_amb": 0.0}
    n = 0
    with torch.no_grad():
        for x, y, m, name in cache.items:
            xt = torch.from_numpy(x.astype(np.float32))[None].to(dev); yt = torch.from_numpy(y.astype(np.float32))[None].to(dev)
            mt = torch.from_numpy(m.astype(np.float32))[None, None].to(dev)
            xp, (h, w) = pad_to(xt)
            with torch.autocast(device_type=dev, dtype=torch.bfloat16, enabled=(dev == "cuda")):
                p = model(xp)[..., :h, :w].float()
            cm = mt.sum().clamp(min=1.0)
            d = torch.abs(p - yt) * mt
            tot["mae_log"] += (d.mean(1, keepdim=True).sum() / cm).item()
            tot["mae_log_key"] += (d[:, :3].mean(1, keepdim=True).sum() / cm).item()
            tot["mae_log_amb"] += (d[:, 3:].mean(1, keepdim=True).sum() / cm).item()
            pl, yl = dec_y_t(p), dec_y_t(yt)
            for k, sl in (("rel_lin_key", slice(0, 3)), ("rel_lin_amb", slice(3, 6))):
                num = (torch.abs(pl[:, sl] - yl[:, sl]) * mt).sum(); den = (yl[:, sl] * mt).sum().clamp(min=1e-3 * cm * 3)   # a dark view does not blow the ratio up
                tot[k] += (num / den).item()
            # Display levels (the project's unit): the tonemapped total, key + amb, 8-bit, over cloud pixels.
            tp = 255.0 * (1.0 - torch.exp(-(pl[:, :3] + pl[:, 3:]))); ty = 255.0 * (1.0 - torch.exp(-(yl[:, :3] + yl[:, 3:])))
            tot["levels"] = tot.get("levels", 0.0) + ((torch.abs(tp - ty).mean(1, keepdim=True) * mt).sum() / cm).item()
            n += 1
    return {k: v / max(n, 1) for k, v in tot.items()}

def load_model(path, dev):
    import torch
    ck = torch.load(path, map_location=dev)
    model = build_model(ck["width"], ck["depth"], ck["attn"]).to(dev)
    model.load_state_dict(ck["model"]); model.eval()
    return model, ck

def tonemap(rgb):
    x = np.maximum(rgb, 0.0)
    return (255.0 * (1.0 - np.exp(-x))).astype(np.uint8)

def cmd_eval(a):
    import torch
    from PIL import Image
    dev = "cuda" if torch.cuda.is_available() else "cpu"
    model, ck = load_model(a.ckpt, dev)
    files = list_views(a.data)
    tr, va = split(files, a.val_frac, a.seed)
    if a.filter:
        # A Python expression over the view's state summary `s` (sun_el_deg, moon_el_deg, eye_h_m, fov, ...):
        #   --filter "s['eye_h_m'] > 100e3"   (orbit)     --filter "-6 < s['sun_el_deg'] < 8"   (sunset)
        def keep(f):
            with np.load(f) as z: s = json.loads(str(z["meta"]))["summary"]
            s["name"] = os.path.basename(f)
            try: return bool(eval(a.filter, {}, {"s": s}))
            except Exception: return False
        va = [f for f in va if keep(f)]
        print(f"filter kept {len(va)} validation views")
    va = va[: a.n]
    if not va: sys.exit("no validation views")
    cache = Cache(va, 4000)
    v = evaluate(model, cache, dev)
    print("val:", json.dumps(v, indent=0))
    if a.per_view:
        for (x, y, m, name), f in zip(cache.items, va):
            one = Cache.__new__(Cache); one.items = [(x, y, m, name)]
            with np.load(f) as z: s = json.loads(str(z["meta"]))["summary"]
            r = evaluate(model, one, dev)
            extra = ""
            if a.frames_dir:
                # The net's output averaged over the view's single-frame feature dumps (collect.py --frames-each), against
                # the converged target: what the sky TAA would show, the fair reading of gate 1.
                fr = sorted(glob.glob(os.path.join(a.frames_dir, name[:-4] + "_f*.npz")))
                if fr:
                    acc = None
                    for ff in fr:
                        dd, mm = load_npz(ff)
                        xt = torch.from_numpy(features(dd, mm))[None].to(dev); xp, (h, w) = pad_to(xt)
                        with torch.no_grad(): pl = dec_y_t(model(xp)[..., :h, :w].float())
                        acc = pl if acc is None else acc + pl
                    pl = acc / len(fr)
                    yl = torch.from_numpy(targets(load_npz(f)[0]))[None].to(dev)
                    mt = torch.from_numpy(m.astype(np.float32))[None, None].to(dev)
                    tp = 255.0 * (1.0 - torch.exp(-(pl[:, :3] + pl[:, 3:]))); ty = 255.0 * (1.0 - torch.exp(-(yl[:, :3] + yl[:, 3:])))
                    lv = ((torch.abs(tp - ty).mean(1, keepdim=True) * mt).sum() / mt.sum().clamp(min=1.0)).item()
                    extra = f"  levels_avg{len(fr)} {lv:5.2f}"
            print(f"  {name:36s} sun {s.get('sun_el_deg', 0) or 0:6.1f} moon {s.get('moon_el_deg', 0) or 0:6.1f} "
                  f"eye {s.get('eye_h_m', 0) or 0:9.0f} m  levels {r['levels']:5.2f} key {r['rel_lin_key']:.3f} amb {r['rel_lin_amb']:.3f}{extra}")
    if a.sheet:
        tiles = []
        for (x, y, m, name), f in zip(cache.items, va):
            d, meta = load_npz(f)
            xt = torch.from_numpy(x.astype(np.float32))[None].to(dev)
            xp, (h, w) = pad_to(xt)
            with torch.no_grad():
                p = dec_y_t(model(xp)[..., :h, :w].float())[0].cpu().numpy()
            key_t = d[..., 0:3]; amb_t = d[..., 3:6]; rest = d[..., 6:9]
            key_p = p[:3].transpose(1, 2, 0); amb_p = p[3:].transpose(1, 2, 0)
            ex = a.exposure
            row = [tonemap(ex * (key_t + amb_t + rest)), tonemap(ex * (key_p + amb_p + rest)),
                   tonemap(ex * key_t), tonemap(ex * key_p), tonemap(ex * amb_t), tonemap(ex * amb_p)]
            dd = np.abs(key_p + amb_p - key_t - amb_t).sum(-1, keepdims=True) * ex * 4.0
            row.append(tonemap(np.repeat(dd, 3, -1)))
            step = a.div
            tiles.append(np.concatenate([r[::step, ::step] for r in row], 1))
        sheet = np.concatenate(tiles, 0)
        Image.fromarray(sheet).save(a.sheet)
        print(f"{a.sheet}: rows = views, columns = target total | predicted total | key target | key pred | amb target | amb pred | |diff| x4")

def cmd_bench(a):
    """Inference time of the net at the grid size: a checkpoint, or `none` with --width/--depth (random weights: the
    cost does not depend on them). PyTorch fp16 eager with channels_last is an UPPER bound for a fused Vulkan kernel."""
    import torch
    dev = "cuda"; torch.backends.cudnn.benchmark = True
    if a.ckpt == "none":
        model = build_model(a.width, a.depth, a.attn).to(dev).eval()
    else:
        model, ck = load_model(a.ckpt, dev)
    nparam = sum(p.numel() for p in model.parameters())
    w, h = (int(s) for s in a.size.split("x"))
    x = torch.randn(1, N_IN, h, w, device=dev)
    xp, _ = pad_to(x)
    model = model.half().to(memory_format=torch.channels_last); xp = xp.half().contiguous(memory_format=torch.channels_last)
    with torch.no_grad():
        for _ in range(10): model(xp)
        torch.cuda.synchronize(); t0 = time.time()
        for _ in range(50): model(xp)
        torch.cuda.synchronize()
    print(f"{a.size} width {getattr(a, 'width', '?')} depth {getattr(a, 'depth', '?')} ({nparam / 1e6:.2f} M params): "
          f"{(time.time() - t0) / 50 * 1000:.2f} ms per pass (PyTorch fp16 eager, channels_last)")

def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = p.add_subparsers(dest="cmd", required=True)
    s = sp.add_parser("stats"); s.add_argument("data"); s.set_defaults(fn=cmd_stats)
    s = sp.add_parser("train"); s.add_argument("data"); s.add_argument("--out", required=True)
    s.add_argument("--steps", type=int, default=20000); s.add_argument("--batch", type=int, default=8); s.add_argument("--crop", type=int, default=256)
    s.add_argument("--lr", type=float, default=5e-4); s.add_argument("--width", type=int, default=32); s.add_argument("--depth", type=int, default=4)
    s.add_argument("--attn", action="store_true"); s.add_argument("--eval-every", type=int, default=1000)
    s.add_argument("--val-frac", type=float, default=0.1); s.add_argument("--val-views", type=int, default=24); s.add_argument("--seed", type=int, default=0)
    s.add_argument("--cache-mb", type=int, default=12000); s.add_argument("--resume", help="checkpoint whose weights to start from")
    s.set_defaults(fn=cmd_train)
    s = sp.add_parser("eval"); s.add_argument("ckpt"); s.add_argument("data"); s.add_argument("--n", type=int, default=12)
    s.add_argument("--sheet"); s.add_argument("--div", type=int, default=2); s.add_argument("--exposure", type=float, default=1.0)
    s.add_argument("--val-frac", type=float, default=0.1); s.add_argument("--seed", type=int, default=0)
    s.add_argument("--filter", help="Python expression over the state summary s, e.g. \"s['eye_h_m'] > 100e3\"")
    s.add_argument("--per-view", action="store_true"); s.add_argument("--frames-dir", help="single-frame dumps (collect.py --frames-each) for a frame-averaged reading")
    s.set_defaults(fn=cmd_eval)
    s = sp.add_parser("bench"); s.add_argument("ckpt"); s.add_argument("--size", default="640x360")
    s.add_argument("--width", type=int, default=32); s.add_argument("--depth", type=int, default=4); s.add_argument("--attn", action="store_true")
    s.set_defaults(fn=cmd_bench)
    a = p.parse_args()
    a.fn(a)

if __name__ == "__main__":
    main()
