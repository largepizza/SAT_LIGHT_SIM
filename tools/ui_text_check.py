#!/usr/bin/env python3
"""Check the UI text table (src/text/ui_text_en.inc) against the code.

    python tools/ui_text_check.py [--prune]

- every string is plain ASCII (the font atlas is ASCII 32-126);
- every tx(Txt::Id) / txc(Txt::Id) in the code names an entry, and every entry is used (--prune drops unused ones);
- where an entry is a printf format (snprintf, uiKitStr, wxStr, ... with tx(Txt::Id) as the format), the number of
  arguments after it matches the number of conversions (a reworded string that lost or gained a %s would otherwise
  print garbage or crash: MSVC cannot check a format that is not a literal).
Exit 1 on any error. See docs/UI_TEXT.md.
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INC = os.path.join(ROOT, 'src', 'text', 'ui_text_en.inc')
FMT_CALLS = {'snprintf': 2, 'uiKitStr': 0, 'wxStr': 0, 'msStr': 0, 'skStr': 0, 'str': 0, 'fmt': 0}
SPEC = re.compile(r'%[-+ 0#]*(\d+|\*)?(\.(\d+|\*))?(hh|h|ll|l|z|j|t|L)?([diouxXeEfFgGaAcspn%])')


def conversions(fmt):
    n = 0
    for m in SPEC.finditer(fmt):
        if m.group(5) == '%':
            continue
        n += 1
        if m.group(1) == '*':
            n += 1
        if m.group(3) == '*':
            n += 1
    return n


def split_args(s, i):
    """s[i] == '(' -> list of top-level argument strings."""
    depth, start, out, k = 0, i + 1, [], i
    in_str = None
    while k < len(s):
        c = s[k]
        if in_str:
            if c == '\\':
                k += 2
                continue
            if c == in_str:
                in_str = None
        elif c in '"\'':
            in_str = c
        elif c in '([{':
            depth += 1
        elif c in ')]}':
            depth -= 1
            if depth == 0:
                out.append(s[start:k])
                return out
        elif c == ',' and depth == 1:
            out.append(s[start:k])
            start = k + 1
        k += 1
    return out


def main():
    prune = '--prune' in sys.argv
    lines = open(INC, encoding='utf-8').read().split('\n')
    table = {}
    errors = 0
    for ln, l in enumerate(lines, 1):
        m = re.match(r'\s*UI_TEXT\((\w+),\s*"((?:[^"\\]|\\.)*)"\)\s*$', l)
        if m:
            if m.group(1) in table:
                print(f'ui_text_en.inc:{ln}: duplicate id {m.group(1)}')
                errors += 1
            table[m.group(1)] = m.group(2)
            if any(ord(c) > 126 for c in m.group(2)) or re.search(r'\\x[89a-fA-F][0-9a-fA-F]', m.group(2)):
                print(f'ui_text_en.inc:{ln}: {m.group(1)}: not plain ASCII')
                errors += 1
        elif l.strip() and not l.strip().startswith('//'):
            print(f'ui_text_en.inc:{ln}: not a UI_TEXT line')
            errors += 1
    used = set()
    checked = 0
    files = glob.glob(os.path.join(ROOT, 'src', '**', '*.cpp'), recursive=True) + \
        glob.glob(os.path.join(ROOT, 'src', '**', '*.h'), recursive=True)
    for f in files:
        try:
            s = open(f, encoding='utf-8').read()
        except UnicodeDecodeError:
            continue
        rel = os.path.relpath(f, ROOT)
        s = re.sub(r'//[^\n]*', lambda m: ' ' * len(m.group(0)), s)   # comments (keeps offsets)
        for m in re.finditer(r'\bTxt::(\w+)\b', s):
            tid = m.group(1)
            used.add(tid)
            if tid not in table and tid != "Count":
                print(f'{rel}:{s.count(chr(10), 0, m.start()) + 1}: unknown id {tid}')
                errors += 1
        for name, fmt_i in FMT_CALLS.items():
            for m in re.finditer(r'\b' + name + r'\s*\(', s):
                args = split_args(s, m.end() - 1)
                if len(args) <= fmt_i:
                    continue
                fm = re.fullmatch(r'\s*tx\(Txt::(\w+)\)\s*', args[fmt_i])
                if not fm or fm.group(1) not in table:
                    continue
                checked += 1
                want = conversions(table[fm.group(1)].replace('\\"', ''))
                have = len(args) - fmt_i - 1
                if want != have:
                    print(f'{rel}:{s.count(chr(10), 0, m.start()) + 1}: {fm.group(1)} has {want} conversions, '
                          f'{have} arguments')
                    errors += 1
    unused = [t for t in table if t not in used]
    if unused:
        print(f'{len(unused)} unused entries' + ('' if prune else ': ' + ', '.join(unused[:40])))
        if prune:
            keep = [l for l in lines if not (re.match(r'\s*UI_TEXT\((\w+),', l) and
                                             re.match(r'\s*UI_TEXT\((\w+),', l).group(1) in unused)]
            open(INC, 'w', encoding='utf-8', newline='').write('\n'.join(keep))
            print('pruned')
    print(f'{len(table)} entries, {checked} format call sites checked, {errors} errors')
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
