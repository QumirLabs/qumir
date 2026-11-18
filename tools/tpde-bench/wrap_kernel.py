#!/usr/bin/env python3
# Joins qdb kernel library files (each a top-level (block ...)) into one
# program: pragmas, (use qumirdb), types, then the functions and an empty main
# (none if one of the files, e.g. a driver given first, defines <main>).
# Duplicate top-level definitions (same head and name) are kept once.
import sys, re

def top_forms(text):
    """Yields the top-level forms inside the outermost (block ...)."""
    i = text.index('(block') + len('(block')
    depth, start, n = 0, None, len(text)
    while i < n:
        c = text[i]
        if c == ';':
            i = text.index('\n', i) if '\n' in text[i:] else n
            continue
        if c == '"':
            i += 1
            while text[i] != '"':
                i += 2 if text[i] == '\\' else 1
        elif c == '#' and text[i + 1:i + 2] == '\\':
            i += 3
            continue
        elif c == '(':
            if depth == 0:
                start = i
            depth += 1
        elif c == ')':
            if depth == 0:
                return
            depth -= 1
            if depth == 0:
                yield text[start:i + 1]
        i += 1

# --keep f1,f2: of the files after the first (the driver), keep only the
# functions named here.
keep = None
args = sys.argv[1:]
if args and args[0] == '--keep':
    keep = set(args[1].split(','))
    args = args[2:]
order = {'pragma': 0, 'use': 1, 'type': 2}
forms, seen = [(1, -1, '(use qumirdb)')], {('use', 'qumirdb)')}
for n, path in enumerate(args):
    for f in top_forms(open(path).read()):
        m = re.match(r'\((\S+)\s+(\S+)', f)
        head = m.group(1) if m else ''
        if keep is not None and n > 0 and head == 'fun' and m.group(2) not in keep:
            continue
        key = (head, m.group(2) if m else f) if head in ('pragma', 'use', 'type') else (None, f)
        if key in seen:
            continue
        seen.add(key)
        forms.append((order.get(head, 3), len(forms), f))
forms.sort()
body = [f for _, _, f in forms]
print('(block')
for f in body:
    print('  ' + f)
if not any(f.startswith('(fun <main>') for f in body):
    print('  (fun <main> () (block))')
print(')')
