"""(radar x rain_amount, clamped 0..1 like the map) Radar vs the rain at the eye: for each `observer ...` followed by `weather radar` in a run, the radar's centre (the 4
cells round you, 120 m over the smoothed ground) against the lightning pass's rain map at the eye (the same cv2Field,
40-m cells, clamped 0..1). Usage: python tools/harness/radar_agree.py <run folder> (a run of tools/harness/scripts/weather_window.satcmd)"""
import json
import sys

run = sys.argv[1]
obs = None
rows = []
for line in open(run + "/results.jsonl", encoding="utf-8"):
    d = json.loads(line)
    c = d["cmd"]
    if c.startswith("observer"):
        obs = c
    if c == "weather radar" and obs:
        r = d["result"]["radar"]
        rows.append((obs, min(r["centre_rain"] * r.get("rain_amount", 1.0), 1.0), r.get("eye_rain_map", -1), r.get("eye_rain_map_max", -1), r["gpu_ms"], r.get("cpu_ms", -1)))
        obs = None
print("%-44s %8s %8s %8s %7s %7s" % ("where", "radar", "eye", "eye_max", "gpu_ms", "cpu_ms"))
for o, a, b, m, g, cp in rows:
    print("%-44s %8.3f %8.3f %8.3f %7.3f %7.2f" % (o.replace("observer ", ""), a, b, m, g, cp))
