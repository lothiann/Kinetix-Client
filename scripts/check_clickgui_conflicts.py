#!/usr/bin/env python3
"""Detect ClickGUI row-name collisions in clickgui.cpp.

Rows are dispatched by NAME only: the commit / toggle / sync chains are plain
if(str_comp(<row>.pName, "Name") == 0) chains, first match wins. When two
sections use the same row name (e.g. "Main Color" in the ClickGUI settings and
in ArrayAgent), the wrong branch can catch the row.

Reports:
  * duplicate row names across SRow arrays;
  * shadowed branches: the same name checked twice in one dispatch function
    where the first check has no owner guard (the later branch is unreachable);
  * unguarded branches whose name exists in more than one array.

Usage: python scripts/check_clickgui_conflicts.py [--strict] [--file PATH]
Exit code 1 when shadowed branches are found (any finding with --strict).
"""

import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT = os.path.join(
    HERE, '..', 'src', 'game', 'client', 'components', 'kinetix', 'clickgui.cpp'
)

NL = chr(10)
BS = chr(92)
ID_START = set('ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_')
ID_CHARS = ID_START | set('0123456789')
WS = set(' ') | set(chr(9)) | set(chr(10)) | set(chr(13))
DISPATCH_HINTS = (
    'Apply', 'Commit', 'Toggle', 'Visible', 'Sync', 'Click', 'Handle', 'Slider',
    'Input', 'Value', 'Config',
)


def strip_comments(text):
    """Replace comments with spaces, keep string literals and newlines."""
    out = []
    i = 0
    n = len(text)
    in_str = None
    in_line = False
    in_block = False
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ''
        if in_line:
            if c == NL:
                in_line = False
                out.append(c)
            else:
                out.append(' ')
            i += 1
            continue
        if in_block:
            if c == '*' and nxt == '/':
                out.append('  ')
                i += 2
                in_block = False
            else:
                out.append(NL if c == NL else ' ')
                i += 1
            continue
        if in_str:
            out.append(c)
            if c == BS and i + 1 < n:
                out.append(text[i + 1])
                i += 2
                continue
            if c == in_str:
                in_str = None
            i += 1
            continue
        if c == '/' and nxt == '/':
            in_line = True
            out.append('  ')
            i += 2
            continue
        if c == '/' and nxt == '*':
            in_block = True
            out.append('  ')
            i += 2
            continue
        if c == '"' or c == "'":
            in_str = c
        out.append(c)
        i += 1
    return ''.join(out)


def find_matching(text, start, open_ch, close_ch):
    depth = 0
    i = start
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            q = c
            i += 1
            while i < n:
                if text[i] == BS:
                    i += 2
                    continue
                if text[i] == q:
                    i += 1
                    break
                i += 1
            continue
        if c == open_ch:
            depth += 1
        elif c == close_ch:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def read_c_string(text, pos):
    """Read a C string starting at the quote at pos; returns (value, end)."""
    i = pos + 1
    n = len(text)
    out = []
    while i < n:
        c = text[i]
        if c == BS and i + 1 < n:
            out.append(text[i + 1])
            i += 2
            continue
        if c == '"':
            return ''.join(out), i + 1
        out.append(c)
        i += 1
    return ''.join(out), i


def line_no(text, pos):
    return text.count(NL, 0, pos) + 1


def read_ident(text, pos):
    n = len(text)
    i = pos
    while i < n and text[i] in ID_CHARS:
        i += 1
    return text[pos:i], i


def skip_ws(text, pos):
    n = len(text)
    while pos < n and text[pos] in WS:
        pos += 1
    return pos


