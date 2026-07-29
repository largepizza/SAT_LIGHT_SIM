#!/usr/bin/env python3
"""Verify GpuCloudParams (C++) and the CloudParams UBO block (GLSL) agree field-for-field.

These two are a hand-maintained mirror: GLSL and C++ cannot share a declaration, so this is the
one place a CloudParams mismatch can still hide now that the GLSL side lives in a shared header.

The failure mode this exists to catch is a PERMUTATION, not a size change. Appending a field in a
different position in each file leaves the total size identical, so the C++ static_assert still
passes and nothing fails to compile - every field from the divergence point onward simply reads
its neighbour's value. That shipped once: flatSunGainScale read a pad (0, so clouds rendered
black) while flatCoverageScale read 4.0 (so cloud coverage quadrupled and swallowed the Earth).

Usage:  python tools/check_cloud_params.py       (exit 0 = match, 1 = mismatch)
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CPP = ROOT / "src" / "simulations" / "SatelliteSim.h"
GLSL = ROOT / "shaders" / "include" / "cloud_params.glsl"

# Matches "<type> <name>;" and "<type> <name>[<anything>];" - the array bound may be a named
# constant (kNumCloudLayers) on the C++ side and a literal on the GLSL side.
FIELD = re.compile(r"^\s+\w[\w:]*\s+(\w+)\s*(?:\[[^\]]*\])?\s*;", re.M)


def fields(path: Path, pattern: str) -> list[str]:
    body = re.search(pattern, path.read_text(encoding="utf-8"), re.S)
    if not body:
        sys.exit(f"could not locate the struct/block in {path}")
    return FIELD.findall(body.group(1))


def main() -> None:
    cpp = fields(CPP, r"struct GpuCloudParams\s*\{(.*?)\n\};")
    glsl = fields(GLSL, r"uniform CloudParams \{(.*?)\n\} cloud;")

    if cpp == glsl:
        print(f"OK - {len(cpp)} fields, identical order")
        return

    print(f"MISMATCH  C++={len(cpp)} fields  GLSL={len(glsl)} fields\n")
    for i in range(max(len(cpp), len(glsl))):
        a = cpp[i] if i < len(cpp) else "<missing>"
        b = glsl[i] if i < len(glsl) else "<missing>"
        if a != b:
            print(f"  [{i:2d}]  C++ {a:<28} GLSL {b}")
    print("\nFix by making both files declare the fields in the SAME order.")
    print("Prefer appending at the end of both; a size check cannot catch a permutation.")
    sys.exit(1)


if __name__ == "__main__":
    main()
