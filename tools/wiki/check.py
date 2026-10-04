#!/usr/bin/env python3
"""Wiki gate: strict MkDocs build, history-leak lint, nav coverage, inbox report.

    python tools/wiki/check.py            # everything (what CI runs)
    python tools/wiki/check.py --lint     # lint + nav only, no build
    python tools/wiki/check.py --inbox    # list pending notes and the pages they name

Main pages (everything outside wiki/history/) must describe the system as it is now. The lint flags
phrasing that belongs in History (dates, review numbers, "used to", ...). A line that genuinely needs one
(a real-world date, say) is exempted by ending it with  <!-- history-ok -->.
See wiki/development/style-guide.md.
"""
import argparse
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
WIKI = ROOT / "wiki"
INBOX = WIKI / "_inbox"

LEAKS = [
    (r"\b20\d\d-\d\d(-\d\d)?\b", "a date"),
    (r"\b(review|pass|session|round)\s+\d+[a-z]?\b", "a review/pass/session number"),
    (r"\bused to\b", '"used to"'),
    (r"\bno longer\b", '"no longer"'),
    (r"\bfirst cut\b", '"first cut"'),
    (r"\b(was|were|been) (replaced|removed|deleted|changed|fixed|reverted|added)\b", "a change narrative"),
    (r"\breverted\b", '"reverted"'),
    (r"\btried and\b", '"tried and ..."'),
    (r"\buntil (20\d\d|recently|now)\b", '"until ..."'),
    (r"\b(bug ?fix|fixed a|we fixed)\b", "a fix narrative"),
    (r"\bfollow-up #?\d+", "a follow-up number"),
]
# Pages that quote the banned phrasing on purpose (as examples of what not to write).
LINT_EXEMPT = {"development/style-guide.md", "development/wiki.md"}
LEAK_RES = [(re.compile(p, re.IGNORECASE), why) for p, why in LEAKS]


def main_pages():
    for p in sorted(WIKI.rglob("*.md")):
        rel = p.relative_to(WIKI).as_posix()
        if rel.startswith(("_inbox/", "history/")) or rel in LINT_EXEMPT:
            continue
        yield p, rel


def lint():
    problems = 0
    for p, rel in main_pages():
        in_fence = False
        for n, line in enumerate(p.read_text(encoding="utf-8").splitlines(), 1):
            if line.lstrip().startswith(("```", "~~~")):
                in_fence = not in_fence
            if in_fence or "history-ok" in line:
                continue
            for rx, why in LEAK_RES:
                m = rx.search(line)
                if m:
                    print(f"wiki/{rel}:{n}: {why}: ...{line[max(0, m.start() - 30):m.end() + 30].strip()}...")
                    problems += 1
                    break
    return problems


def nav_coverage():
    cfg = (ROOT / "mkdocs.yml").read_text(encoding="utf-8")
    nav = cfg[cfg.index("\nnav:"):]
    listed = set(re.findall(r"^\s*-\s*(?:[^:\n]+:\s*)?([\w/.-]+\.md)\s*$", nav, re.MULTILINE))
    problems = 0
    for p in sorted(WIKI.rglob("*.md")):
        rel = p.relative_to(WIKI).as_posix()
        if rel.startswith("_inbox/"):
            continue
        if rel not in listed:
            print(f"wiki/{rel}: not in mkdocs.yml nav")
            problems += 1
    for rel in sorted(listed):
        if not (WIKI / rel).exists():
            print(f"mkdocs.yml: nav lists missing page {rel}")
            problems += 1
    return problems


def inbox():
    notes = sorted(p for p in INBOX.glob("*.md") if p.name != "README.md")
    print(f"{len(notes)} pending note(s) in wiki/_inbox/")
    for p in notes:
        text = p.read_text(encoding="utf-8")
        m = re.search(r"^pages:\s*\[(.*?)\]", text, re.MULTILINE)
        print(f"  {p.name}: {m.group(1) if m else '(no pages: field)'}")
    return len(notes)


def build():
    cmd = [sys.executable, "-m", "mkdocs", "build", "--strict", "--clean"]
    print("$", " ".join(cmd[1:]))
    return subprocess.call(cmd, cwd=ROOT)


def run():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lint", action="store_true", help="lint and nav only, no build")
    ap.add_argument("--inbox", action="store_true", help="list pending notes and exit")
    a = ap.parse_args()
    if a.inbox:
        inbox()
        return 0
    bad = lint() + nav_coverage()
    if not a.lint and build() != 0:
        bad += 1
    inbox()
    print("OK" if bad == 0 else f"FAILED ({bad} problem(s))")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(run())
