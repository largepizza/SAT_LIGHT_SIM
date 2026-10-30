#!/usr/bin/env python3
"""List Settings rows whose backing variable nothing reads but the UI's own load / save.

The UI Fixes mega issue (issue_20261029_205641) asked for an audit of "dead settings that point to
nothing or unsupported features". A row is easy to add and easy to orphan: the slider table, the
JSON load pair and the JSON save pair are three separate lists, and a variable can stay in all three
long after whatever read it was removed, so the knob still turns and the value still round-trips while
nothing in the frame depends on it.

A variable counts as a setting when it appears in a load/save pair in SatelliteSimUI.cpp:

    cloudSolarGain = c.value("solar_gain", cloudSolarGain);      // load
    {"solar_gain", cloudSolarGain},                              // save

Its "real" uses are then every other occurrence in src/**/*.cpp, src/**/*.h and shaders/** -- the row
table, the load/save lines and the declaration in SatelliteSim.h are not uses. Zero real uses means the
setting is dead: either wire it to a consumer or delete the row, the load line and the save line.

    python tools/audit_settings.py            # the whole report
    python tools/audit_settings.py --fail     # exit 1 when anything is dead (a CI gate)

Not covered (a known limit, not an oversight): a variable that IS read but only into a shader uniform
the shader itself ignores (`cp.someName = someName;` counts as a use here); and a setting whose only
reader is another dead setting.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
UI = ROOT / "src" / "simulations" / "SatelliteSimUI.cpp"
HEADER = ROOT / "src" / "simulations" / "SatelliteSim.h"
SCAN_DIRS = [ROOT / "src", ROOT / "shaders"]
SCAN_EXT = {".cpp", ".h", ".hpp", ".c", ".cc", ".glsl", ".comp", ".frag", ".vert", ".inc"}

# `name = c.value("key", name)` and `{"key", name}`
LOAD_RE = re.compile(r'(\w+)\s*=\s*c\.value\(\s*"([\w]+)"\s*,\s*(\w+)')
SAVE_RE = re.compile(r'\{\s*"([\w]+)"\s*,\s*(\w+)\s*\}')
# A slider / knob row: `{tx(Txt::SldFoo), &foo, 0.0f, 1.0f, ...}`
ROW_RE = re.compile(r"tx\(\s*Txt::\w+\s*\)\s*,\s*&(\w+)\s*,")
# A member declaration: `float foo = 1.0f;` / `bool foo = false;` / `glm::vec3 foo{0.0f};`
DECL_RE = re.compile(r"^\s*(?:const\s+)?(?:float|double|bool|int|uint32_t|int32_t|std::size_t|glm::\w+)\s+(\w+)\s*(?:=|;|\{)")


def settings_pairs(text: str) -> dict[str, str]:
    """variable -> JSON key, from the load/save pairs (the definition of "this is a setting")."""
    pairs: dict[str, str] = {}
    for m in LOAD_RE.finditer(text):
        var, key, read = m.group(1), m.group(2), m.group(3)
        if var == read:  # `x = c.value("x", x)`: the pair names the variable twice
            pairs[var] = key
    for key, var in SAVE_RE.findall(text):
        pairs.setdefault(var, key)
    return pairs


def row_vars(text: str) -> set[str]:
    return set(ROW_RE.findall(text))


def scan_files() -> list[pathlib.Path]:
    out: list[pathlib.Path] = []
    for d in SCAN_DIRS:
        for p in d.rglob("*"):
            if p.suffix in SCAN_EXT and p.is_file():
                out.append(p)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fail", action="store_true", help="exit 1 when any setting is dead (a CI gate)")
    ap.add_argument("--verbose", action="store_true", help="also list the live settings, with their use counts")
    args = ap.parse_args()

    ui_text = UI.read_text(encoding="utf-8", errors="replace")
    pairs = settings_pairs(ui_text)
    rows = row_vars(ui_text)
    if not pairs:
        print("audit_settings: no settings load/save pairs found -- did the settings code move?", file=sys.stderr)
        return 2

    files = scan_files()
    # Every line of every scanned file, tagged with where it came from (so the UI's own bookkeeping can be excluded).
    lines: list[tuple[pathlib.Path, str]] = []
    for p in files:
        try:
            for line in p.read_text(encoding="utf-8", errors="replace").splitlines():
                lines.append((p, line))
        except OSError:
            continue

    dead: list[tuple[str, str]] = []          # (variable, json key)
    thin: list[tuple[str, str, int]] = []     # a real setting, but not a slider row
    ok: list[tuple[str, str, int]] = []
    for var, key in sorted(pairs.items()):
        word = re.compile(r"\b" + re.escape(var) + r"\b")
        uses = 0
        for p, line in lines:
            if not word.search(line):
                continue
            if p == UI:
                if LOAD_RE.search(line) or SAVE_RE.search(line):      # the load and save lines
                    continue
                if "tx(Txt::" in line and "&" + var in line:          # its row table
                    continue
            if p == HEADER:
                decl = DECL_RE.match(line)
                if decl and decl.group(1) == var:                     # the member declaration itself
                    continue
            uses += 1
        if uses == 0:
            dead.append((var, key))
        elif var in rows:
            ok.append((var, key, uses))
        else:
            thin.append((var, key, uses))

    print(f"{len(pairs)} settings, {len(rows)} slider rows, {len(files)} files scanned")
    if dead:
        print(f"\nDEAD ({len(dead)}) -- a row, a load line and a save line with nothing reading the value:")
        w = max(len(v) for v, _ in dead)
        for var, key in dead:
            print(f"  {var.ljust(w)}  ({key})")
    else:
        print("\nDEAD (0)")
    print(f"\nReal settings that are not slider rows (debug toggles, preset knobs, harness-only): {len(thin)}")
    if args.verbose:
        for var, key, n in thin:
            print(f"  {var}  ({key})  {n} uses")
        print(f"\nLive rows ({len(ok)}):")
        for var, key, n in ok:
            print(f"  {var.ljust(34)} {str(n).rjust(4)} uses  ({key})")
    return 1 if (dead and args.fail) else 0


if __name__ == "__main__":
    sys.exit(main())
