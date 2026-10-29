"""Figures for the wiki page accuracy/brightness-validation.md.

usage: figures.py --mmt <mmt_results.json> --reports <dir of --run-benchmark reports> --iridium-dump <dump.json>
                  --lee <ronlee_1997.json> --flares <flare_stats.json> --out <wiki/accuracy/img>
                  [--parity <parity.json>]

mmt_results.json comes from mmt_replay.py; the benchmark reports from the accuracy gate (build/accuracy_gate/
benchmark_runs) or `SatModelTool --run-benchmark ... --report-dir`; the Iridium dump from `SatModelTool
data/satellite_models/iridium_block1.json --dump-json`. Prints every number the page quotes.
"""
import argparse, glob, json, math, os, sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "video"))
from vidkit import photometry as VP  # noqa: E402

# reference palette (dataviz skill): categorical slots 1-3 + inks
BLUE, ORANGE, AQUA, RED, VIOLET = "#2a78d6", "#eb6834", "#1baf7a", "#e34948", "#6250d6"
INK, INK2, MUTED, GRID = "#0b0b0b", "#52514e", "#a3a29d", "#e4e3df"
plt.rcParams.update({"font.size": 9.5, "axes.edgecolor": MUTED, "axes.labelcolor": INK2, "xtick.color": INK2,
                     "ytick.color": INK2, "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.7,
                     "axes.spines.top": False, "axes.spines.right": False, "figure.facecolor": "white",
                     "axes.titlesize": 10, "axes.titleweight": "bold", "legend.frameon": False, "lines.linewidth": 2})


def save(fig, out, name):
    p = os.path.join(out, name)
    fig.savefig(p, dpi=130, bbox_inches="tight"); plt.close(fig); print("wrote", p)


def group_of(t):
    return "bright" if t["bright"] else ("held out" if t["hold"] else "fit")


GCOL = {"fit": BLUE, "held out": ORANGE, "bright": MUTED}


# ---------------------------------------------------------------------------------------------- benchmarks
BENCH = [  # file id, label, status (fitted / held out / not gated); status from data/benchmarks/KNOWN_RESIDUALS.md
    ("mallama2020a_original", "Starlink V1.0", "calibrated (MMT tracks)"),
    ("mallama2021_visorsat", "Starlink VisorSat", "calibrated (MMT tracks)"),
    ("visorsat_vs_original", "VisorSat - V1.0 (difference)", "calibrated (MMT tracks)"),
    ("mallama2020b_oneweb", "OneWeb", "fitted to this mean"),
    ("mallama2023_v2mini", "Starlink V2 Mini (mitigated)", "fitted to this mean"),
    ("mallama2025_dtc", "Starlink Direct-to-Cell", "held out"),
    ("mallama2025_starlink_v15", "Starlink V1.5", "held out"),
    ("mallama2026_amazon_leo", "Amazon Leo", "held out"),
    ("mallama2023_bluewalker3", "BlueWalker 3", "fitted to this mean"),
    ("mallama2025_guowang", "Guowang (orbit raising)", "not gated"),
]
SCOL = {"held out": AQUA, "calibrated (MMT tracks)": BLUE, "fitted to this mean": VIOLET, "not gated": RED}


def load_reports(d):
    reps = {}
    for p in glob.glob(os.path.join(d, "*.json")):
        if p.endswith(".samples.json"):
            continue
        try:
            r = json.load(open(p))
        except Exception:
            continue
        bid = os.path.basename(p).split("__")[0]
        reps[bid] = r
    return reps


def comparison_rows(r):
    for key in ("comparison", "comparisons", "rows"):
        if key in r:
            return r[key]
    for v in r.values():
        if isinstance(v, dict) and "comparison" in v:
            return v["comparison"]
    return []


