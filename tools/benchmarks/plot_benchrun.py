"""Plot a SatBench report (SatModelTool --run-benchmark, benchmarking milestone M5).

Usage (from the repo root, with the perf_analysis venv, which has matplotlib):
    tools/perf_analysis/.venv/Scripts/python.exe tools/benchmarks/plot_benchrun.py <report.json> [...]

For each distribution report, writes <report>.png next to it: the simulated and observed 1000-km
magnitude histograms, and magnitude vs phase angle with 10-degree binned means. Differential
reports are skipped (plot their two sub-reports instead).
"""
import json
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402


def binned_means(phase, mag, edges):
    centers, means = [], []
    for lo, hi in zip(edges[:-1], edges[1:]):
        sel = (phase >= lo) & (phase < hi)
        if sel.sum() >= 5:
            centers.append(0.5 * (lo + hi))
            means.append(mag[sel].mean())
    return np.array(centers), np.array(means)


def plot(path: Path) -> None:
    r = json.loads(path.read_text(encoding="utf-8"))
    if r.get("kind") != "distribution":
        print(f"{path.name}: {r.get('kind')} report, skipped")
        return
    cols = r["samples"]["columns"]
    rows = r["samples"]["rows"]
    ph = np.array([row[cols.index("phase_deg")] for row in rows], float)
    m = np.array([row[cols.index("m1000")] for row in rows], float)
    ocols = r["reference_observations"]["columns"]
    orows = r["reference_observations"]["rows"]
    oph = np.array([row[ocols.index("phase_deg")] for row in orows], float)
    om = np.array([row[ocols.index("m1000")] for row in orows], float)

    fig, (a1, a2) = plt.subplots(1, 2, figsize=(12, 4.8))
    bins = np.arange(0.0, 11.0, 0.25)
    a1.hist(m, bins=bins, density=True, alpha=0.55, label=f"model ({len(m)})")
    if len(om):
        a1.hist(om, bins=bins, density=True, alpha=0.55, label=f"observed ({len(om)})")
    else:
        ref = r["reference"]
        a1.axvline(ref["mean_m1000"], color="C1", lw=2, label=f"published mean {ref['mean_m1000']}")
    a1.axvline(r["result"]["mean_m1000"], color="C0", ls="--", lw=1)
    a1.set_xlabel("1000-km magnitude (fainter →)")
    a1.set_ylabel("density")
    a1.legend()

    edges = np.arange(0.0, 190.0, 10.0)
    a2.scatter(ph, m, s=4, alpha=0.25, label="model samples")
    if len(om):
        a2.scatter(oph, om, s=8, alpha=0.5, color="C1", label="observations")
        c, mm = binned_means(oph, om, edges)
        a2.plot(c, mm, "o-", color="C1", label="observed binned mean")
    c, mm = binned_means(ph, m, edges)
    a2.plot(c, mm, "s-", color="C0", label="model binned mean")
    a2.invert_yaxis()
    a2.set_xlabel("phase angle (deg)")
    a2.set_ylabel("1000-km magnitude")
    a2.legend(fontsize=8)

    cfg = r["config"]
    fig.suptitle(
        f"{r['benchmark']['id']} vs {r['model']['id']}  —  commit {r.get('git', {}).get('commit')}, "
        f"seed {cfg['seed']}, elev ≥ {cfg['min_elevation_deg']}°, Sun {cfg['sun_alt_window_deg']}°, "
        f"occlusion: {r['model']['occlusion']}",
        fontsize=9,
    )
    fig.tight_layout()
    out = path.with_suffix(".png")
    fig.savefig(out, dpi=110)
    plt.close(fig)
    print(f"{path.name}: wrote {out}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    for p in sys.argv[1:]:
        plot(Path(p))
