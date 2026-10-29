"""Builds the v1.2 tour cinematic (tools/cinematics/tour_v1_2.json) in a LIVE harness app (docs/HARNESS.md).

    python tools/harness/live.py start
    python tools/cinematics/tour_v1_2_build.py new          # an empty cinematic named tour_v1_2
    python tools/cinematics/tour_v1_2_build.py all          # every shot in ORDER (or name shots to append them)
    python tools/harness/live.py send "cine shot 1; cine shot del; cine save tour_v1_2"   # drop the empty first shot

Each shot is placed (a Watch, a bookmark, an issue-report view with `trail=off`, or observer/time), then keyed:
relative pans/zooms from the placed camera, keys sampled from a moving object's az/el (`track`), or a follow shot
(`follow`). Rebuilding a shot appends it; keep the last of each name in ORDER by editing the saved JSON. Issue-report
views need the reports folder (SATLIGHT_REPORTS_DIR, default <repo>/user_data/reports). Previews: `cine preview`.
"""
import json, os, sys, time
REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
D = os.environ.get("SATLIGHT_LIVE_DIR", os.path.join(REPO, "harness_live"))

def send(text, timeout=900):
    name = f"{int(time.time()*1000)}_{os.getpid()}"
    inbox = os.path.join(D, "inbox")
    tmp = os.path.join(inbox, name + ".tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(text.replace("; ", "\n") + "\n")
    os.replace(tmp, os.path.join(inbox, name + ".satcmd"))
    out = os.path.join(D, "outbox", name + ".json")
    t0 = time.time()
    while time.time() - t0 < timeout:
        if os.path.exists(out):
            try:
                with open(out, encoding="utf-8") as f:
                    return json.load(f)["results"]
            except (OSError, json.JSONDecodeError):
                pass
        time.sleep(0.1)
    raise SystemExit("timeout")


R = os.environ.get("SATLIGHT_REPORTS_DIR", os.path.join(REPO, "user_data", "reports")).replace("\\", "/")
CLEAR = "set clouds_v2.coverage 0; set clouds_v2.high_amount 0; set clouds_v2.mid_amount 0"
UNCLEAR = "set clouds_v2.coverage 1; set clouds_v2.high_amount 1; set clouds_v2.mid_amount 1"

# name: dict(place=cmds, dur=s, rate=simrate, moves=[(t, daz, del, fovmul, dlat, dlon, dalt)], post=cmds after keys)
SHOTS = {
    "halo": dict(place="observer lat=48 lon=12 alt=420000; time set 2036-11-22T21:00:00Z; camera az=-25 el=-17 fov=72",
                 dur=12, rate=1, moves=[(0, 0, 0, 1, 0, 0, 0), (12, 30, 2, 0.9, 0, 0, 0)]),
    "coast": dict(place="observer lat=36.05 lon=-121.75 alt=40; time sun 4 setting; camera look sun; camera el=-3 fov=55",
                  dur=12, rate=1, moves=[(0, -10, 0, 1, 0, 0, 0), (12, 6, 1, 0.92, 0, 0, 0)]),
    "alps": dict(place="observer lat=46.55 lon=7.98 agl=1500; time sun 7 rising; camera az=170 el=-7 fov=60",
                 dur=12, rate=1, moves=[(0, 0, 0, 1, 0, 0, 0), (12, 25, 1, 1, 0, 0, 0)]),
    "city": dict(place="observer lat=34.05 lon=-118.45 alt=2500; time sun -25 setting; camera az=80 el=-14 fov=60",
                 dur=12, rate=1, moves=[(0, 0, 0, 1, 0, 0, 0), (12, 0, 1, 1, 0, 0.06, 0)]),
    "anvil": dict(place=f'report load "{R}/issue_20261023_003611" shot=1 settings=off trail=off',
                  dur=12, rate=1, moves=[(0, -6, 0, 1.1, 0, 0, 0), (12, 6, 0, 0.95, 0, 0, 0)]),
    "mammatus": dict(place="bookmark go 5; time pause",
                     dur=12, rate=1, moves=[(0, -20, -4, 1, 0, 0, 0), (12, 20, 6, 1, 0, 0, 0)]),
    "haboob": dict(place="weather event haboob; time pause",
                   dur=12, rate=1, moves=[(0, -8, 0, 1, 0, 0, 0), (12, 8, 2, 1, 0, 0, 0)]),
    "fireline": dict(place=f'report load "{R}/issue_20261028_191813" shot=1 settings=off trail=off',
                     dur=10, rate=1, moves=[(0, -6, 0, 1, 0, 0, 0), (10, 6, 0, 0.9, 0, 0, 0)]),
    "eyewall": dict(place=f'report load "{R}/issue_20261027_213254" shot=3 settings=off trail=off',
                    dur=12, rate=1, moves=[(0, -20, 0, 1, 0, 0, 0), (12, 20, 2, 1, 0, 0, 0)]),
    "cyclone": dict(place="bookmark go 10; time pause",
                    dur=12, rate=60, moves=[(0, 0, 0, 1.15, 0, 0, 0), (12, 0, 0, 0.8, 0, 0, 0)]),
    "liftoff": dict(place="time set 2036-11-22T02:06:00Z; launch next when=day vehicle=starship pad_dist_m=5000; time pause; time add 24; wait 20; launch select kind=rocket; wait 10",
                    track=True, raise_m=120, dur=24, step=2.5, rate=1, fov=18, lead=0.12),
    "jellyfish": dict(place="bookmark go 8; time pause; time add -150",
                      dur=14, rate=15, moves=[(0, -6, -2, 1, 0, 0, 0), (14, 6, 2, 0.9, 0, 0, 0)]),
    "deploy": dict(place="bookmark go 9; time pause",
                   dur=12, rate=1, moves=[(0, 0, 0, 1, 0, 0, 0), (12, 4, 1, 0.8, 0, 0, 0)]),
    "droneship": dict(place="launch droneship day=1 lead=14 dist_m=2500 h=15; time pause; launch select kind=booster; wait 20", track=True,
                      dur=16, step=2, rate=1, fov=14, lead=0.05),
    "catch": dict(place="missions catch; time pause; wait 40", track=True,
                  dur=24, step=3, rate=1, fov=40, lead=0.1),
    "aurora_orbit": dict(place="bookmark go 3; time pause",
                         dur=12, rate=1, moves=[(0, -10, 0, 1, 0, 0, 0), (12, 10, 2, 0.95, 0, 0, 0)]),
    "comet": dict(place="time set 2037-03-17T01:00:00Z; comets watch great; time pause",
                  dur=12, rate=1, moves=[(0, 0, -3, 1.0, 0, 0, 0), (12, 0, 6, 1.15, 0, 0, 0)]),
    "eclipse": dict(place=CLEAR + "; bookmark go 1; time pause; time add -20",
                    dur=14, rate=1, moves=[(0, 0, -20, 4.0, 0, 0, 0), (14, 0, 0, 0.8, 0, 0, 0)], post=UNCLEAR),
    "supernova": dict(place="skyevents watch supernova; time pause",
                      dur=12, rate=1, moves=[(0, 0, -12, 1.3, 0, 0, 0), (12, 0, -7, 1.0, 0, 0, 0)]),
    "fury": dict(place="bookmark go 7; time pause",
                 dur=12, rate=1, moves=[(0, -15, -3, 1, 0, 0, 0), (12, 15, 3, 1, 0, 0, 0)]),
    "reentry": dict(place="time set 2037-07-15T06:30:00Z; kessler wait days=2; kessler reentry watch=1; time pause; wait 40", track=True,
                    dur=16, step=2, rate=1, fov=12, lead=0.0),
    "iss": dict(place="time set 2036-11-22T03:16:00Z; select const \"International Space Station\"", follow=True, dur=14, rate=1,
                offsets=[(0, "follow offset=-110,50,70; follow aim=sat"), (14, "follow offset=-70,95,45; follow aim=sat")]),
    "dead": dict(place="time set 2037-07-16T06:00:00Z; kessler wait days=1.5; kessler select dead formation=1", follow=True, dur=14, rate=1,
                 offsets=[(0, "follow toward=sun dist=150 side=50 lift=-20"), (14, "follow toward=sun dist=115 side=70 lift=-10")]),
    "rings": dict(place="bookmark go 6; time pause; select sat 434774", follow=True, dur=14, rate=1,
                  offsets=[(0, "follow toward=sun dist=1000 side=-40 lift=15"), (14, "follow toward=sun dist=750 side=-15 lift=25")]),
}
ORDER = ["halo", "coast", "alps", "city", "anvil", "mammatus", "haboob", "fireline", "cyclone", "eyewall",
         "liftoff", "jellyfish", "droneship", "catch", "iss", "rings", "aurora_orbit", "comet", "eclipse", "supernova",
         "fury", "dead"]

def state():
    r = send("state")[0]["result"]
    return r


def build(name):
    S = SHOTS[name]
    res = send(S["place"] + "; wait settle; wait 20; state")
    for r in res:
        if not r.get("ok"):
            print("ERR", r.get("cmd"), r.get("error"))
    st = res[-1]["result"]
    cam = st["camera"]; ob = st["observer"]
    az0, el0, fov0 = cam["az_deg"], cam["el_deg"], cam["fov_y_deg"]
    lat0, lon0, alt0 = ob["lat_deg"], ob["lon_deg"], ob["alt_m"]
    cmds = [f"cine shot add {name}", "cine shot simnow", f"cine shot simrate {S['rate']}", "cine shot look"]
    if S.get("follow"):
        send("; ".join(cmds) + "; cine shot follow sel")
        for (t, oc) in S["offsets"]:
            r = send(oc + f"; wait 2; cine key t={t}")
            print("  ", [(x["cmd"], x.get("error")) for x in r if not x.get("ok")])
        send("follow off")
        print(name, "follow keys", len(S["offsets"]))
        return
    if S.get("track"):
        if S.get("raise_m"):
            cmds = [f"observer lat={lat0:.6f} lon={lon0:.6f} agl={S['raise_m']}"] + cmds
        send("; ".join(cmds))
        t, keys = 0.0, []
        while t <= S["dur"] + 1e-6:
            res = send(("time add %g; " % S["step"] if t > 0 else "") + "wait 4; camera look sel; state")
            st = res[-1]["result"]; ok = res[-2].get("ok")
            if not ok:
                print("  track lost at", t, res[-2].get("error")); break
            az, el = st["camera"]["az_deg"], st["camera"]["el_deg"]
            fov = S["fov"]
            send(f"camera az={az:.3f} el={el + S['lead'] * fov:.3f} fov={fov}; cine key t={t:g}")
            keys.append((t, round(az, 2), round(el, 2)))
            t += S["step"]
        print(name, "track keys", keys)
        if S.get("post"): send(S["post"])
        return
    for (t, daz, de, fm, dla, dlo, dal) in S["moves"]:
        if dla or dlo or dal:
            cmds.append(f"observer lat={lat0+dla:.6f} lon={lon0+dlo:.6f} alt={alt0+dal:.2f}")
        cmds.append(f"camera az={az0+daz:.3f} el={el0+de:.3f} fov={fov0*fm:.3f}")
        cmds.append(f"cine key t={t}")
    cmds += S.get("post", "").split("; ") if S.get("post") else []
    res = send("; ".join(cmds))
    bad = [r for r in res if not r.get("ok")]
    print(name, "az/el/fov", az0, el0, fov0, "at", lat0, lon0, alt0, "errors:", [(b["cmd"], b["error"]) for b in bad])


if __name__ == "__main__":
    names = sys.argv[1:]
    if names == ["new"]:
        print(send("cine new tour_v1_2")[0]["result"]["message"])
        sys.exit()
    for n in (ORDER if names == ["all"] else names):
        build(n)
    print(send("cine save tour_v1_2")[0]["result"]["message"])
