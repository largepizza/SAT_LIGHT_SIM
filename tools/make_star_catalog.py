#!/usr/bin/env python3
"""
make_star_catalog.py - builds assets/stars/star_catalog.bin, the sky's star catalogue (2026-10-27).

Source: the HYG database v4.1 (Astronomy Nexus, https://github.com/astronexus/HYG-Database, CC BY-SA 4.0), itself a
compilation of Hipparcos (ESA 1997 / van Leeuwen 2007), the Yale Bright Star Catalogue and Gliese, with the IAU WGSN
proper names. Every star brighter than VMAG_LIMIT (Sol left out), brightest first.

    python tools/make_star_catalog.py            # downloads the CSV into build/star_cache/ on first use

The app reads the file with SatelliteSim::loadStarCatalog (SatelliteSimStars.cpp); without it it falls back to the
compiled-in BSC5 set (src/simulations/star_catalog.h, no names).

Format (little-endian):
  header  'SLSC', u32 version (1), u32 count, u32 stringBytes
  count x 48-byte records:
     f32 raDeg, decDeg (J2000), vmag, bv, distPc (0 = unknown)
     u32 hip, hr (0 = none)
     u32 nameOff, desigOff, spectOff  (into the string blob; 0 = "")
     char con[4] (IAU abbreviation in 0..2; byte 3: flags, bit 0 = a known variable)
     u32 conNameOff (the constellation's name)
  stringBytes of NUL-terminated ASCII strings (offset 0 is the empty string)
"""
import csv
import os
import struct
import sys
import urllib.request

VMAG_LIMIT = 8.5
URL = "https://raw.githubusercontent.com/astronexus/HYG-Database/main/hyg/CURRENT/hygdata_v41.csv"
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CACHE = os.path.join(ROOT, "build", "star_cache", "hyg.csv")
OUT = os.path.join(ROOT, "assets", "stars", "star_catalog.bin")

# Overrides: Betelgeuse at 168 pc (Joyce et al. 2020, ApJ 902, 63: 168 +27/-15 pc from its pulsations and evolution;
# Hipparcos' parallax of an extended, spotted disc gives ~150-200 pc). The scripted 2037 supernova uses the same.
DIST_OVERRIDE_HIP = {27989: 168.0}
# HYG carries an odd spectral type for Capella (a G3III + G8III giant pair).
SPECT_OVERRIDE_HIP = {24608: "G3III+G8III"}

GREEK = {"Alp": "Alpha", "Bet": "Beta", "Gam": "Gamma", "Del": "Delta", "Eps": "Epsilon", "Zet": "Zeta", "Eta": "Eta",
         "The": "Theta", "Iot": "Iota", "Kap": "Kappa", "Lam": "Lambda", "Mu": "Mu", "Nu": "Nu", "Xi": "Xi",
         "Omi": "Omicron", "Pi": "Pi", "Rho": "Rho", "Sig": "Sigma", "Tau": "Tau", "Ups": "Upsilon", "Phi": "Phi",
         "Chi": "Chi", "Psi": "Psi", "Ome": "Omega"}