def fig_benchmarks(reps, out):
    rows = []
    for bid, label, status in BENCH:
        r = reps.get(bid)
        if not r:
            print("missing report", bid); continue
        comp = comparison_rows(r)
        mean = next(c for c in comp if c.get("metric") in ("mean_m1000", "delta_mean_m1000"))
        rows.append((label, status, mean["model"], mean["reference"], mean.get("tolerance", 0.3)))
        print(f"  {label:32s} model {mean['model']:.3f} published {mean['reference']:.3f} gap {mean['model'] - mean['reference']:+.3f} ({status}) {mean['verdict']}")
    fig, ax = plt.subplots(figsize=(8.2, 4.6))
    y = np.arange(len(rows))[::-1]
    ax.axvspan(-0.3, 0.3, color="#eef4fb", zorder=0, label="gate tolerance +-0.3 mag")
    ax.axvline(0, color=INK2, lw=0.8)
    for yi, (label, status, m, p, tol) in zip(y, rows):
        g = m - p
        x = min(g, 1.0)
        ax.plot([0, x], [yi, yi], color=SCOL[status], lw=2.5, solid_capstyle="round")
        ax.plot(x, yi, "o", ms=8, color=SCOL[status], mec="white", mew=1.5)
        txt = f"{g:+.2f}" + ("  (off scale)" if g > 1.0 else "")
        ax.annotate(txt, (x, yi), xytext=(8 if g >= 0 else -8, 0), textcoords="offset points", va="center",
                    ha="left" if g >= 0 else "right", fontsize=8.5, color=INK)
    ax.set_yticks(y); ax.set_yticklabels([r[0] for r in rows])
    ax.set_xlim(-0.75, 1.15); ax.set_xlabel("model minus published mean $m_{1000}$ (mag)   <- model brighter | model fainter ->")
    handles = [plt.Line2D([], [], marker="o", ls="", color=c, ms=8, label=s) for s, c in SCOL.items()]
    handles.append(plt.Rectangle((0, 0), 1, 1, color="#eef4fb", label="gate tolerance"))
    ax.legend(handles=handles, loc="center right", fontsize=8)
    ax.set_title("Population means against published campaigns (SatBench, 5000 samples, seed 1)", loc="left")
    ax.grid(axis="y", visible=False)
    save(fig, out, "bench_summary.png")


def fig_visorsat_phase(reps, out):
    r = reps.get("mallama2021_visorsat")
    pc = r.get("phase_curve") if r else None
    if not pc:
        for v in (r or {}).values():
            if isinstance(v, dict) and "phase_curve" in v:
                pc = v["phase_curve"]
    if not pc:
        print("no phase curve"); return
    xs, mo, ob, n = [], [], [], []
    for b in pc:
        xs.append(b["phase_lo_deg"] + 5); mo.append(b.get("model_mean", np.nan)); ob.append(b.get("obs_mean", np.nan)); n.append(b.get("n_obs", 0))
    xs, mo, ob, n = map(np.array, (xs, mo, ob, n))
    fig, ax = plt.subplots(figsize=(7.4, 4.2))
    k = (n >= 10) & np.isfinite(ob)
    ax.plot(xs[k], ob[k], "o-", color=INK, ms=6, lw=1.5, label="observed bin mean (Mallama 2021, 430 passes)")
    km = np.isfinite(mo)
    ax.plot(xs[km], mo[km], "s-", color=BLUE, ms=6, label="model bin mean (SatBench)")
    for xi, ni in zip(xs[k], n[k]):
        ax.text(xi, 6.27, f"n={ni}", ha="center", fontsize=7, color=MUTED)
    ax.invert_yaxis(); ax.set_xlabel("phase angle (deg)"); ax.set_ylabel("mean $m_{1000}$ (mag)")
    ax.set_title("VisorSat phase curve: model vs observed bins (scored bins have n >= 10)", loc="left")
    ax.legend(loc="lower left", fontsize=8)
    save(fig, out, "visorsat_phase_curve.png")
    for xi, a, b, ni in zip(xs, mo, ob, n):
        print(f"  phase {xi:5.0f} model {a:.2f} observed {b:.2f} n {ni}")


