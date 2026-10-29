"""Replay the MMT-9 Starlink light curves with the shipped models and summarise model vs measured.

usage: mmt_replay.py --tool <SatModelTool.exe> --models <data/satellite_models> --rows <rows.pkl>
                     --fitset <fitset.pkl> --work <dir> --out <results.json>

Inputs (not in the repo: the MMT data is public, mmt.favor2.info/satellites, ~100 tracks of 10 Hz photometry):
  rows.pkl   one dict per track: sat (NORAD), trk (MMT track id), t (s from the track start), mag (MMT 'Clear', apparent,
             extinction-corrected), dist (km), pen (penumbra flag)
  fitset.pkl the calibration's track list: key, des (V1.0 / VisorSat), spec (SatModelTool --pass spec, SGP4 states at
             0.1 s, WGS84 observer), tb / magb (1.5-s medians), hold (held out of the fit), bright (unmodelled bright mode)

Every track is evaluated with SatModelTool --pass against the model file given, so the result follows the models as
they are now. Output: per-track summaries, flare events, and the light curves of the tracks the figures show.
"""
import argparse, json, os, pickle, subprocess
from concurrent.futures import ThreadPoolExecutor
import numpy as np

MODEL_OF = {"V1.0": "starlink_v1_0", "VisorSat": "starlink_visorsat"}
FLARE_TRACKS = ("46071_19195344", "45713_19105664", "44714_18902994")   # the three published flares (mmt_passes.json)


def run_pass(tool, spec_path, model_path, out_path):
    if not os.path.exists(out_path):
        s = json.load(open(spec_path)); s["model"] = model_path
        sp = out_path[:-9] + ".spec.json"; json.dump(s, open(sp, "w"))
        r = subprocess.run([tool, "--pass", sp, "--pass-out", out_path], capture_output=True, text=True)
        if r.returncode:
            raise RuntimeError(r.stdout + r.stderr)
    return json.load(open(out_path))["samples"]


def median_bins(t, y, w):
    edges = np.arange(t[0], t[-1] + w, w); tb, yb = [], []
    for a in edges[:-1]:
        k = (t >= a) & (t < a + w)
        if k.sum() >= 3:
            tb.append(np.median(t[k])); yb.append(np.median(y[k]))
    return np.array(tb), np.array(yb)


def baseline(t, y, deg=2, clip=2.5, it=5):
    """Robust low-order polynomial in time (sigma-clipped): the smooth part of a light curve."""
    t = t - t.mean(); k = np.isfinite(y)
    for _ in range(it):
        c = np.polyfit(t[k], y[k], deg); e = y - np.polyval(c, t)
        sd = 1.4826 * np.median(np.abs(e[k])) + 1e-3
        k = np.isfinite(y) & (np.abs(e) < clip * sd)
    return np.polyval(c, t), sd