# IAU constellations: abbreviation -> (name, genitive)
CON = {
    "And": ("Andromeda", "Andromedae"), "Ant": ("Antlia", "Antliae"), "Aps": ("Apus", "Apodis"),
    "Aql": ("Aquila", "Aquilae"), "Aqr": ("Aquarius", "Aquarii"), "Ara": ("Ara", "Arae"), "Ari": ("Aries", "Arietis"),
    "Aur": ("Auriga", "Aurigae"), "Boo": ("Bootes", "Bootis"), "CMa": ("Canis Major", "Canis Majoris"),
    "CMi": ("Canis Minor", "Canis Minoris"), "CVn": ("Canes Venatici", "Canum Venaticorum"),
    "Cae": ("Caelum", "Caeli"), "Cam": ("Camelopardalis", "Camelopardalis"), "Cap": ("Capricornus", "Capricorni"),
    "Car": ("Carina", "Carinae"), "Cas": ("Cassiopeia", "Cassiopeiae"), "Cen": ("Centaurus", "Centauri"),
    "Cep": ("Cepheus", "Cephei"), "Cet": ("Cetus", "Ceti"), "Cha": ("Chamaeleon", "Chamaeleontis"),
    "Cir": ("Circinus", "Circini"), "Cnc": ("Cancer", "Cancri"), "Col": ("Columba", "Columbae"),
    "Com": ("Coma Berenices", "Comae Berenices"), "CrA": ("Corona Australis", "Coronae Australis"),
    "CrB": ("Corona Borealis", "Coronae Borealis"), "Crt": ("Crater", "Crateris"), "Cru": ("Crux", "Crucis"),
    "Crv": ("Corvus", "Corvi"), "Cyg": ("Cygnus", "Cygni"), "Del": ("Delphinus", "Delphini"),
    "Dor": ("Dorado", "Doradus"), "Dra": ("Draco", "Draconis"), "Equ": ("Equuleus", "Equulei"),
    "Eri": ("Eridanus", "Eridani"), "For": ("Fornax", "Fornacis"), "Gem": ("Gemini", "Geminorum"),
    "Gru": ("Grus", "Gruis"), "Her": ("Hercules", "Herculis"), "Hor": ("Horologium", "Horologii"),
    "Hya": ("Hydra", "Hydrae"), "Hyi": ("Hydrus", "Hydri"), "Ind": ("Indus", "Indi"),
    "LMi": ("Leo Minor", "Leonis Minoris"), "Lac": ("Lacerta", "Lacertae"), "Leo": ("Leo", "Leonis"),
    "Lep": ("Lepus", "Leporis"), "Lib": ("Libra", "Librae"), "Lup": ("Lupus", "Lupi"), "Lyn": ("Lynx", "Lyncis"),
    "Lyr": ("Lyra", "Lyrae"), "Men": ("Mensa", "Mensae"), "Mic": ("Microscopium", "Microscopii"),
    "Mon": ("Monoceros", "Monocerotis"), "Mus": ("Musca", "Muscae"), "Nor": ("Norma", "Normae"),
    "Oct": ("Octans", "Octantis"), "Oph": ("Ophiuchus", "Ophiuchi"), "Ori": ("Orion", "Orionis"),
    "Pav": ("Pavo", "Pavonis"), "Peg": ("Pegasus", "Pegasi"), "Per": ("Perseus", "Persei"),
    "Phe": ("Phoenix", "Phoenicis"), "Pic": ("Pictor", "Pictoris"), "PsA": ("Piscis Austrinus", "Piscis Austrini"),
    "Psc": ("Pisces", "Piscium"), "Pup": ("Puppis", "Puppis"), "Pyx": ("Pyxis", "Pyxidis"),
    "Ret": ("Reticulum", "Reticuli"), "Scl": ("Sculptor", "Sculptoris"), "Sco": ("Scorpius", "Scorpii"),
    "Sct": ("Scutum", "Scuti"), "Ser": ("Serpens", "Serpentis"), "Sex": ("Sextans", "Sextantis"),
    "Sge": ("Sagitta", "Sagittae"), "Sgr": ("Sagittarius", "Sagittarii"), "Tau": ("Taurus", "Tauri"),
    "Tel": ("Telescopium", "Telescopii"), "TrA": ("Triangulum Australe", "Trianguli Australis"),
    "Tri": ("Triangulum", "Trianguli"), "Tuc": ("Tucana", "Tucanae"), "UMa": ("Ursa Major", "Ursae Majoris"),
    "UMi": ("Ursa Minor", "Ursae Minoris"), "Vel": ("Vela", "Velorum"), "Vir": ("Virgo", "Virginis"),
    "Vol": ("Volans", "Volantis"), "Vul": ("Vulpecula", "Vulpeculae"),
}


def ascii(s):
    return s.encode("ascii", "replace").decode("ascii")


def designation(row):
    con = row["con"]
    gen = CON.get(con, (None, con))[1]
    b = row["bayer"]
    if b:
        g, _, n = b.partition("-")
        return f"{GREEK.get(g, g)}{n} {gen}"
    if row["flam"]:
        return f"{row['flam']} {gen}"
    if row["hr"]:
        return f"HR {row['hr']}"
    if row["hip"]:
        return f"HIP {row['hip']}"
    if row["hd"]:
        return f"HD {row['hd']}"
    return ""


def main():
    if not os.path.exists(CACHE):
        os.makedirs(os.path.dirname(CACHE), exist_ok=True)
        print("downloading", URL)
        urllib.request.urlretrieve(URL, CACHE)
    rows = []
    with open(CACHE, encoding="utf-8") as f:
        for r in csv.DictReader(f):
            if r["id"] == "0":
                continue
            try:
                m = float(r["mag"])
            except ValueError:
                continue
            if m > VMAG_LIMIT:
                continue
            rows.append((m, r))
    rows.sort(key=lambda x: (x[0], int(x[1]["id"])))

    blob = bytearray(b"\0")
    cache = {"": 0}

    def s(x):
        x = ascii(x.strip())
        if x not in cache:
            cache[x] = len(blob)
            blob.extend(x.encode("ascii") + b"\0")
        return cache[x]

    recs = bytearray()
    named = 0
    for m, r in rows:
        ra = float(r["ra"]) * 15.0
        dec = float(r["dec"])
        try:
            bv = float(r["ci"])
        except ValueError:
            bv = 0.65
        hip = int(r["hip"]) if r["hip"] else 0
        hr = int(r["hr"]) if r["hr"] else 0
        dist = float(r["dist"]) if r["dist"] else 0.0
        if dist <= 0.0 or dist >= 99999.0:
            dist = 0.0
        dist = DIST_OVERRIDE_HIP.get(hip, dist)
        name = r["proper"]
        named += 1 if name else 0
        flags = 1 if r["var"] else 0
        con = r["con"].encode("ascii")[:3].ljust(3, b"\0") + bytes([flags])
        con_name = CON.get(r["con"], ("", ""))[0]
        recs += struct.pack("<5f2I3I4sI", ra, dec, m, bv, dist, hip, hr, s(name), s(designation(r)),
                            s(SPECT_OVERRIDE_HIP.get(hip, r["spect"])), con, s(con_name))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "wb") as f:
        f.write(b"SLSC" + struct.pack("<3I", 1, len(rows), len(blob)))
        f.write(recs)
        f.write(blob)
    print(f"{OUT}: {len(rows)} stars to mag {VMAG_LIMIT}, {named} with proper names, "
          f"{os.path.getsize(OUT) / 1e6:.2f} MB")


if __name__ == "__main__":
    sys.exit(main())
