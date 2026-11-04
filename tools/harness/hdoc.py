#!/usr/bin/env python3
"""Look things up in docs/HARNESS.md instead of reading all 130 KB of it (2026-11-03). Stdlib only.

    python tools/harness/hdoc.py report          # command-reference rows whose command cell matches
    python tools/harness/hdoc.py "shaders reload" perf
    python tools/harness/hdoc.py -s "Launch spacing"   # one whole ## section
    python tools/harness/hdoc.py -a ghosting     # every line in the manual mentioning it (with line numbers)
    python tools/harness/hdoc.py --list          # section headings + every command name (~4 KB)

The in-app `help` command (kHelp in SatelliteSimHarness.cpp) gives the one-line syntax of every command.
"""
import argparse
import os
import re
import sys

DOC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "docs", "HARNESS.md")


def load():
    with open(DOC, encoding="utf-8") as f:
        return f.read().split("\n")


def sections(lines):
    out, cur = [], None
    for i, s in enumerate(lines):
        if s.startswith("## "):
            if cur:
                out.append(cur)
            cur = [s[3:].strip(), i, i]
        elif cur:
            cur[2] = i
    if cur:
        out.append(cur)
    return out


def rows(lines):
    """(line index, command cell, rest) for every table row whose first cell holds a `command`."""
    for i, s in enumerate(lines):
        if s.startswith("| `"):
            m = re.match(r"\|\s*(.*?)\s*(?<!\\)\|(.*)$", s)
            if m:
                yield i, m.group(1), m.group(2).rstrip(" |")


def main():
    for st in (sys.stdout, sys.stderr):
        try:
            st.reconfigure(encoding="utf-8", errors="replace")
        except AttributeError:
            pass
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("terms", nargs="*", help="match the command cell of reference rows (case-insensitive)")
    ap.add_argument("-s", "--section", help="print the ## section whose heading contains this")
    ap.add_argument("-a", "--all", help="grep the whole manual for this regex")
    ap.add_argument("--list", action="store_true", help="section headings and command names")
    a = ap.parse_args()
    lines = load()
    if a.list:
        for name, s, e in sections(lines):
            print(f"L{s + 1}-{e + 1}  ## {name}")
        names = sorted({re.match(r"`([^`\s]+(?: [a-z_]+)?)", c).group(1) for _, c, _ in rows(lines)
                        if re.match(r"`([^`\s]+)", c)})
        print("\ncommands: " + ", ".join(names))
        return
    if a.section:
        hit = [x for x in sections(lines) if a.section.lower() in x[0].lower()]
        if not hit:
            sys.exit(f"hdoc: no section matching '{a.section}' (--list shows them)")
        for name, s, e in hit:
            print("\n".join(lines[s:e + 1]).rstrip())
        return
    if a.all:
        rx = re.compile(a.all, re.I)
        for i, s in enumerate(lines):
            if rx.search(s):
                print(f"L{i + 1}: {s[:400]}")
        return
    if not a.terms:
        ap.print_help()
        return
    found = 0
    for term in a.terms:
        t = term.lower()
        for i, cmd, rest in rows(lines):
            if t in cmd.lower():
                print(f"L{i + 1}  {cmd}\n      {rest.strip()}\n")
                found += 1
    if not found:
        sec = [x for t in a.terms for x in sections(lines) if t.lower() in x[0].lower()]
        if sec:
            print("no command rows; sections: " + "; ".join(f"L{s + 1} {n}" for n, s, _ in sec)
                  + "  (print one with -s)")
        else:
            print("no match; try -a <regex> or --list")


if __name__ == "__main__":
    main()
