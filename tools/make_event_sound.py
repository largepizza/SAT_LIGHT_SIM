"""The event window's cue (a placeholder): assets/sound/ui/event.wav.

A low struck gong under a slow fifth that swells and dies away, ~3 s, 44.1 kHz mono 16-bit, peak -6 dBFS. Played on
the UI effects bus (AudioSystem::playSfx: the master and effects volumes). Replace the file with real art at will; this
script only rewrites it.

    python tools/make_event_sound.py
"""
import math
import os
import random
import struct
import wave

RATE = 44100
DUR = 3.2
OUT = os.path.join(os.path.dirname(__file__), "..", "assets", "sound", "ui", "event.wav")


def main():
    random.seed(2037)
    n = int(RATE * DUR)
    # partials of a struck gong (inharmonic) on A1, plus a swelling fifth (A2 + E3) and a faint high shimmer
    gong = [(55.0, 1.0, 2.2), (55.0 * 2.76, 0.45, 1.4), (55.0 * 5.40, 0.22, 0.8), (55.0 * 8.93, 0.10, 0.5)]
    pad = [(110.0, 0.30), (164.81, 0.22), (220.0, 0.12)]
    out = []
    lp = 0.0
    for i in range(n):
        t = i / RATE
        s = 0.0
        strike = min(1.0, t / 0.008)
        for f, a, tau in gong:
            s += a * strike * math.exp(-t / tau) * math.sin(2 * math.pi * f * t + 0.3 * math.sin(2 * math.pi * 0.7 * t))
        swell = math.sin(math.pi * min(1.0, t / DUR)) ** 1.5
        for f, a in pad:
            s += a * swell * math.sin(2 * math.pi * f * t + 0.004 * f * math.sin(2 * math.pi * 0.25 * t))
        lp += 0.02 * (random.uniform(-1.0, 1.0) - lp)  # filtered noise: the air of the hit
        s += 0.25 * lp * math.exp(-t / 0.6)
        fade = min(1.0, (DUR - t) / 0.4)
        out.append(s * fade)
    peak = max(abs(x) for x in out) or 1.0
    g = 0.5 / peak  # -6 dBFS
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with wave.open(OUT, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", int(max(-1.0, min(1.0, x * g)) * 32767)) for x in out))
    print("wrote", os.path.normpath(OUT), f"{DUR:.1f} s")


if __name__ == "__main__":
    main()
