#!/usr/bin/env python3
# Runs every regtest case with LLVM (-O0) and TPDE (-O0, -O2) and compares output.
import os, subprocess, sys, collections, re
root = os.path.join(os.environ.get('TPDE_BENCH_ROOT', os.path.expanduser('~/tpde-draft')), 'qumir')
qumiri = os.path.join(root, 'build/bin/qumiri')
cases = os.path.join(root, 'test/regtest/cases')

def run(args, src, stdin):
    inp = open(stdin, 'rb') if os.path.exists(stdin) else subprocess.DEVNULL
    try:
        p = subprocess.run([qumiri] + args + ['-i', src], stdin=inp, capture_output=True, timeout=60)
        return p.returncode, p.stdout, p.stderr.decode(errors='replace')
    except subprocess.TimeoutExpired:
        return 'timeout', b'', ''

files = []
for d, _, fs in os.walk(cases):
    for f in sorted(fs):
        if f.endswith('.kum') or f.endswith('.oz'):
            files.append(os.path.join(d, f))
files.sort()
only = sys.argv[1:]
stats = collections.Counter()
reasons = collections.Counter()
mismatches = []
for src in files:
    rel = os.path.relpath(src, cases)
    if only and not any(o in rel for o in only):
        continue
    head = open(src, encoding='utf-8', errors='replace').readline()
    if 'disable_exec' in head or 'disable_llvm' in head:
        stats['skipped'] += 1
        continue
    core = ['--core'] if src.endswith('.oz') else []
    stdin = os.path.splitext(src)[0] + '.stdin'
    ref = run(core + ['--jit', '-O0'], src, stdin)
    for opt in ['-O0', '-O2']:
        got = run(core + ['--tpde', opt, '--timings'], src, stdin)
        backend = re.search(r'timings backend=(\S+)', got[2])
        backend = backend.group(1) if backend else 'none'
        for m in re.finditer(r'unsupported\[([^\]]*)\]=(\d+)', got[2]):
            reasons[m.group(1)] += int(m.group(2))
        same = (ref[0] == got[0] and ref[1] == got[1])
        key = f'{opt} {backend} {"ok" if same else "DIFF"}'
        stats[key] += 1
        if not same:
            mismatches.append((rel, opt, backend, ref[0], got[0], got[2][-300:]))
for k, v in sorted(stats.items()):
    print(f'{k}: {v}')
print('fallback reasons (sum over runs):')
for k, v in reasons.most_common():
    print(f'  {k}: {v}')
print('mismatches:')
for m in mismatches:
    print('  ', m[0], m[1], m[2], 'rc', m[3], m[4], repr(m[5][-200:]))
