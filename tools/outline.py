#!/usr/bin/env python3
"""Outline a big file with line ranges, so an agent reads only the part it needs (2026-11-03).

    python tools/outline.py shaders/cloud_v2_march.comp            # top-level functions / blocks
    python tools/outline.py src/simulations/SatelliteSim.cpp -g launch|kessler
    python tools/outline.py docs/subsystems/clouds.md               # headings + top-level bold topics
    python tools/outline.py docs/subsystems/clouds.md -g anvil      # only the topics matching a regex
    python tools/outline.py tools/harness/run.py                    # Python defs / classes

Each line is `L<first>-<last> (<lines>)  <item>`; read that range with `sed -n first,lastp` or the Read
tool's offset / limit. Markdown: `#` headings and top-level `- **bold**` bullets (the topics inside the
long subsystem notes). C, C++, GLSL: what opens a block at file scope (functions, structs, classes, data
tables, buffer blocks); namespaces and extern "C" are see-through, class members are listed with
--members. Python: def / class from the ast. Standard library only.
"""
import argparse
import ast
import os
import re
import sys

C_EXT = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hh", ".inl", ".inc", ".glsl", ".comp", ".frag", ".vert",
         ".geom", ".tesc", ".tese", ".js", ".java", ".cs"}


def outline_md(lines, deep=False):
    """# headings (depth 1-6), top-level `- **topic` bullets (9) and, with deep, indented ones (10)."""
    items = []  # (start, depth, text)
    in_code = False
    for i, s in enumerate(lines, 1):
        if s.lstrip().startswith("```"):
            in_code = not in_code
            continue
        if in_code:
            continue
        m = re.match(r"(#{1,6})\s+(.*)", s)
        if m:
            items.append((i, len(m.group(1)), m.group(2).strip()))
            continue
        m = re.match(r"( *)[-*] \*\*(.*)", s)
        if m and (not m.group(1) or deep):
            t = m.group(2).replace("**", "").replace("`", "")
            items.append((i, 9 if not m.group(1) else 10, t.strip()))
    out = []
    for k, (start, depth, text) in enumerate(items):
        end = len(lines)
        for start2, depth2, _ in items[k + 1:]:
            if depth2 <= depth:
                end = start2 - 1
                break
        while end > start and not lines[end - 1].strip():
            end -= 1
        indent = {9: "    - ", 10: "        - "}.get(depth, "  " * (depth - 1) + "#" * depth + " ")
        out.append((start, end, indent + text))
    return out


def _strip_c(s, state):
    """Drop comments, strings and char literals from one line; state['block'] carries /* */ across lines."""
    out = []
    i, n = 0, len(s)
    while i < n:
        if state["block"]:
            j = s.find("*/", i)
            if j < 0:
                return "".join(out)
            state["block"] = False
            i = j + 2
            continue
        c = s[i]
        if s.startswith("//", i):
            break
        if s.startswith("/*", i):
            state["block"] = True
            i += 2
            continue
        if c in "\"'":
            j = i + 1
            while j < n and s[j] != c:
                j += 2 if s[j] == "\\" else 1
            out.append(c + c)
            i = j + 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def _label(sig):
    sig = re.sub(r"\s+", " ", sig).strip()
    sig = re.sub(r"^(template\s*<[^>]*>\s*)", "", sig)
    if re.match(r"(typedef\s+)?(struct|class|union|enum)\b", sig) or re.search(r"\b(struct|class|enum)\s+\w+\s*(:|$)", sig):
        return sig
    if re.match(r"layout\s*\(", sig) or re.search(r"\b(uniform|buffer)\b", sig):
        return sig
    if "=" in sig.split("(")[0]:
        return "data: " + sig
    return sig


def outline_c(lines, members=False):
    state = {"block": False}
    stack = []  # entries: ("see", start) transparent, ("item", start, label, logical_depth), ("inner",)
    depth = 0   # logical brace depth (namespaces do not count)
    pending, pending_start = "", None
    out = []
    for i, raw in enumerate(lines, 1):
        if raw.lstrip().startswith("#"):
            continue
        s = _strip_c(raw, state)
        for ch_i, ch in enumerate(s):
            if ch == "{":
                if depth == 0 or (members and depth == 1 and stack and stack[-1][0] == "item" and
                                  re.match(r"(struct|class|union)\b", stack[-1][2])):
                    sig = (pending + s[:ch_i]).strip()
                    if re.match(r"(namespace\b|extern\s+\"\")", sig) or (sig == "" and depth == 0):
                        stack.append(("see", i))
                        pending, pending_start = "", None
                        continue
                    start = pending_start or i
                    stack.append(("item", start, _label(sig) or "{", depth))
                    depth += 1
                    pending, pending_start = "", None
                else:
                    stack.append(("inner",))
                    depth += 1
            elif ch == "}":
                if not stack:
                    continue
                top = stack.pop()
                if top[0] == "see":
                    continue
                depth -= 1
                if top[0] == "item":
                    out.append((top[1], i, ("    " if top[3] else "") + top[2]))
                if depth == 0:
                    pending, pending_start = "", None
            elif ch == ";" and depth == 0:
                pending, pending_start = "", None
        if depth == 0 or (members and depth == 1):
            tail = s.rsplit("{", 1)[-1].rsplit("}", 1)[-1].rsplit(";", 1)[-1]
            if tail.strip():
                if pending_start is None:
                    pending_start = i
                pending += " " + tail
    out.sort()
    return out


def outline_py(text):
    out = []
    tree = ast.parse(text)
    def walk(nodes, ind):
        for n in nodes:
            if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
                kind = "class " if isinstance(n, ast.ClassDef) else "def "
                start = min([n.lineno] + [d.lineno for d in n.decorator_list])
                out.append((start, n.end_lineno, ind + kind + n.name))
                if isinstance(n, ast.ClassDef):
                    walk(n.body, ind + "    ")
    walk(tree.body, "")
    return out


def main():
    for st in (sys.stdout, sys.stderr):
        try:
            st.reconfigure(encoding="utf-8", errors="replace")
        except AttributeError:
            pass
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file")
    ap.add_argument("-g", "--grep", help="only items whose text matches this regex (case-insensitive)")
    ap.add_argument("--members", action="store_true", help="C/C++: also list members inside structs / classes")
    ap.add_argument("--deep", action="store_true", help="markdown: also list indented bold sub-topics (always on with -g)")
    ap.add_argument("--min", type=int, default=0, help="only items at least this many lines long")
    ap.add_argument("-w", "--width", type=int, default=150, help="truncate each item to this many characters")
    a = ap.parse_args()
    with open(a.file, encoding="utf-8", errors="replace") as f:
        text = f.read()
    lines = text.split("\n")
    ext = os.path.splitext(a.file)[1].lower()
    if ext in (".md", ".markdown"):
        items = outline_md(lines, a.deep or bool(a.grep))
    elif ext == ".py":
        items = outline_py(text)
    elif ext in C_EXT:
        items = outline_c(lines, a.members)
    else:
        sys.exit(f"outline: no outliner for '{ext}' (md, py, C/C++/GLSL)")
    rx = re.compile(a.grep, re.I) if a.grep else None
    shown = 0
    for start, end, label in items:
        if end - start + 1 < a.min or (rx and not rx.search(label)):
            continue
        label = label if len(label) <= a.width else label[:a.width - 3] + "..."
        print(f"L{start}-{end} ({end - start + 1})  {label}")
        shown += 1
    print(f"-- {shown} of {len(items)} items, {len(lines)} lines, {len(text):,} bytes: {a.file}", file=sys.stderr)


if __name__ == "__main__":
    main()
