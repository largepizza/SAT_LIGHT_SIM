#!/usr/bin/env python3
"""Run SatBench under material overrides and tabulate the result (Phase 3c calibration scans).

    python tools/benchmarks/scan_overrides.py --tool build/Debug/SatModelTool.exe \
        --variant "baseline" \
        --variant "beckmann: bus_metal.distribution=beckmann" \
        --variant "cells 0.03: bus_metal.distribution=beckmann solar_cell.diffuse_albedo=0.03"

Each --variant is "<label>: <override> <override> ..." (overrides as SatModelTool --set takes them;
a label alone means no overrides). Every variant runs the VisorSat distribution, the V1.0
distribution and the differential with the same seed and sample count, so rows differ only by the
overrides. Reports go to <report-dir>/<variant index>/ and are kept, so a chosen row is traceable
back to its full report (git commit, file hashes, every sample).
"""
import argparse
import json
import os
import subprocess
import sys

BENCH = {
    'visorsat': 'data/benchmarks/mallama2021_visorsat.json',
    'v1_0': 'data/benchmarks/mallama2020a_original.json',
    'diff': 'data/benchmarks/visorsat_vs_original.json',
}


def run(tool, bench, overrides, samples, seed, outdir):
    cmd = [os.path.abspath(tool), '--run-benchmark', bench, '--samples', str(samples), '--seed', str(seed), '--report-dir', outdir]
    for o in overrides:
        cmd += ['--set', o]
    subprocess.run(cmd, check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def load(outdir, stem):
    for f in os.listdir(outdir):
        if f.startswith(stem + '__') and f.endswith('.json'):
            with open(os.path.join(outdir, f), encoding='utf-8') as fh:
                return json.load(fh)
    return None


def metric(report, name):
    for row in report.get('comparison', []):
        if row['metric'] == name:
            return row.get('model')
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--tool', default='build/Debug/SatModelTool.exe')
    ap.add_argument('--samples', type=int, default=4000)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--report-dir', default='benchmark_runs/scan')
    ap.add_argument('--variant', action='append', required=True)
    ap.add_argument('--curve', action='store_true', help='print each variant\'s VisorSat phase curve')
    a = ap.parse_args()

    rows = []
    for i, v in enumerate(a.variant):
        label, _, rest = v.partition(':')
        overrides = rest.split()
        outdir = os.path.join(a.report_dir, str(i))
        os.makedirs(outdir, exist_ok=True)
        for key in ('visorsat', 'v1_0', 'diff'):
            run(a.tool, BENCH[key], overrides, a.samples, a.seed, outdir)
        vs = load(outdir, 'mallama2021_visorsat')
        v1 = load(outdir, 'mallama2020a_original')
        df = load(outdir, 'visorsat_vs_original')
        rows.append((label.strip(), overrides, vs, v1, df))

    print(f"{'variant':34s} {'VS mean':>7s} {'VS pm':>6s} {'curve':>6s} {'V1 mean':>7s} {'diff':>6s}")
    print(f"{'(reference)':34s} {7.218:7.2f} {7.218:6.2f} {0.0:6.2f} {5.93:7.2f} {1.29:6.2f}")
    for label, ov, vs, v1, df in rows:
        d = df['comparison'][0]['model'] if df else float('nan')
        print(f"{label[:34]:34s} {metric(vs, 'mean_m1000'):7.2f} {metric(vs, 'mean_m1000_phase_matched'):6.2f} "
              f"{metric(vs, 'phase_curve_rms'):6.2f} {metric(v1, 'mean_m1000'):7.2f} {d:6.2f}")
    if a.curve:
        for label, ov, vs, v1, df in rows:
            print(f"\n{label}:  " + '  '.join(
                f"{b['phase_lo_deg']}:{b['delta']:+.2f}" for b in vs['phase_curve'] if 'delta' in b and b['n_obs'] >= 10))


if __name__ == '__main__':
    sys.exit(main())