# ---------------------------------------------------------------------------------------------- MMT
def pick_tracks(T):
    fit = [t for t in T if group_of(t) == "fit" and t["dur_s"] > 30]
    hold = [t for t in T if group_of(t) == "held out" and t["dur_s"] > 20 and t["key"] not in ("46071_19195344", "44714_18902994")]
    good = min(fit, key=lambda t: abs(t["med_res"]) + t["within_rms"])
    medh = np.median([t["med_res"] for t in hold]); medw = np.median([t["within_rms"] for t in hold])
    typ = min([t for t in hold if t["dur_s"] > 30], key=lambda t: abs(t["med_res"] - medh) / 0.2 + abs(t["within_rms"] - medw) / 0.1)
    worst = max(hold, key=lambda t: abs(t["med_res"]))
    bright = min([t for t in T if t["bright"] and t["dur_s"] > 20], key=lambda t: abs(t["med_res"] + 1.27))
    byk = {t["key"]: t for t in T}
    return [("Good fit (fit set)", good), ("Typical held-out track", typ), ("Worst held-out track", worst),
            ("VisorSat bright mode (excluded from the fit)", bright),
            ("Flare: STARLINK-1538 (held out)", byk["46071_19195344"]), ("Flare: STARLINK-1008", byk["44714_18902994"])]


def fig_lightcurves(R, out):
    T = R["tracks"]; picks = pick_tracks(T)
    fig, axs = plt.subplots(3, 2, figsize=(11, 10.5))
    for ax, (title, t) in zip(axs.flat, picks):
        c = R["curves"][t["key"]]
        tr, mr = np.array(c["t_raw"]), np.array(c["m_raw"])
        ax.plot(tr, mr, ".", ms=2, color=MUTED, alpha=0.6, label="MMT-9 frames (10 Hz)")
        ax.plot(c["t_bin"], c["m_bin"], "o", ms=3.5, color=INK, label="MMT 1.5-s medians")
        tm, mm = np.array(c["t_mod"]), np.array(c["m_mod"], float)
        k = (tm >= tr.min() - 1) & (tm <= tr.max() + 1)
        ax.plot(tm[k], mm[k], "-", color=BLUE, label="model (above the atmosphere)")
        ax.invert_yaxis(); ax.set_xlabel("s from track start"); ax.set_ylabel("V magnitude")
        sat = {44714: "STARLINK-1008 (V1.0)", 45713: "STARLINK-1436 (VisorSat)", 46071: "STARLINK-1538 (VisorSat)"}[t["sat"]]
        ax.set_title(title, loc="left", pad=30)
        ph = np.array(c["ph"])
        ax.text(0, 1.02, f"{sat}, {t['year']}, MMT track {t['key'].split('_')[1]}; phase {ph[k].min():.0f}-{ph[k].max():.0f} deg, "
                f"elevation {t['el']:.0f} deg\nMMT - model: median {t['med_res']:+.2f} mag, within-track rms {t['within_rms']:.2f} mag",
                transform=ax.transAxes, fontsize=7.8, color=INK2, va="bottom")
        print(f"  {title}: {t['key']} med {t['med_res']:+.2f} within {t['within_rms']:.2f} ph {t['ph']:.0f} el {t['el']:.0f}")
    axs.flat[0].legend(loc="upper right", fontsize=7.5)
    fig.suptitle("MMT-9 light curves against the shipped models (SatModelTool --pass, SGP4 from archived elements)",
                 x=0.01, ha="left", fontsize=11, fontweight="bold")
    fig.tight_layout()
    save(fig, out, "mmt_lightcurves.png")


def fig_residuals(R, out):
    T = R["tracks"]
    fig, axs = plt.subplots(1, 2, figsize=(11, 4.3), gridspec_kw=dict(width_ratios=[1.35, 1]))
    ax = axs[0]
    for g in ("fit", "held out", "bright"):
        for des, mk in (("V1.0", "s"), ("VisorSat", "o")):
            s = [t for t in T if group_of(t) == g and t["des"] == des]
            if s:
                ax.plot([t["ph"] for t in s], [t["med_res"] for t in s], mk, ms=6, color=GCOL[g],
                        mfc=GCOL[g] if g != "held out" else "white", mew=1.6, label=f"{des}, {g} ({len(s)})")
    ax.axhline(0, color=INK2, lw=0.8); ax.axhspan(-0.3, 0.3, color="#eef4fb", zorder=0)
    ax.set_xlabel("track median phase angle (deg)"); ax.set_ylabel("track median MMT - model (mag)\n<- MMT brighter | model brighter ->")
    ax.set_title("Per-track offset (94 tracks)", loc="left"); ax.legend(fontsize=7.5, ncol=3, loc="upper center", bbox_to_anchor=(0.5, -0.16))
    ax = axs[1]
    bins = np.linspace(0, 1.2, 25)
    for g in ("fit", "held out"):
        w = [t["within_rms"] for t in T if group_of(t) == g]
        ax.hist(w, bins=bins, histtype="stepfilled" if g == "fit" else "step", color=GCOL[g], alpha=0.45 if g == "fit" else 1,
                lw=2, label=f"{g}: median {np.median(w):.2f} mag")
    ax.set_xlabel("within-track rms about the track median (mag)"); ax.set_ylabel("tracks")
    ax.set_title("Light-curve shape error", loc="left"); ax.legend(fontsize=8)
    fig.tight_layout(); save(fig, out, "mmt_residuals.png")
    for des in ("V1.0", "VisorSat"):
        for g in ("fit", "held out", "bright"):
            s = [t for t in T if t["des"] == des and group_of(t) == g]
            if s:
                m = np.array([t["med_res"] for t in s])
                print(f"  {des} {g}: n {len(s)} median {np.median(m):+.2f} rms {np.sqrt(np.mean(m ** 2)):.2f} within {np.median([t['within_rms'] for t in s]):.2f}")


