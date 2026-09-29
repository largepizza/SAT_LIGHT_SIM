"""Table of `perf name=` results from a live.py outbox file (or run.py results.jsonl).

    python tools/perf_analysis/live_perf_table.py <outbox.json|results.jsonl> [baseline.json]

Prints GPU total and the cloud_march bucket per named perf; with a baseline, the change in each."""
import json
import sys


def load(path):
    if path.endswith('.jsonl'):
        rows = [json.loads(l) for l in open(path, encoding='utf-8') if l.strip()]
    else:
        rows = json.load(open(path, encoding='utf-8'))['results']
    out = {}
    for r in rows:
        cmd = r.get('cmd') or r.get('command', '')
        if not cmd.startswith('perf') or 'name=' not in cmd:
            continue
        name = cmd.split('name=')[1].split()[0]
        res = r.get('result', {})
        gpu = res.get('gpu_ms', {})
        t = res.get('gpu_total_ms', float('nan'))
        t = t.get('mean', float('nan')) if isinstance(t, dict) else t
        out[name] = (t, gpu.get('cloud_march', float('nan')))
    return out


cur = load(sys.argv[1])
base = load(sys.argv[2]) if len(sys.argv) > 2 else {}
print(f"{'view':24s} {'total':>7s} {'clouds':>7s}" + ("   base clouds   delta" if base else ''))
for k, (t, c) in cur.items():
    line = f"{k:24s} {t:7.2f} {c:7.2f}"
    if k in base:
        b = base[k][1]
        line += f"   {b:11.2f} {c - b:+7.2f} ({(c - b) / b * 100:+.0f}%)"
    print(line)
