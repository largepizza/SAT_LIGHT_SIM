#!/usr/bin/env python3
"""Move user-facing string literals from C++ UI code into the UI text table (src/text/ui_text_en.inc).

    python tools/ui_text_migrate.py [--dry] FILE...      migrate the known UI call sites in FILE(s)
    python tools/ui_text_migrate.py --report FILE...     count the user-facing literals left per file

A literal is moved when it is an argument of a known UI call at a text position (uiButton's label and tip,
uiKV's label and value, ui.tooltip's text, a toast snprintf's format, CLAY_TEXT(CLAY_STRING(...)), the first member
of a brace initializer such as a CloudSlider row...) and it looks like text (a word of two letters, not an id like
"FollowAimBtn" or a key like "photometry.glare_gain"). It is replaced by tx(Txt::<Id>) and appended to the table
(an identical string reuses its id). Review the diff: the script knows call shapes, not meaning.
See docs/UI_TEXT.md.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INC = os.path.join(ROOT, 'src', 'text', 'ui_text_en.inc')

# call name -> {arg index: kind}
CALLS = {
    'uiButton': {4: 'Btn', 5: 'Tip'},
    'uiDangerButton': {4: 'Btn', 5: 'Tip'},
    'uiToggleRow': {4: 'Lbl', 6: 'Tip', 7: 'Lbl', 8: 'Lbl'},
    'uiChoiceRow': {4: 'Lbl', 8: 'Tip'},
    'uiKV': {2: 'Lbl', 3: 'Val'},
    'uiSection': {2: 'Sec'},
    'uiStatTile': {2: 'Lbl', 3: 'Val'},
    'tooltip': {2: 'Tip'},
    'uiKitStr': {0: 'Fmt'},
    'infoSection': {None: 'Sec'},
    'buildResizableWindow': {4: 'Ttl'},
    'chipButton': {1: 'Btn', 3: 'Tip'},
    'button': {1: 'Btn'},
    'bootStatus': {0: 'Boot'},
    'pill': {2: 'Btn', 5: 'Tip'},
    'stepper': {1: 'Lbl', 8: 'Tip'},
    'CLOUD_SEC': {0: 'Sec'},
    'infoKV': {1: 'Lbl'},
    'num': {1: 'Lbl', 7: 'Tip'},
    'wxStr': {0: 'Fmt'},
    'cs': {0: 'Note'},
    'text': {0: 'Note'},
    'card': {2: 'Lbl', 4: 'Val', 5: 'Val', 6: 'Val'},
    'msStr': {0: 'Fmt'},
    'skStr': {0: 'Fmt'},
}
ASSIGN_ALL = '--assign' in sys.argv
# --range FILE:L1-L2 (repeatable): every text literal in those lines (surgical, after reading them)
RANGES = {}
for _a in sys.argv:
    if _a.startswith('--range='):
        _f, _r = _a[8:].rsplit(':', 1)
        _l1, _l2 = _r.split('-')
        RANGES.setdefault(os.path.basename(_f), []).append((int(_l1), int(_l2)))   # x = "text"; and return "text"; (UI files only)
SNPRINTF_ALL = '--snprintf' in sys.argv   # every snprintf format with a space in it (UI files only)
SKIP_BUF = re.compile(r'(?i)(log|path|key|file|json|dir|^id|Id$|name$)')
TOAST_BUFS = ('screenshotToastText',)
# brace initializers here are data (roster, keybinding actions persisted in settings.json, planet names), not UI
# char arrays that are data (keys, file names, ids), never shown
DATA_ARRAYS = ('kOrder', 'kKeys', 'kCapNames', 'kBucketKeys', 'kCpuBucketKeys', 'kParts', 'kSdf', 'kBin', 'kKindName',
               'kKinds', 'iconPaths', 'kNames', 'kCls')
NO_BRACE_FILES = ('SatelliteSim.cpp', 'SatelliteSimPlanets.cpp', 'SatelliteSimHarness.cpp')

LIT = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
FMT_SPEC = re.compile(r'%[-+ 0#]*(\d+|\*)?(\.(\d+|\*))?(ll|l|z|h)?[a-zA-Z%]')


def looks_like_text(s):
    if not s:
        return False
    t = FMT_SPEC.sub(' ', s)
    if not re.search(r'[A-Za-z]{2}', t):
        return False
    if ' ' not in s:
        if '_' in s or '.' in s and not s.endswith('.'):
            return False
        if re.fullmatch(r'[A-Z][a-z0-9]+([A-Z][A-Za-z0-9]*)+', s):  # FollowAimBtn
            return False
        if re.fullmatch(r'[a-z][A-Za-z0-9]*', s):  # identifiers, keys
            return False
        if s.endswith(('.png', '.json', '.wav', '.spv', '.jpg', '.csv', '.mp4', '.txt', '.zip')):
            return False
    if s.startswith(('assets/', 'shaders/', 'data/')):
        return False
    return True


def make_id(kind, text, taken):
    t = FMT_SPEC.sub(' ', text)
    t = t.replace("\\n", ' ').replace("'", '')
    words = re.findall(r'[A-Za-z0-9]+', t)
    name = ''.join(w[:1].upper() + w[1:].lower() if not w.isupper() or len(w) > 1 and not w.isalpha() else w.capitalize()
                   for w in words[:6])
    name = re.sub(r'^[0-9]+', '', name)[:40] or 'Text'
    base = kind + name
    i, cand = 2, base
    while cand in taken:
        cand = f'{base}{i}'
        i += 1
    return cand


def load_table():
    ids, by_text, order = {}, {}, []
    if os.path.exists(INC):
        for line in open(INC, encoding='utf-8'):
            m = re.match(r'\s*UI_TEXT\((\w+),\s*"((?:[^"\\]|\\.)*)"\)', line)
            if m:
                ids[m.group(1)] = m.group(2)
                by_text.setdefault(m.group(2), m.group(1))
    return ids, by_text


def skip_space_comments(s, i):
    n = len(s)
    while i < n:
        if s[i].isspace():
            i += 1
        elif s.startswith('//', i):
            j = s.find('\n', i)
            i = n if j < 0 else j + 1
        elif s.startswith('/*', i):
            j = s.find('*/', i)
            i = n if j < 0 else j + 2
        else:
            break
    return i


def scan(s):
    """Yield (kind, start, end) for code tokens: 'str', 'chr', 'id', 'p' (punct)."""
    i, n = 0, len(s)
    while i < n:
        c = s[i]
        if c.isspace():
            i += 1
            continue
        if s.startswith('//', i):
            j = s.find('\n', i)
            i = n if j < 0 else j
            continue
        if s.startswith('/*', i):
            j = s.find('*/', i)
            i = n if j < 0 else j + 2
            continue
        if c == '#':  # preprocessor line
            j = s.find('\n', i)
            while j > 0 and s[j - 1] == '\\':
                j = s.find('\n', j + 1)
            i = n if j < 0 else j
            continue
        if c == '"' or (c == 'R' and s.startswith('R"', i)):
            if c == 'R':
                m = re.match(r'R"([^(]*)\(', s[i:])
                end = s.find(')' + m.group(1) + '"', i)
                yield ('raw', i, end + len(m.group(1)) + 2)
                i = end + len(m.group(1)) + 2
                continue
            j = i + 1
            while j < n and s[j] != '"':
                j += 2 if s[j] == '\\' else 1
            yield ('str', i, j + 1)
            i = j + 1
            continue
        if c == "'":
            j = i + 1
            while j < n and s[j] != "'":
                j += 2 if s[j] == '\\' else 1
            yield ('chr', i, j + 1)
            i = j + 1
            continue
        if c.isalpha() or c == '_':
            j = i
            while j < n and (s[j].isalnum() or s[j] == '_'):
                j += 1
            yield ('id', i, j)
            i = j
            continue
        yield ('p', i, i + 1)
        i += 1


def literal_groups(toks, a, b):
    """Adjacent string literal runs among toks[a:b] -> list of (start_tok, end_tok)."""
    out, k = [], a
    while k < b:
        if toks[k][0] == 'str':
            m = k
            while m + 1 < b and toks[m + 1][0] == 'str':
                m += 1
            out.append((k, m))
            k = m + 1
        else:
            k += 1
    return out


def process(path, ids, by_text, dry=False, report=False):
    s = open(path, encoding='utf-8').read()
    toks = list(scan(s))
    n = len(toks)
    # matching brackets
    match = {}
    stack = []
    for k, (kind, a, b) in enumerate(toks):
        if kind == 'p' and s[a] in '([{':
            stack.append(k)
        elif kind == 'p' and s[a] in ')]}':
            if stack:
                match[stack.pop()] = k
    repl = {}  # (start, end) -> (kind, text)

    def args_of(open_k):
        close_k = match.get(open_k)
        if close_k is None:
            return []
        out, start, depth = [], open_k + 1, 0
        for k in range(open_k + 1, close_k):
            kind, a, b = toks[k]
            if kind == 'p' and s[a] in '([{':
                depth += 1
            elif kind == 'p' and s[a] in ')]}':
                depth -= 1
            elif kind == 'p' and s[a] == ',' and depth == 0:
                out.append((start, k))
                start = k + 1
        out.append((start, close_k))
        return out

    def take(a_k, b_k, kind, need_space=False):
        # only literals at this argument's top level (not inside a nested call's own parentheses)
        depth = 0
        k = a_k
        while k < b_k:
            tk, a, b = toks[k]
            if tk == 'p' and s[a] in '([{':
                depth += 1
            elif tk == 'p' and s[a] in ')]}':
                depth -= 1
            if tk == 'str' and depth == 0:
                m = k
                while m + 1 < b_k and toks[m + 1][0] == 'str':
                    m += 1
                text = ''.join(s[toks[q][1] + 1:toks[q][2] - 1] for q in range(k, m + 1))
                if looks_like_text(text) and (not need_space or ' ' in text.strip()):
                    # never a literal that is compared / used as a key: preceded by == or followed by ==
                    repl.setdefault((toks[k][1], toks[m][2]), (kind, text))
                k = m + 1
                continue
            k += 1

    def line_of(pos):
        ls = s.rfind('\n', 0, pos) + 1
        le = s.find('\n', pos)
        return s[ls:le]

    for k in range(n):
        kind, a, b = toks[k]
        if kind == 'id' and k + 1 < n and toks[k + 1][0] == 'p' and s[toks[k + 1][1]] == '(':
            name = s[a:b]
            if name in CALLS:
                args = args_of(k + 1)
                spec = CALLS[name]
                for ai, (x, y) in enumerate(args):
                    kd = spec.get(ai) or spec.get(None)
                    if kd:
                        take(x, y, kd, need_space=(name in ('uiKitStr', 'wxStr', 'msStr', 'skStr')))
            elif name == 'snprintf':
                args = args_of(k + 1)
                if len(args) >= 3:
                    x, y = args[0]
                    first = s[toks[x][1]:toks[y - 1][2]] if y > x else ''
                    buf = first.strip()
                    if buf in TOAST_BUFS:
                        take(args[2][0], args[2][1], 'Toast')
                    elif SNPRINTF_ALL and not SKIP_BUF.search(buf.split('[')[0].split('.')[-1].split('>')[-1]) \
                            and not re.search(r'Log::|r\["message"\]|harness::|R\.', s[toks[k][2]:toks[match.get(k + 1, k)][2] + 260]):
                        take(args[2][0], args[2][1], 'Fmt', need_space=True)
            elif name == 'CLAY_STRING' and k >= 2 and not (toks[k - 1][0] == 'p' and s[toks[k - 1][1]] == '(' and
                                                       toks[k - 2][0] == 'id' and s[toks[k - 2][1]:toks[k - 2][2]].startswith('CLAY_')
                                                       and s[toks[k - 2][1]:toks[k - 2][2]] != 'CLAY_TEXT'):
                o = k + 1
                c = match.get(o)
                if c and c > o + 1 and all(toks[q][0] == 'str' for q in range(o + 1, c)):
                    text = ''.join(s[toks[q][1] + 1:toks[q][2] - 1] for q in range(o + 1, c))
                    if looks_like_text(text):
                        repl.setdefault((toks[k][1], toks[c][2]), ('Note', text, 'clay'))
            elif name == 'CLAY_TEXT':
                # CLAY_TEXT(CLAY_STRING("..."), ...)
                if k + 3 < n and toks[k + 2][0] == 'id' and s[toks[k + 2][1]:toks[k + 2][2]] == 'CLAY_STRING':
                    o = k + 3
                    c = match.get(o)
                    if c == o + 2 and toks[o + 1][0] == 'str':
                        text = s[toks[o + 1][1] + 1:toks[o + 1][2] - 1]
                        if looks_like_text(text):
                            repl[(toks[k + 2][1], toks[c][2])] = ('Note', text, 'clay')
        # x = "text"; / x = c ? "a" : "b"; / return "text";
        if ASSIGN_ALL and ((kind == 'p' and s[a] == '=' and k >= 1 and toks[k - 1][0] in ('id', 'p')
                            and not (toks[k - 1][0] == 'p' and s[toks[k - 1][1]] in '=!<>+-*/')
                            and not (k + 1 < n and toks[k + 1][0] == 'p' and s[toks[k + 1][1]] == '='))
                           or (kind == 'id' and s[a:b] == 'return')):
            # the statement up to ';' at depth 0, not crossing a '{'
            q, depth = k + 1, 0
            while q < n:
                tq, qa, qb = toks[q]
                if tq == 'p' and s[qa] in '([':
                    depth += 1
                elif tq == 'p' and s[qa] in ')]':
                    depth -= 1
                elif tq == 'p' and s[qa] in ';{}' and depth <= 0:
                    break
                q += 1
            if q < n and s[toks[q][1]] == ';':
                stmt = s[toks[k][2]:toks[q][1]]
                lhs = s[s.rfind('\n', 0, a) + 1:a]
                if 'json' not in lhs and '[' not in lhs.split('=')[-1][-3:] and 'j[' not in lhs and 'key' not in lhs.lower() \
                        and 'path' not in lhs.lower() and 'id' != lhs.strip().split(' ')[-1]:
                    take(k + 1, q, 'Val')
        # an option list: const char *kX[] = {"Off", "Low", ...} (not constexpr)
        if kind == 'p' and s[a] == '{' and k >= 3 and s[toks[k - 1][1]] == '=' and s[toks[k - 2][1]] == ']':
            ls = s.rfind('\n', 0, a) + 1
            decl = s[ls:a]
            vname = s[toks[k - 4][1]:toks[k - 4][2]] if k >= 4 else ''
            m_ = re.search(r'(\w+)\s*\[[^\]]*\]\s*$', decl)
            vname = m_.group(1) if m_ else vname
            if 'char' in decl and 'constexpr' not in decl and os.path.basename(path) not in NO_BRACE_FILES \
                    and vname not in DATA_ARRAYS:
                c = match.get(k)
                if c:
                    for (q0, q1) in literal_groups(toks, k + 1, c):
                        text = ''.join(s[toks[q][1] + 1:toks[q][2] - 1] for q in range(q0, q1 + 1))
                        if looks_like_text(text):
                            repl.setdefault((toks[q0][1], toks[q1][2]), ('Opt', text))
        # brace initializer whose first member is a literal: {"Label", ...}
        if kind == 'p' and s[a] == '{' and k + 2 < n and toks[k + 1][0] == 'str' and toks[k + 2][0] == 'p' \
                and s[toks[k + 2][1]] == ',':
            text = s[toks[k + 1][1] + 1:toks[k + 1][2] - 1]
            ln = line_of(a)
            base = os.path.basename(path)
            if looks_like_text(text) and 'constexpr' not in ln and 'GLFW_KEY' not in ln and 'j[' not in ln \
                    and '{{"' not in ln and base not in NO_BRACE_FILES:
                # a static constexpr table above us?
                back = s.rfind(';', 0, a)
                decl = s[back:a]
                if 'constexpr' not in decl:
                    repl.setdefault((toks[k + 1][1], toks[k + 1][2]), ('Sld' if '&' in s[toks[k + 2][2]:toks[k + 2][2] + 4] else 'Lbl', text))

    if RANGES.get(os.path.basename(path)):
        for (l1, l2) in RANGES[os.path.basename(path)]:
            for (q0, q1) in literal_groups(toks, 0, n):
                ln = s.count('\n', 0, toks[q0][1]) + 1
                if l1 <= ln <= l2:
                    text = ''.join(s[toks[q][1] + 1:toks[q][2] - 1] for q in range(q0, q1 + 1))
                    before = s[max(0, toks[q0][1] - 30):toks[q0][1]]
                    if looks_like_text(text) and not re.search(r'(CLAY_\w*\(|Log::\w+\()\s*$', before):
                        repl.setdefault((toks[q0][1], toks[q1][2]), ('Txt', text))
    if report:
        if report == 'list':
            for (a, b), v in sorted(repl.items()):
                print(f'{path}:{s.count(chr(10), 0, a) + 1}: [{v[0]}] {v[1]}')
        return len(repl)
    if not repl:
        return 0
    out = s
    taken = set(ids)
    new_lines = []
    for (a, b), v in sorted(repl.items(), reverse=True):
        kd, text = v[0], v[1]
        if text in by_text:
            tid = by_text[text]
        else:
            tid = make_id(kd, text, taken)
            taken.add(tid)
            ids[tid] = text
            by_text[text] = tid
            new_lines.append((tid, text))
        if len(v) > 2 and v[2] == 'clay':
            out = out[:a] + f'txc(Txt::{tid})' + out[b:]
        else:
            out = out[:a] + f'tx(Txt::{tid})' + out[b:]
    if 'txc(Txt::' in out and 'UiTextClay.h' not in out:
        m = re.search(r'#include "(\.\./)?clay\.h"[^\n]*\n', out)
        if m:
            pre = '../' if os.path.basename(os.path.dirname(os.path.abspath(path))) == 'simulations' else ''
            out = out[:m.end()] + f'#include "{pre}UiTextClay.h"\n' + out[m.end():]
    if not dry:
        open(path, 'w', encoding='utf-8', newline='').write(out)
        with open(INC, 'a', encoding='utf-8', newline='') as f:
            f.write(f'\n// ── {os.path.basename(path)} ──\n')
            for tid, text in reversed(new_lines):
                f.write(f'UI_TEXT({tid}, "{text}")\n')
    return len(repl)


def main():
    argv = sys.argv[1:]
    dry = '--dry' in argv
    report = 'list' if '--list' in argv else ('--report' in argv)
    files = [a for a in argv if not a.startswith('--')]
    ids, by_text = load_table()
    total = 0
    for f in files:
        c = process(f, ids, by_text, dry=dry, report=report)
        total += c
        print(f'{f}: {c}')
    print('total', total)


if __name__ == '__main__':
    main()