def fig_peaks(R, out):
    T = [t for t in R["tracks"] if "peak_dt_s" in t]
    F = [f for f in R["flares"] if f["t_rel"] > 0.5]                      # drop events on a track's first second
    fig, axs = plt.subplots(1, 3, figsize=(14, 4.4))
    ax = axs[0]
    for g in ("fit", "held out", "bright"):
        s = [t for t in T if group_of(t) == g]
        ax.plot([t["peak_mod_m1000"] for t in s], [t["peak_obs_m1000"] for t in s], "o", ms=5.5, color=GCOL[g],
                mfc=GCOL[g] if g != "held out" else "white", mew=1.5, label=g)
    lo, hi = 3.6, 8.3
    ax.plot([lo, hi], [lo, hi], color=INK2, lw=0.8); ax.fill_between([lo, hi], [lo - 0.3, hi - 0.3], [lo + 0.3, hi + 0.3], color="#eef4fb", zorder=0)
    ax.set_xlim(lo, hi); ax.set_ylim(lo, hi); ax.invert_xaxis(); ax.invert_yaxis()
    ax.set_xlabel("model: brightest 1-s $m_{1000}$ in the track"); ax.set_ylabel("MMT: brightest 1-s $m_{1000}$ in the track")
    ax.set_title("Peak magnitude per track", loc="left"); ax.legend(fontsize=8, loc="lower right")
    ax = axs[1]
    nb = [t for t in T if not t["bright"]]
    inner = [t for t in nb if not t["peak_obs_at_edge"]]
    dt = np.array([t["peak_dt_s"] for t in inner])
    edge = np.array([t["peak_mod_at_edge"] for t in inner])
    ax.hist([dt[~edge], dt[edge]], bins=np.arange(-50, 51, 4), stacked=True, color=[VIOLET, MUTED],
            label=["model peaks inside the track", "model peaks at a track end (smooth curve)"])
    ax.legend(fontsize=7.5, loc="upper right")
    ax.set_xlabel("model peak time minus MMT peak time (s)"); ax.set_ylabel("tracks")
    ax.set_title("Peak timing (MMT peak inside the track)", loc="left")
    ax.text(0.02, 0.80, f"{len(inner)} of {len(nb)} tracks peak inside the track in MMT;\n"
            f"the model peaks at a track end in {sum(t['peak_mod_at_edge'] for t in inner)} of them\n"
            f"|dt| median {np.median(np.abs(dt)):.0f} s, within 3 s: {np.mean(np.abs(dt) <= 3):.0%}",
            transform=ax.transAxes, va="top", fontsize=8, color=INK2)
    ax = axs[2]
    ax.plot([f["obs_excess"] for f in F], [f["mod_excess"] for f in F], "*", ms=10, color=RED, label="MMT flare (> 0.4 mag over baseline)")
    ax.plot([0, 2.2], [0, 2.2], color=INK2, lw=0.8, label="model = MMT")
    ax.set_xlim(0, 2.2); ax.set_ylim(-0.1, 2.2)
    ax.set_xlabel("MMT flare amplitude (mag over the smooth baseline)"); ax.set_ylabel("model excess at the same second (mag)")
    ax.set_title(f"Flares: {len(F)} in {len(set(f['key'] for f in F))} tracks", loc="left"); ax.legend(fontsize=8, loc="upper left")
    fig.tight_layout(); save(fig, out, "mmt_peaks_flares.png")
    d = np.array([t["peak_obs_m1000"] - t["peak_mod_m1000"] for t in nb])
    print(f"  peak m1000 MMT - model (non-bright, {len(nb)}): median {np.median(d):+.2f}, rms {np.sqrt(np.mean(d**2)):.2f}")
    print(f"  interior MMT peaks {len(inner)}/{len(nb)}; model at edge {sum(t['peak_mod_at_edge'] for t in inner)}; |dt| median {np.median(np.abs(dt)):.1f}, <=3 s {np.mean(np.abs(dt)<=3):.2f}")
    print(f"  flares {len(F)} in {len(set(f['key'] for f in F))} tracks; obs amplitude median {np.median([f['obs_excess'] for f in F]):.2f} max {max(f['obs_excess'] for f in F):.2f};"
          f" model excess median {np.median([f['mod_excess'] for f in F]):+.3f} max {max(f['mod_excess'] for f in F):+.3f};"
          f" model's largest excess anywhere in those tracks: median {np.median([f['mod_excess_max_track'] for f in F]):.3f}")
    allm = np.array([t["mod_excess_max"] for t in R["tracks"]]); allo = np.array([t["obs_excess_max"] for t in R["tracks"]])
    print(f"  largest excess per track: model p50 {np.median(allm):.3f} p90 {np.percentile(allm,90):.3f} max {allm.max():.3f}; MMT p50 {np.median(allo):.2f} p90 {np.percentile(allo,90):.2f}")