def parse_arrays(text):
    """Returns [(array_name, body_start, body_end)] for SRow arrays."""
    arrays = []
    i = 0
    n = len(text)
    while True:
        j = text.find('SRow', i)
        if j < 0:
            break
        k = skip_ws(text, j + 4)
        name, k2 = read_ident(text, k)
        if not name:
            i = j + 4
            continue
        k = skip_ws(text, k2)
        if k >= n or text[k] != '[':
            i = j + 4
            continue
        close = find_matching(text, k, '[', ']')
        if close < 0:
            i = j + 4
            continue
        k = skip_ws(text, close + 1)
        if k >= n or text[k] != '=':
            i = j + 4
            continue
        k = skip_ws(text, k + 1)
        if k >= n or text[k] != '{':
            i = j + 4
            continue
        end = find_matching(text, k, '{', '}')
        if end < 0:
            i = j + 4
            continue
        arrays.append((name, k + 1, end))
        i = end
    return arrays


def parse_entries(body, base):
    """[(type, name, absolute_pos)] from an array initializer body."""
    res = []
    i = 0
    n = len(body)
    while True:
        j = body.find('ERowType::', i)
        if j < 0:
            break
        typ, k = read_ident(body, j + len('ERowType::'))
        k = skip_ws(body, k)
        if k < n and body[k] == ',':
            k += 1
        k = skip_ws(body, k)
        if k < n and body[k] == '"':
            name, k = read_c_string(body, k)
            res.append((typ, name, base + j))
        i = k
    return res


def parse_rows(text):
    """name -> {array: [(type, line), ...]}"""
    rows = {}
    for array, start, end in parse_arrays(text):
        for typ, name, pos in parse_entries(text[start:end], start):
            rows.setdefault(name, {}).setdefault(array, []).append(
                (typ, line_no(text, pos))
            )
    return rows


def parse_parents(text, known):
    parents = {}
    i = 0
    while True:
        j = text.find('ERowType::Expandable', i)
        if j < 0:
            break
        k = j + len('ERowType::Expandable')
        k = skip_ws(text, k)
        if k < len(text) and text[k] == ',':
            k += 1
        k = skip_ws(text, k)
        parent = None
        if k < len(text) and text[k] == '"':
            parent, k = read_c_string(text, k)
        end = text.find('}', k)
        if end < 0:
            end = min(len(text), k + 2000)
        seg = text[k:end]
        for arr in known:
            if arr in seg:
                parents.setdefault(arr, parent)
                break
        i = k
    return parents


def find_functions(text):
    """[(name, body_start, body_end)] for top-level definitions."""
    funcs = []
    offset = 0
    for line in text.splitlines(keepends=True):
        if line[:1] and line[0] not in (' ', chr(9), NL, chr(13)) and '(' in line:
            brace = text.find('{', offset, offset + 2000)
            semi = text.find(';', offset, offset + 2000)
            if brace >= 0 and (semi < 0 or brace < semi):
                end = find_matching(text, brace, '{', '}')
                if end > brace:
                    head = line[:line.find('(')]
                    tokens = head.split()
                    name = tokens[-1] if tokens else '?'
                    funcs.append((name, brace + 1, end))
        offset += len(line)
    return funcs


def iter_conditions(body):
    """[(condition_text, relative_offset)] for every if(...) in body."""
    result = []
    i = 0
    n = len(body)
    while True:
        j = body.find('if', i)
        if j < 0:
            break
        after = j + 2
        if j > 0 and (body[j - 1] in ID_CHARS):
            i = after
            continue
        k = skip_ws(body, after)
        if k >= n or body[k] != '(':
            i = after
            continue
        close = find_matching(body, k, '(', ')')
        if close < 0:
            break
        result.append((body[j:close + 1], j))
        i = close + 1
    return result


def split_args(inner):
    args = []
    depth = 0
    cur = []
    for c in inner:
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
        elif c == ',' and depth == 0:
            args.append(''.join(cur).strip())
            cur = []
            continue
        cur.append(c)
    args.append(''.join(cur).strip())
    return args