def main():
    ap = argparse.ArgumentParser()
    for a in ("tool", "models", "rows", "fitset", "work", "out"):
        ap.add_argument("--" + a, required=True)
    ap.add_argument("--workers", type=int, default=6)
    A = ap.parse_args()
    os.makedirs(A.work, exist_ok=True)
    rows = {f"{r['sat']}_{r['trk']}": r for r in pickle.load(open(A.rows, "rb"))}
    tracks = pickle.load(open(A.fitset, "rb"))
    jobs = [(A.tool, t["spec"], os.path.join(A.models, MODEL_OF[t["des"]] + ".json"),
             os.path.join(A.work, t["key"] + ".out.json")) for t in tracks]
    with ThreadPoolExecutor(A.workers) as ex:
        outs = list(ex.map(lambda j: run_pass(*j), jobs))

    res = dict(tracks=[], flares=[], curves={})
    for t, o in zip(tracks, outs):
        r = rows[t["key"]]
        t0s = json.load(open(t["spec"]))["samples"][0]["t_j2000"]
        ts = np.array([s["t_j2000"] for s in o])
        g = lambda k: np.array([np.nan if s[k] is None else s[k] for s in o], float)
        mvac, lit, ph, el, rng = g("mag"), g("lit"), g("phase_deg"), g("elevation_deg"), g("range_m") / 1e3
        dom = np.array([s["dominant_lobe"] for s in o])
        # 1.5-s bins (the calibration's): residual MMT - model, above the atmosphere
        mb = np.interp(t["tb"], ts, mvac); lb = np.interp(t["tb"], ts, lit)
        ok = (lb > 0.99) & np.isfinite(mb)
        resb = t["magb"][ok] - mb[ok]
        phb = np.interp(t["tb"][ok], ts, ph); elb = np.interp(t["tb"][ok], ts, el)
        if ok.sum() < 3:
            continue
        # raw points, range-normalised (m1000) for peak / flare analysis
        tobs = t0s + 1.0 + r["t"]                       # the calibration's alignment (pass spec pad 1 s)
        k = (r["pen"] == 0) & np.isfinite(r["mag"]) & (r["dist"] > 0) & (tobs >= ts[0]) & (tobs <= ts[-1])
        to, mo = tobs[k], r["mag"][k]
        m1o = mo - 5 * np.log10(r["dist"][k] / 1000)
        rm = np.interp(to, ts, rng); m1m = np.interp(to, ts, mvac) - 5 * np.log10(rm / 1000)
        tb1, yb1 = median_bins(to, m1o, 1.0)                              # 1-s medians
        _, ybm = median_bins(to, m1m, 1.0)
        n = min(len(yb1), len(ybm)); tb1, yb1, ybm = tb1[:n], yb1[:n], ybm[:n]
        rec = dict(key=t["key"], sat=int(t["sat"]), des=t["des"], year=int(t["year"]), hold=bool(t["hold"]),
                   bright=bool(t["bright"]), n_bins=int(ok.sum()), dur_s=float(to[-1] - to[0]) if len(to) else 0.0,
                   med_res=float(np.median(resb)), within_rms=float(np.sqrt(np.mean((resb - np.median(resb)) ** 2))),
                   ph=float(np.median(phb)), el=float(np.median(elb)), sunalt=float(np.median(t["sunalt"][ok])),
                   dom_lobe=int(np.bincount(dom[dom >= 0]).argmax()) if (dom >= 0).any() else -1)
        if n >= 6:
            # peak of the range-normalised curve (where the geometry makes it brightest), observed vs model
            io, im = int(np.argmin(yb1)), int(np.argmin(ybm))
            rec.update(peak_obs_m1000=float(yb1[io]), peak_mod_m1000=float(ybm[im]),
                       peak_dt_s=float(tb1[im] - tb1[io]), peak_obs_at_edge=bool(io in (0, n - 1)),
                       peak_mod_at_edge=bool(im in (0, n - 1)))
            # flares: 1-s medians brighter than the smooth baseline by > 0.4 mag (observed), model at the same times
            bo, sdo = baseline(tb1, yb1); bm, _ = baseline(tb1, ybm)
            exo, exm = bo - yb1, bm - ybm                                 # positive = brighter than baseline
            rec.update(obs_excess_max=float(exo.max()), mod_excess_max=float(exm.max()), obs_base_sd=float(sdo))
            above = exo > 0.4
            i = 0
            while i < n:
                if above[i]:
                    j = i
                    while j + 1 < n and above[j + 1]:
                        j += 1
                    pk = i + int(np.argmax(exo[i:j + 1]))
                    res["flares"].append(dict(key=t["key"], des=t["des"], bright=bool(t["bright"]), t_rel=float(tb1[pk] - tb1[0]),
                                              obs_excess=float(exo[pk]), mod_excess=float(exm[pk]), dur_s=float(tb1[j] - tb1[i] + 1.0),
                                              mod_excess_max_track=float(exm.max())))
                    i = j + 1
                else:
                    i += 1
        res["tracks"].append(rec)
        res["curves"][t["key"]] = dict(t_raw=(to - to[0]).round(2).tolist() if len(to) else [], m_raw=mo.round(3).tolist(),
                                       t_bin=(t["tb"][ok] - (to[0] if len(to) else 0)).round(2).tolist(),
                                       m_bin=t["magb"][ok].round(3).tolist(), t_mod=(ts - (to[0] if len(to) else 0)).round(2).tolist(),
                                       m_mod=np.where(lit > 0.99, mvac, np.nan).round(3).tolist(), ph=ph.round(2).tolist(),
                                       rng=rng.round(1).tolist(), dom=dom.tolist())
    json.dump(res, open(A.out, "w"))
    # summary table
    T = res["tracks"]
    for des in ("V1.0", "VisorSat"):
        for grp, f in (("fit", lambda x: not x["hold"] and not x["bright"]), ("held out", lambda x: x["hold"] and not x["bright"]),
                       ("bright", lambda x: x["bright"])):
            s = [x for x in T if x["des"] == des and f(x)]
            if not s:
                continue
            m = np.array([x["med_res"] for x in s]); w = np.array([x["within_rms"] for x in s])
            print(f"{des:8s} {grp:8s} n={len(s):2d} median {np.median(m):+.2f} track rms {np.sqrt(np.mean(m**2)):.2f} "
                  f"within-track rms (median) {np.median(w):.2f}")
    F = res["flares"]
    print(f"flares (1-s medians > 0.4 mag over baseline): {len(F)} in {len(set(f['key'] for f in F))} tracks; "
          f"model excess there: median {np.median([f['mod_excess'] for f in F]):+.3f}, max {max(f['mod_excess'] for f in F):+.3f}")


if __name__ == "__main__":
    main()
