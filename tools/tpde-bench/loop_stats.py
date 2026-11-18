#!/usr/bin/env python3
# Per-loop instruction statistics of an AArch64 object (llvm-objdump -d):
# innermost loops are address ranges [target, branch] of backward branches
# that contain no other such range. Usage: loop_stats.py file.o [function]
import re, subprocess, sys

OBJDUMP = '/usr/lib/llvm-22/bin/llvm-objdump'
LINE = re.compile(r'^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$')
FUNC = re.compile(r'^[0-9a-f]+ <(.*)>:$')
BRANCH = re.compile(r'^(b|b\.\w+|cbz|cbnz|tbz|tbnz)$')

def parse(path):
    out = subprocess.run([OBJDUMP, '-d', '--no-show-raw-insn', path], capture_output=True, text=True).stdout
    funcs, cur = {}, None
    for line in out.splitlines():
        m = FUNC.match(line)
        if m:
            cur = m.group(1)
            funcs[cur] = []
            continue
        m = LINE.match(line)
        if m and cur:
            funcs[cur].append((int(m.group(1), 16), m.group(2), m.group(3)))
    return funcs

def classify(op, args):
    frame = re.search(r'\[(x29|sp)\b', args)
    if op.startswith(('ldr', 'ldp', 'ldur')):
        return 'frame_ld' if frame else 'ld'
    if op.startswith(('str', 'stp', 'stur')):
        return 'frame_st' if frame else 'st'
    if op == 'bl' or op == 'blr':
        return 'call'
    if op in ('mov', 'movk', 'movz', 'movn', 'fmov') and ('#' in args or 'x16' in args):
        return 'const'
    if op.startswith('f') or op in ('scvtf', 'ucvtf'):
        return 'fp'
    if BRANCH.match(op) or op in ('cmp', 'cmn', 'tst', 'ccmp', 'cset', 'csel', 'csinc'):
        return 'branch_cmp'
    return 'alu'

def loops(insns):
    ranges = []
    for addr, op, args in insns:
        if BRANCH.match(op):
            m = re.search(r'\b0x([0-9a-f]+)\b', args)
            if m and int(m.group(1), 16) <= addr:
                ranges.append((int(m.group(1), 16), addr))
    inner = [r for r in ranges if not any(o != r and r[0] <= o[0] and o[1] <= r[1] for o in ranges)]
    return sorted(set(inner))

funcs = parse(sys.argv[1])
want = sys.argv[2] if len(sys.argv) > 2 else None
keys = ['frame_ld', 'frame_st', 'ld', 'st', 'fp', 'alu', 'const', 'branch_cmp', 'call']
print('function | loop | insns | ' + ' | '.join(keys))
for name, insns in funcs.items():
    if want and name != want:
        continue
    for lo, hi in loops(insns):
        body = [(op, args) for addr, op, args in insns if lo <= addr <= hi]
        counts = {k: 0 for k in keys}
        for op, args in body:
            counts[classify(op, args)] += 1
        print(f'{name} | {lo:#x}-{hi:#x} | {len(body)} | ' + ' | '.join(str(counts[k]) for k in keys))
