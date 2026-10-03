# python tools/harness/find_cloud_spots.py [ISO time] [drift rate, rad/s — default 6.5e-6]
# Cloudy benchmark spots for a fixed sim time: samples the 8K cloud map with the same longitude
# drift the shaders use (cloudPhase = fmod(cloudDriftRate * simT, 2pi), map lon = lon + phase).
# Local solar hour = UTC + lon / 15 (the sim's Earth rotation angle is GMST since 2026-10-03).
import math, datetime, os, statistics, sys
from PIL import Image

T = sys.argv[1] if len(sys.argv) > 1 else '2036-11-16T06:26:07'
t = datetime.datetime.fromisoformat(T)
j2000 = datetime.datetime(2000, 1, 1, 12, 0, 0)
simT = (t - j2000).total_seconds()
# sanity: the documented start epoch
assert abs((datetime.datetime(2036, 6, 21) - j2000).total_seconds() - 1150891200) < 1
# SatelliteSim::cloudDriftPhase(): rate x (t - 2036-06-21) + the default offset, which puts the
# default rate's map where rate x (t - J2000) put it. Pass the settings file's clouds.drift_rate.
RATE = float(sys.argv[2]) if len(sys.argv) > 2 else 6.5e-6
phase = math.fmod(RATE * (simT - 1150891200) + 3.6793436499, 2 * math.pi)
print(f'simT {simT:.0f}  phase {math.degrees(phase):.1f} deg')

W, H = 1440, 720
img = Image.open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'assets', 'textures',
                              '8k_earth_clouds.jpg')).convert('L').resize((W, H), Image.BILINEAR)
px = img.load()

def mapval(lat, lon):
    ml = lon + math.degrees(phase)
    u = ((ml + 180.0) / 360.0) % 1.0
    v = (90.0 - lat) / 180.0
    return px[min(int(u * W), W - 1), min(int(v * H), H - 1)] / 255.0

def stats(lat, lon, r=1.0):
    vals = [mapval(lat + dy * r / 3, lon + dx * r / 3 / max(math.cos(math.radians(lat)), 0.2))
            for dy in range(-3, 4) for dx in range(-3, 4)]
    return statistics.mean(vals), statistics.pstdev(vals)

def solar_hour(lon):
    return (t.hour + t.minute / 60 + lon / 15.0) % 24.0

cats = {'overcast_day': [], 'broken_day': [], 'overcast_sunrise': [], 'broken_sunrise': [],
        'broken_night': [], 'tropical_towers_day': []}
for lat in range(-50, 61, 1):
    for lon in range(-180, 180, 1):
        sh = solar_hour(lon)
        m, sd = stats(lat, lon)
        day = 10.0 <= sh <= 15.0
        sunrise = 6.4 <= sh <= 7.3
        night = sh <= 3.0 or sh >= 22.0
        if m > 0.6 and sd < 0.12:
            if day: cats['overcast_day'].append((m, lat, lon, sh))
            if sunrise: cats['overcast_sunrise'].append((m, lat, lon, sh))
        if 0.3 < m < 0.6 and sd > 0.15:
            if day: cats['broken_day'].append((sd, lat, lon, sh))
            if sunrise: cats['broken_sunrise'].append((sd, lat, lon, sh))
            if night: cats['broken_night'].append((sd, lat, lon, sh))
            if day and abs(lat) < 18 and m > 0.4: cats['tropical_towers_day'].append((m, lat, lon, sh))

for k, v in cats.items():
    v.sort(reverse=True)
    picked = []
    for e in v:
        if all(abs(e[1] - p[1]) > 8 or abs(e[2] - p[2]) > 8 for p in picked):
            picked.append(e)
        if len(picked) == 4:
            break
    print(k, [(f'{a:.2f}', la, lo, f'{h:.1f}h') for a, la, lo, h in picked])
