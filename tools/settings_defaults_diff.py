#!/usr/bin/env python3
"""Report every key in a settings.json whose live value differs from the compiled-in default.

Why this exists: the compiled-in defaults (the member initializers in SatelliteSim.h) ARE the
out-of-box look, but the only way to see them is to read them out of the header and compare them
by hand against a settings.json - and there are ~250 keys. This tool walks the load path instead:
applySettingsJson in SatelliteSimUI.cpp reads every persisted key as `member = c.value("key",
member)`, so the second argument of each of those calls names the member whose initializer is the
default. Parse those two files, flatten the settings.json, and diff.

What a MISMATCH means depends on which file you point it at:

  * A tuned settings.json (e.g. build-win-release/Release/settings.json) - hand-tuned values that
    have NOT been promoted into the code. This is how you find out exactly which values would have
    to move for a first run / the FRESH build to reproduce the tuned look. Runtime state (camera,
    observer, window geometry, active tab) shows up here too and is normally correct to ignore.
  * A fresh-written settings.json - anything reported is a default that changed between the release
    that wrote the file and this build.

Usage:  python tools/settings_defaults_diff.py [path/to/settings.json]
        (no argument: build-win-release/Release/settings.json if present, else build/Release/...)
Exit 0 = every key matches the compiled-in default, 1 = at least one mismatch.
"""
import json
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
UI_CPP = ROOT / "src" / "simulations" / "SatelliteSimUI.cpp"

# "member = <container>.value("key", <default>);" - the container is `p`, `c`, `d`, `ctl`,
# `j["hud"]["right_panel"]`, ... i.e. anything ending in `.value(`.
LOAD = re.compile(
    r"([A-Za-z_][\w\.\[\]]*?)\s*=\s*[\w\[\]\"'\.]+?\.value\(\s*\"([^\"]+)\"\s*,\s*(.+?)\)\s*;",
    re.S,
)

# Metadata keys that are written but never loaded. Not tunables - never report them as mismatches.
SKIP_KEYS = {"app_version", "git_commit", "schema_version"}


def f32(x: float) -> float:
    """Round a Python double to the float32 a C++ `0.4f` literal would hold."""
    return struct.unpack("<f", struct.pack("<f", x))[0]


def parse_literal(text: str):
    """Return the value of a C++ literal, or None if it is not a bare literal (calls, exprs)."""
    t = text.strip()
    if t in ("true", "false"):
        return t == "true"
    if (t.startswith('"') and t.endswith('"')) or (t.startswith("'") and t.endswith("'")):
        return t[1:-1]
    t = re.sub(r"(?i)([0-9a-fx\.])(u|l|ll|f)\s*$", r"\1", t)  # 1.0f, 3u, 2LL
    try:
        return int(t, 0)
    except ValueError:
        pass
    try:
        return float(t)
    except ValueError:
        return None


_INIT_CACHE: dict[str, tuple[object, str]] = {}


def _headers():
    """SatelliteSim.h first - it holds almost every tunable - then the rest of src/, A-Z."""
    files = sorted((ROOT / "src").rglob("*.h"), key=lambda p: str(p))
    return sorted(files, key=lambda p: p.name != "SatelliteSim.h")


def _declared_type(owner: str):
    """The type of a dotted container member, e.g. `WindowChrome settingsChrome;` -> WindowChrome."""
    for path in _headers():
        text = path.read_text(encoding="utf-8", errors="replace")
        m = re.search(r"\b([A-Za-z_]\w*)\s+" + re.escape(owner) + r"\b", text)
        if m:
            return m.group(1)
    return None


def _struct_body(type_name: str):
    """(path, line, body) for `struct type_name { ... }`, brace-matched so nested braces survive."""
    if not type_name:
        return None
    for path in (ROOT / "src").rglob("*.h"):
        text = path.read_text(encoding="utf-8", errors="replace")
        m = re.search(r"\b(?:struct|class)\s+" + re.escape(type_name) + r"\s*\{", text)
        if not m:
            continue
        depth, start = 0, m.end() - 1
        for j in range(start, len(text)):
            if text[j] == "{":
                depth += 1
            elif text[j] == "}":
                depth -= 1
                if depth == 0:
                    return path, text[:start].count("\n") + 1, text[start + 1 : j]
    return None


