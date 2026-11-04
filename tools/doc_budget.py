#!/usr/bin/env python3
"""Keep what Claude Code loads into every request small (2026-11-03). Standard library only.

CLAUDE.md and every file it imports (`@path` lines) are sent with every request of every session and every
subagent. Until 2026-11-03 CLAUDE.md alone was 1.15 MB (~290k tokens). This fails (exit 1) when:

  - CLAUDE.md + its imports exceed the budget (default 32 KB),
  - a docs/subsystems/*.md file is not named in AGENTS.md's map (§4), or
  - CLAUDE.md / AGENTS.md has grown a subsystem section (a `## Subsystem:` heading).

    python tools/doc_budget.py            # check + list sizes
    python tools/doc_budget.py --budget 40000
"""
import argparse
import glob
import os
import re
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def imports(path, seen):
    """Files loaded with `path`: itself plus its @imports, recursively (outside code fences)."""
    if path in seen or not os.path.isfile(path):
        return
    seen.append(path)
    fence = False
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line.lstrip().startswith("```"):
                fence = not fence
            m = None if fence else re.match(r"\s*@(\S+)\s*$", line)
            if m:
                imports(os.path.normpath(os.path.join(os.path.dirname(path), os.path.expanduser(m.group(1)))), seen)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--budget", type=int, default=32 * 1024, help="bytes for CLAUDE.md + imports")
    a = ap.parse_args()
    bad = []
    loaded = []
    imports(os.path.join(REPO, "CLAUDE.md"), loaded)
    total = 0
    print("loaded into every request:")
    for p in loaded:
        n = os.path.getsize(p)
        total += n
        print(f"  {n:>8,}  {os.path.relpath(p, REPO)}")
        with open(p, encoding="utf-8") as f:
            if re.search(r"^## Subsystem:", f.read(), re.M):
                bad.append(f"{os.path.relpath(p, REPO)} has a '## Subsystem:' section: move it to docs/subsystems/")
    print(f"  {total:>8,}  total (budget {a.budget:,}, ~{total // 4:,} tokens)")
    if total > a.budget:
        bad.append(f"always-loaded docs are {total:,} bytes, over the {a.budget:,} budget")
    agents = os.path.join(REPO, "AGENTS.md")
    amap = open(agents, encoding="utf-8").read() if os.path.isfile(agents) else ""
    print("docs/subsystems (read on demand):")
    for p in sorted(glob.glob(os.path.join(REPO, "docs", "subsystems", "*.md"))):
        name = os.path.basename(p)
        listed = f"`{name}`" in amap
        print(f"  {os.path.getsize(p):>8,}  {name}{'' if listed else '   <- not in the AGENTS.md map'}")
        if not listed:
            bad.append(f"docs/subsystems/{name} is not in AGENTS.md's map (§4)")
    for b in bad:
        print("FAIL: " + b)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