def checks_in(cond):
    """[(expr, name)] for every str_comp(<expr>.pName, "Name") in cond."""
    out = []
    i = 0
    n = len(cond)
    while True:
        j = cond.find('str_comp', i)
        if j < 0:
            break
        k = skip_ws(cond, j + len('str_comp'))
        if k >= n or cond[k] != '(':
            i = j + len('str_comp')
            continue
        close = find_matching(cond, k, '(', ')')
        if close < 0:
            break
        args = split_args(cond[k + 1:close])
        if len(args) >= 2:
            expr = args[0]
            val = args[1]
            name = None
            if val.startswith('"'):
                name, _ = read_c_string(val, 0)
            if name is not None and (
                expr == 'pName'
                or expr.endswith('.pName')
                or expr.endswith('->pName')
            ):
                out.append((expr, name))
        i = close + 1
    return out


def main():
    ap = argparse.ArgumentParser(description='Detect ClickGUI row-name collisions.')
    ap.add_argument('--file', default=DEFAULT, help='path to clickgui.cpp')
    ap.add_argument('--strict', action='store_true',
                    help='also fail when duplicate names exist')
    args = ap.parse_args()

    path = os.path.normpath(args.file)
    with open(path, 'r', encoding='utf-8') as f:
        text = strip_comments(f.read())

    rows = parse_rows(text)
    known = set()
    for owners in rows.values():
        known.update(owners)
    parents = parse_parents(text, known)

    duplicates = {n: o for n, o in rows.items() if len(o) > 1}

    shadow = []
    ambiguous = []
    for fname, start, end in find_functions(text):
        method = fname.split('::')[-1]
        if not any(h in method for h in ('Apply', 'Commit', 'Toggle')):
            continue
        body = text[start:end]
        first = {}
        parent_guard = None
        for cond, rel in iter_conditions(body):
            line = line_no(text, start + rel)
            checks = checks_in(cond)
            if not checks:
                continue
            guarded = len(checks) > 1
            # Track the enclosing parent-row guard: a bare
            # str_comp(row.pName, "AimBot") == 0 opens a per-parent block and
            # the child checks inside it (pTarget->pName) inherit that parent.
            if not guarded:
                for expr, name in checks:
                    if expr.endswith('row.pName'):
                        parent_guard = name
            for expr, name in checks:
                # Only checks on the SAME expression under the SAME parent can
                # shadow each other (row.pName is the parent, pTarget->pName
                # the clicked child).
                key = (parent_guard, expr, name)
                if key in first and not first[key][1]:
                    shadow.append((fname, name, first[key][0], line))
                if key not in first:
                    first[key] = (line, guarded)
                    if not guarded and len(rows.get(name, {})) > 1:
                        ambiguous.append((fname, line, name))

    print('file: ' + path)
    print('arrays: %d, row names: %d, duplicates: %d'
          % (len(known), len(rows), len(duplicates)))
    print()

    if duplicates:
        print('== duplicate row names ==')
        for name in sorted(duplicates):
            parts = []
            for array in sorted(duplicates[name]):
                typ, line = duplicates[name][array][0]
                parent = parents.get(array)
                label = array + '[' + parent + ']' if parent else array
                parts.append('%s %s@%d' % (label, typ, line))
            print('  "%s": %s' % (name, ', '.join(parts)))
        print()

    if shadow:
        print('== shadowed dispatch branches (later branch unreachable) ==')
        for fname, name, first_line, later_line in shadow:
            print('  %s: "%s" first checked at %d WITHOUT owner guard, '
                  're-checked at %d' % (fname, name, first_line, later_line))
        print()

    if ambiguous:
        print('== unguarded dispatch of a name used in several arrays ==')
        for fname, line, name in ambiguous:
            print('  %s:%d "%s" -> %s'
                  % (fname, line, name, ', '.join(sorted(rows[name]))))
        print()

    failed = bool(shadow) or (args.strict and bool(duplicates))
    if failed:
        print('Error: ClickGUI row-name conflicts found.')
        return 1
    print('Success: no shadowed dispatch branches found.')
    return 0


if __name__ == '__main__':
    sys.exit(main())