def find_initializer(name: str, owner: str = None):
    """Search the headers for `<type> name = <literal>;` and return (value, location).

    The `(?<![\\w.])` guard keeps `.x = 3;` (a member access) from answering for `x`. For a dotted
    member (settingsChrome.x, camera.azDeg) the owner's own struct body is searched first, so a
    single-letter field answers from its own struct: without that, `x`, `y` and `h` all matched
    fields of unrelated structs in other headers.
    """
    key = f"{owner}.{name}" if owner else name
    if key in _INIT_CACHE:
        return _INIT_CACHE[key]
    # ([^;{},]+) stops at a comma so a multi-declarator line (`float w = 0.0f, h = 0.0f;`) yields
    # the value of the field being searched for rather than the whole tail of the declaration.
    # Every match must be tried, not just the first: a comment that mentions the member by name
    # (`// cloudSurfaceCarve = 0 and ...` in SatelliteSim.h) matches too and is not parseable.
    pattern = re.compile(r"(?<![\w.])" + re.escape(name) + r"(?:\s*\[[^\]]*\])?\s*=\s*(?!=)([^;{},]+)[,;]")

    def scan(text: str):
        for m in pattern.finditer(text):
            value = parse_literal(m.group(1))
            if value is not None:
                return value, m.start()
        return None, 0

    result: tuple[object, str] = (None, "")
    if owner:
        type_name = _declared_type(owner)
        found = _struct_body(type_name)
        if found:
            path, line, body = found
            value, at = scan(body)
            if value is not None:
                result = (value, f"{path.name}:{line + body[:at].count(chr(10))} ({type_name} field)")
    if result[0] is None:
        for path in _headers():
            text = path.read_text(encoding="utf-8", errors="replace")
            value, at = scan(text)
            if value is not None:
                result = (value, f"{path.name}:{text[:at].count(chr(10)) + 1}")
                break
    _INIT_CACHE[key] = result
    return result




def load_path() -> dict:
    """key -> [(member, default, "File.h:line")] for every persisted key in applySettingsJson."""
    src = UI_CPP.read_text(encoding="utf-8", errors="replace")
    entries: dict[str, list] = {}
    for m in LOAD.finditer(src):
        member, key, default_expr = m.group(1), m.group(2), m.group(3)
        if key in SKIP_KEYS:
            continue
        # Some loaders copy into a local first (`float latDeg = j["observer"].value("lat_deg",
        # obsLatDeg);`) or pass a different member than the assignment target (`masterVol_ =
        # a.value("master_vol", masterVol_)`). When the fallback is a bare identifier, THAT is the
        # member whose initializer is the default; only otherwise fall back to the target's leaf.
        fallback = default_expr.strip()
        is_ident = re.fullmatch(r"[A-Za-z_]\w*", fallback) and fallback not in ("true", "false", "nullptr")
        target = fallback if is_ident else member.split(".")[-1]
        owner = None
        # The container comes from the assignment target even when the leaf came from the fallback:
        # `settingsChrome.x = d.value("win_x", settingsChrome.x)` must search WindowChrome's body.
        for candidate in (member, target):
            parts = candidate.split(".")
            if len(parts) == 2 and "[" not in parts[0]:
                owner, target = parts[0], parts[1]
                break
        leaf = re.sub(r"\[.*\]$", "", target)  # ambGroupGain[g] -> ambGroupGain
        # A literal fallback is NOT the compiled-in default - it is a deliberate "absent key"
        # behavior (play_intro_on_startup reads `false` so an upgrade keeps its old behavior while
        # the member's default stays true), so note it rather than reporting it as the default.
        call_literal = parse_literal(default_expr)
        default, where = find_initializer(leaf, owner)
        if default is None:
            default = call_literal
        elif call_literal is not None and call_literal != default:
            where += f" [load call falls back to {call_literal!r}]"
        entries.setdefault(key, []).append((leaf, default, where))
    return entries