def fig_flare_physics(flares, out):
    """Observed flares (amplitude, width) against what a flat specular facet can produce: the reason the model misses them."""
    w = 0.31                       # deg/s: the specular bisector's mean body-frame rate at the flares (flare study)
    asun = 0.00208                 # the Sun's disc as a GGX alpha (SatModel.h kSunAlpha)
    fig, ax = plt.subplots(figsize=(8.4, 5.4))
    alpha = np.geomspace(1e-4, 0.6, 400); at = np.sqrt(alpha ** 2 + asun ** 2)
    dur = np.degrees(2 * 0.644 * at) / w
    for AF, lab, c in [(22.68 * 0.06, "whole solar array glass, 22.7 m$^2$ x F 0.06", VIOLET),
                       (3.3 * 0.9, "bus base metal, 3.3 m$^2$ x F 0.9", INK2),
                       (32e-4 * 0.06, "one 32 cm$^2$ solar cell x F 0.06", ORANGE),
                       (1e-4 * 0.9, "1 cm$^2$ of polished metal x F 0.9", AQUA)]:
        ax.plot(dur, AF / (4 * np.pi * at ** 2), "-", color=c, lw=1.8, label=f"flat facet, any roughness: {lab}")
    for a_, AF, lab, mk in [(0.05, 22.68 * 0.06, "model's array glass (alpha 0.05)", "^"),
                            (0.08, 3.3 * 0.9, "model's bus base (Beckmann alpha 0.08)", "s")]:
        t_ = math.sqrt(a_ ** 2 + asun ** 2)
        ax.plot(math.degrees(2 * 0.644 * t_) / w, AF / (4 * math.pi * t_ ** 2), mk, color=INK, ms=9, label=lab + ", were its specular to reach the observer")
    for k, s in flares.items():
        ax.plot(s["fwhm_s"], s["I_ex_peak"], "*", color=RED, ms=16)
        ax.annotate(s["sat"].split(" ")[0] + f"  +{s['peak_dmag_fit']:.1f} mag", (s["fwhm_s"], s["I_ex_peak"]), xytext=(9, -3),
                    textcoords="offset points", fontsize=8, color=RED)
    ax.axvline(math.degrees(2 * 0.644 * asun) / w, color=INK2, ls=":", lw=1)
    ax.text(math.degrees(2 * 0.644 * asun) / w * 1.06, 3e-4, "Sun's disc limit (perfect mirror)", rotation=90, fontsize=7.5, color=INK2)
    ax.set_xscale("log"); ax.set_yscale("log"); ax.set_xlim(0.3, 200); ax.set_ylim(1e-4, 1e5)
    ax.set_xlabel("flare FWHM (s), bisector sweeping 0.31 deg/s"); ax.set_ylabel("peak excess intensity $I/E$ (m$^2$ sr$^{-1}$)")
    ax.set_title("Observed flares (red) need a near-perfect mirror of 1e-5 to 1e-4 m$^2$ x F", loc="left")
    ax.legend(fontsize=7.2, loc="lower left")
    save(fig, out, "flare_physics.png")


