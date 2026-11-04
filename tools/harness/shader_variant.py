#!/usr/bin/env python3
"""Build a text-substituted shader variant for an in-app A/B (`shaders reload`), 2026-11-03. Stdlib only.

    python tools/harness/shader_variant.py march --tag nofar -s "if (farK > 0.0)=>if (false)"
    python tools/harness/shader_variant.py sky_taa --tag g4 -s "const int kGodReads = 8;=>const int kGodReads = 4;"
    python tools/harness/shader_variant.py march --tag probe -f subs.txt -D CV2_EVAL_COUNT
    python tools/harness/shader_variant.py march --tag base         # no substitutions = the current source

<slot> picks the shader and its build defines; it is also the `shaders reload` keyword:
  march (cloud_v2_march.comp), resolve, lightning, composite (cloud_march.comp), weather, depth (scene_depth.comp),
  sky (sat_sky.frag), sky_taa (sat_sky.frag -DSKY_TAA). Any other shader: --src shaders/x.comp (prints no reload line).

Each `-s OLD=>NEW` (or each `OLD=>NEW` line of -f, `\\n` for a newline) must match EXACTLY ONCE across the shader
and every header in shaders/include (pass --all to replace every match). The variant is compiled with the build's
own flags (glslc -I <include copy> [defines]) into build/<config>/shaders/v_<tag>_<slot>.spv, and the script prints
the line to send, e.g. `shaders reload march=shaders/v_nofar_march.spv`. Interleave variants in ONE live app (the
perf-ab skill); `shaders reload` reports the pipeline's register count, which decides more than the code does.
"""
import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SLOTS = {
    "march": ("cloud_v2_march.comp", []), "resolve": ("cloud_v2_resolve.comp", []),
    "lightning": ("cloud_v2_lightning.comp", []), "composite": ("cloud_march.comp", []),
    "weather": ("cloud_v2_weather.comp", []), "depth": ("scene_depth.comp", []),
    "sky": ("sat_sky.frag", []), "sky_taa": ("sat_sky.frag", ["-DSKY_TAA"]),
}


def find_glslc():
    for p in [shutil.which("glslc"), os.path.join(os.environ.get("VULKAN_SDK", ""), "Bin", "glslc.exe"),
              os.path.join(os.environ.get("VULKAN_SDK", ""), "bin", "glslc")]:
        if p and os.path.isfile(p):
            return p
    sys.exit("shader_variant: glslc not found (install the Vulkan SDK, set VULKAN_SDK)")


def parse_sub(s):
    if "=>" not in s:
        sys.exit(f"shader_variant: substitution needs OLD=>NEW: {s!r}")
    old, new = s.split("=>", 1)
    return old.replace("\\n", "\n"), new.replace("\\n", "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("slot", help="|".join(SLOTS) + " (or 'custom' with --src)")
    ap.add_argument("--tag", required=True, help="variant name, part of the output file name")
    ap.add_argument("--src", help="shader source (overrides the slot's)")
    ap.add_argument("-s", "--sub", action="append", default=[], help="OLD=>NEW (repeatable)")
    ap.add_argument("-f", "--file", help="file of OLD=>NEW lines (# comments ignored)")
    ap.add_argument("-D", "--define", action="append", default=[], help="extra -D for glslc")
    ap.add_argument("--all", action="store_true", help="replace every match, not exactly one")
    ap.add_argument("--config", default="Release")
    a = ap.parse_args()

    if a.src:
        src, defines = a.src, []
    elif a.slot in SLOTS:
        name, defines = SLOTS[a.slot]
        src = os.path.join(REPO, "shaders", name)
    else:
        sys.exit(f"shader_variant: unknown slot {a.slot!r}; use one of {', '.join(SLOTS)} or --src")
    subs = [parse_sub(s) for s in a.sub]
    if a.file:
        with open(a.file, encoding="utf-8") as f:
            subs += [parse_sub(l.rstrip("\n")) for l in f if l.strip() and not l.lstrip().startswith("#")]

    tmp = tempfile.mkdtemp(prefix="shvar_")
    try:
        inc_src = os.path.join(REPO, "shaders", "include")
        inc = os.path.join(tmp, "include")
        shutil.copytree(inc_src, inc)
        main_copy = os.path.join(tmp, os.path.basename(src))
        shutil.copy(src, main_copy)
        files = [main_copy] + sorted(glob.glob(os.path.join(inc, "*.glsl")))
        texts = {p: open(p, encoding="utf-8").read() for p in files}
        for old, new in subs:
            hits = [(p, texts[p].count(old)) for p in files if old in texts[p]]
            n = sum(c for _, c in hits)
            if n == 0:
                sys.exit(f"shader_variant: not found anywhere: {old[:120]!r}")
            if n > 1 and not a.all:
                where = ", ".join(f"{os.path.basename(p)} x{c}" for p, c in hits)
                sys.exit(f"shader_variant: {n} matches ({where}) for {old[:80]!r}; make it unique or pass --all")
            for p, _ in hits:
                texts[p] = texts[p].replace(old, new)
            print(f"  {n}x in {', '.join(os.path.basename(p) for p, _ in hits)}: {old[:60]!r} -> {new[:60]!r}")
        for p, t in texts.items():
            with open(p, "w", encoding="utf-8", newline="\n") as f:
                f.write(t)
        out_dir = os.path.join(REPO, "build", a.config, "shaders")
        if not os.path.isdir(out_dir):
            sys.exit(f"shader_variant: {out_dir} missing - build first")
        slot = a.slot if a.slot in SLOTS else re.sub(r"\W+", "_", os.path.basename(src))
        out = os.path.join(out_dir, f"v_{a.tag}_{slot}.spv")
        cmd = [find_glslc(), "-I", inc] + defines + [f"-D{d}" for d in a.define] + [main_copy, "-o", out]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            err = (r.stderr or r.stdout).replace(tmp, "<variant>")
            sys.exit("shader_variant: glslc failed:\n" + "\n".join(err.splitlines()[:30]))
        print(f"built {os.path.relpath(out, REPO)} ({os.path.getsize(out):,} B)")
        if a.slot in SLOTS:
            print(f"send: shaders reload {a.slot}=shaders/{os.path.basename(out)}")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
