"""GPU vs CPU parity from a harness run of tools/harness/scripts/photometry_parity.satcmd.
usage: parity_extract.py <run dir> <out parity.json>"""
import json, os, sys
run, outp = sys.argv[1], sys.argv[2]
rows, miss = [], []
for line in open(os.path.join(run, "results.jsonl")):
    r = json.loads(line)
    if not r.get("cmd", "").startswith("state"):
        continue
    st = r.get("result", {})
    sel = st.get("selection") or st.get("state", {}).get("selection") or {}
    par = sel.get("parity")
    name = sel.get("constellation") or sel.get("name") or "?"
    if par:
        rows.append(dict(cmd=r["cmd"], model=name, dmag=par["dmag"], range_gpu_m=par.get("range_gpu_m"), range_cpu_m=par.get("range_cpu_m"),
                         mag=(sel.get("photometry") or {}).get("mag")))
    else:
        miss.append((r["cmd"], name, list(sel.keys())[:12]))
json.dump(dict(rows=rows), open(outp, "w"), indent=1)
print(len(rows), "with parity;", len(miss), "without")
for m in miss[:8]:
    print("  no parity:", m)
import statistics
a = sorted(abs(r["dmag"]) for r in rows)
if a:
    print("median", statistics.median(a), "max", a[-1])
    for r in sorted(rows, key=lambda r: -abs(r["dmag"]))[:6]:
        print(" ", r)