# ---------------------------------------------------------------------------------------------- Iridium
def rot(v, ax, a):
    ax = ax / np.linalg.norm(ax)
    return v * math.cos(a) + np.cross(ax, v) * math.sin(a) + ax * (ax @ v) * (1 - math.cos(a))


def fig_iridium(dump, lee, out):
    d = json.load(open(dump)); L = [l for l in d["levels"][0]["lobes"] if l["f0"] > 0.5 and l["albedo"] < 0.05 and abs(l["n"][0] - 0.766) < 0.01]
    lee = json.load(open(lee)); rows = np.array(lee["rows"]); rk = np.array(lee["range_km"])
    m1000 = rows[:, 1] - 5 * np.log10(rk / 1000.0)
    keep = rows[:, 0] < 15        # the 11 rows past 15 deg are flares from other surfaces, not the antenna's wing
    rows, m1000 = rows[keep], m1000[keep]
    deltas = np.concatenate([np.linspace(0, 1, 201), np.linspace(1.01, 30, 400)])

    def curve(th, az):
        t = math.radians(th); n = np.array([0, 0, 1.0]); s = np.array([math.sin(t), 0, math.cos(t)]); sp = np.array([-math.sin(t), 0, math.cos(t)])
        axv = math.cos(math.radians(az)) * np.array([0, 1.0, 0]) + math.sin(math.radians(az)) * np.array([math.cos(t), 0, math.sin(t)])
        O = np.array([rot(sp, axv, math.radians(x)) for x in deltas]); I = np.zeros(len(deltas))
        for l in L:
            I += VP.lobe_intensity(n, l["area"], l["diff_area"], l["albedo"], l["f0"], l["alpha"] ** 2, l["beckmann"], 0.0, s, O, True)
        return VP.mag(I, 1e6)
    ci, ca = curve(30, 0), curve(30, 90); cm = 0.5 * (ci + ca)
    fig, ax = plt.subplots(figsize=(7.6, 4.4))
    ax.plot(rows[:, 0], m1000, "o", ms=4.5, color=INK, label=f"Lee / John 1997 visual estimates ({len(rows)}), at 1000 km")
    ax.plot(deltas, ci, "-", color=BLUE, label="model, offset in the plane of incidence")
    ax.plot(deltas, ca, "--", color=BLUE, lw=1.5, label="model, offset across it")
    ax.set_xscale("log"); ax.set_xlim(0.08, 15); ax.set_ylim(8, -9)
    ax.set_xlabel("mirror angle: observer's offset from the specular direction (deg)"); ax.set_ylabel("magnitude at 1000 km")
    ax.set_title("Iridium main mission antenna flare (incidence 30 deg)", loc="left"); ax.legend(fontsize=8, loc="lower left")
    save(fig, out, "iridium_mirror_angle.png")
    mm = np.interp(rows[:, 0], deltas, cm)
    for a, b in [(0, 0.6), (0.6, 1), (1, 2), (2, 4), (4, 7), (7, 15)]:
        k = (rows[:, 0] >= a) & (rows[:, 0] < b)
        if k.sum():
            print(f"  Iridium {a}-{b} deg n={k.sum()} data {np.median(m1000[k]):.2f} model {np.median(mm[k]):.2f}")