def flatten(node, path="") -> list:
    """[(section, dotted key, value)] - objects are descended, arrays are skipped."""
    out = []
    if isinstance(node, dict):
        for k, v in node.items():
            out += flatten(v, path + "." + k if path else k)
    elif isinstance(node, list):
        return []
    else:
        section = path.split(".")[0] if "." in path else "(root)"
        out.append((section, path.rsplit(".", 1)[-1], path, node))
    return out


def matches(live, default) -> str:
    """'exact' | 'rounding' | 'differ' | 'unknown'."""
    if default is None:
        return "unknown"
    if isinstance(live, bool) or isinstance(default, bool):
        return "exact" if live == default else "differ"
    if isinstance(live, (int, float)) and isinstance(default, (int, float)):
        if isinstance(live, int) and isinstance(default, int):
            return "exact" if live == default else "differ"
        if f32(float(live)) == f32(float(default)):
            return "exact"
        if abs(float(live) - float(default)) <= 1e-6 * max(1.0, abs(float(default))):
            return "rounding"
        return "differ"
    return "exact" if str(live) == str(default) else "differ"

def main() -> None:
    if len(sys.argv) > 1:
        target = Path(sys.argv[1])
    else:
        candidates = [ROOT / "build-win-release" / "Release" / "settings.json",
                      ROOT / "build" / "Release" / "settings.json",
                      ROOT / "build" / "Debug" / "settings.json"]
        target = next((c for c in candidates if c.is_file()), candidates[0])
    if not target.is_file():
        sys.exit(f"no settings.json at {target}")

    data = json.loads(target.read_text(encoding="utf-8-sig"))
    known = load_path()

    print(f"{target}\n")

    mismatches, unresolved, rounding, ok = [], [], [], 0
    for section, key, dotted, live in sorted(flatten(data)):
        cands = known.get(key)
        if not cands:
            unresolved.append((dotted, live))
            continue
        verdict, probe = "unknown", None
        for member, default, where in cands:
            v = matches(live, default)
            if v == "exact":
                verdict = "exact"
                break
            if v in ("rounding", "differ") and verdict == "unknown":
                verdict, probe = v, (member, default, where)
        if verdict == "exact":
            ok += 1
        elif verdict == "rounding":
            ok += 1
            rounding.append((dotted, live, probe))
        elif verdict == "differ":
            mismatches.append((section, dotted, live, probe))
        else:
            unresolved.append((dotted, live))

    if mismatches:
        print(f"MISMATCH vs compiled-in default: {len(mismatches)}\n")
        by_section: dict[str, list] = {}
        for section, dotted, live, probe in mismatches:
            by_section.setdefault(section, []).append((dotted, live, probe))
        for section in sorted(by_section):
            print(f"  [{section}]")
            for dotted, live, probe in sorted(by_section[section]):
                member, default, where = probe
                print(f"    {dotted:<38} live {live!r:<24} default {member} = {default!r} ({where})")
            print()
    else:
        print("OK - every persisted key matches the compiled-in default\n")

    if rounding:
        print("Same value, different text (float32 round-trip):")
        for dotted, live, probe in sorted(rounding):
            print(f"    {dotted:<38} live {live!r:<24} default {probe[0]} = {probe[1]!r} ({probe[2]})")
        print()

    print(f"{ok} keys match, {len(mismatches)} differ, {len(unresolved)} unresolved from the load path")
    if unresolved:
        print("\nUnresolved - metadata, loaders with a different shape, or a member whose initializer")
        print("this tool could not find. Check any that look like a tunable by hand:")
        for dotted, live in unresolved:
            print(f"    {dotted} = {live!r}")

    sys.exit(1 if mismatches else 0)


if __name__ == "__main__":
    main()