def fig_bw3(path, out):
    """BlueWalker 3: every timed observation (Mallama et al. 2023b App. B visual; Nandakumar et al. 2023 CCD) replayed
    with SGP4 states from element sets <= 4 days old, 2022-11-14..12-07, fully sunlit, not upper limits."""
    R = [r for r in json.load(open(path)) if "2022-11-14" <= r["utc"][:10] <= "2022-12-07" and abs(r["tle_age_d"]) <= 4
         and not r["limit"] and r["model_mag"] is not None and r["lit"] > 0.99]
    fig, axs = plt.subplots(1, 2, figsize=(11.5, 4.2))
    for ax, (src, typ, lab, col) in zip(axs, (("mallama", "Visual", "visual estimates (Mallama et al.)", INK),
                                             ("nature", "POMENIS", "Pomenis V-band CCD (Nandakumar et al.)", ORANGE))):
        v = [r for r in R if r["src"] == src and r["type"] == typ]
        ph = np.array([r["phase"] for r in v]); rk = np.array([r["range_km"] for r in v])
        o = np.array([r["obs_mag"] for r in v]) - 5 * np.log10(rk / 1000); m = np.array([r["model_mag"] for r in v]) - 5 * np.log10(rk / 1000)
        ax.plot(ph, o, "o", ms=4, color=col, alpha=0.75, label=f"{lab}, {len(v)} points, mean {o.mean():.2f}")
        ax.plot(ph, m, "s", ms=3.5, color=BLUE, alpha=0.6, label=f"model at the same instants, mean {m.mean():.2f}")
        ax.invert_yaxis(); ax.set_xlabel("phase angle (deg)"); ax.set_ylabel("$m_{1000}$ (mag)"); ax.legend(fontsize=7.8, loc="lower left")
        ax.set_title(f"BlueWalker 3: {lab.split(' (')[0]}", loc="left")
        print(f"  BW3 {typ}: n {len(v)} observed m1000 mean {o.mean():.2f}, model {m.mean():.2f}, diff median {np.median(o - m):+.2f}")
    fig.tight_layout(); save(fig, out, "bluewalker3_replay.png")


def fig_parity(par, out):
    P = json.load(open(par))["rows"]
    fig, ax = plt.subplots(figsize=(7.4, 3.6))
    names = sorted(set(p["model"] for p in P))
    for i, nm in enumerate(names):
        v = [abs(p["dmag"]) for p in P if p["model"] == nm]
        ax.plot(np.full(len(v), i) + np.random.default_rng(i).uniform(-0.18, 0.18, len(v)), np.maximum(v, 1e-6), "o", ms=4, color=BLUE, alpha=0.7)
    ax.axhline(0.02, color=RED, lw=1); ax.text(len(names) - 0.5, 0.022, "warning threshold 0.02 mag", color=RED, fontsize=8, ha="right", va="bottom")
    ax.set_yscale("log"); ax.set_xticks(range(len(names))); ax.set_xticklabels(names, rotation=20, ha="right", fontsize=8)
    ax.set_ylabel("|GPU - CPU| (mag)"); ax.set_title("What the app draws vs the evaluator (selected satellites)", loc="left")
    save(fig, out, "gpu_cpu_parity.png")
    a = np.array([abs(p["dmag"]) for p in P]); print(f"  parity n {len(a)} median {np.median(a):.5f} p95 {np.percentile(a,95):.5f} max {a.max():.5f}")


def main():
    ap = argparse.ArgumentParser()
    for a in ("mmt", "reports", "iridium-dump", "lee", "flares", "out"):
        ap.add_argument("--" + a, required=True)
    ap.add_argument("--parity")
    ap.add_argument("--bw3")
    A = ap.parse_args(); os.makedirs(A.out, exist_ok=True)
    if A.bw3:
        print("BlueWalker 3:"); fig_bw3(A.bw3, A.out)
    reps = load_reports(A.reports)
    print("benchmarks:"); fig_benchmarks(reps, A.out)
    print("VisorSat phase curve:"); fig_visorsat_phase(reps, A.out)
    R = json.load(open(A.mmt))
    print("MMT light curves:"); fig_lightcurves(R, A.out)
    print("MMT residuals:"); fig_residuals(R, A.out)
    print("MMT peaks and flares:"); fig_peaks(R, A.out)
    fig_flare_physics(json.load(open(A.flares)), A.out)
    print("Iridium:"); fig_iridium(A.iridium_dump, A.lee, A.out)
    if A.parity:
        print("parity:"); fig_parity(A.parity, A.out)


if __name__ == "__main__":
    main()
