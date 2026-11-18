#!/usr/bin/env python3
# Turns bench_results.json into markdown tables (median, min in brackets).
import json, os, statistics, sys

res = json.load(open(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.environ.get('TPDE_BENCH_ROOT', os.path.expanduser('~/tpde-draft')), 'bench_results.json')))
progs = []
for r in res:
    if r['prog'] not in progs:
        progs.append(r['prog'])

def get(prog, kind, cfg):
    for r in res:
        if r['prog'] == prog and r['kind'] == kind and r['cfg'] == cfg:
            return r
    return None

def backend_ms(t):
    if str(t.get('backend', '')).startswith('tpde'):
        return t['codegen_ms']
    return t['llvm_ir_ms'] + t['llvm_opt_ms'] + t['codegen_ms']

def jit_ms(t):
    if str(t.get('backend', '')).startswith('tpde'):
        return t['codegen_ms'] + t['jit_ms']
    return t['llvm_ir_ms'] + t['llvm_opt_ms'] + t['jit_ms']

def fmt(vals):
    med = statistics.median(vals)
    lo = min(vals)
    p = lambda x: f'{x:.2f}' if x < 10 else f'{x:.1f}' if x < 100 else f'{x:.0f}'
    return f'{p(med)} ({p(lo)})'

def med(vals):
    return statistics.median(vals)

ccfgs = ['llvm-O0', 'llvm-O0-fastisel', 'llvm-O2', 'llvm-O3', 'tpde-O0', 'tpde-O2']
print('### Compile time: Qumir IR -> object in memory, ms, median (min) of runs\n')
print('| program | IR fn/instr | frontend | IR passes | LLVM O0 | LLVM O0 FastISel | LLVM O2 | LLVM O3 | TPDE (O0 IR) | TPDE (O2 IR) | LLVM O0 / TPDE | LLVM O2 / TPDE |')
print('|---|---|---|---|---|---|---|---|---|---|---|---|')
for p in progs:
    row = [p]
    base = get(p, 'compile', 'tpde-O2')
    if not base:
        continue
    t0 = base['runs'][0]
    row.append(f"{int(t0['ir_functions'])}/{int(t0['ir_instructions'])}")
    row.append(fmt([t['frontend_ms'] for t in base['runs']]))
    row.append(fmt([t['ir_pipeline_ms'] for t in base['runs']]))
    meds = {}
    for c in ccfgs:
        r = get(p, 'compile', c)
        vals = [backend_ms(t) for t in r['runs']]
        meds[c] = med(vals)
        cell = fmt(vals)
        if c.startswith('tpde') and not str(r['runs'][0].get('backend')).startswith('tpde'):
            cell += ' fallback'
        row.append(cell)
    row.append(f"{meds['llvm-O0'] / meds['tpde-O2']:.0f}x")
    row.append(f"{meds['llvm-O2'] / meds['tpde-O2']:.0f}x")
    print('| ' + ' | '.join(row) + ' |')

print('\nLLVM column = Qumir IR -> LLVM IR + LLVM passes + object emission; TPDE column = TPDE compile() (adaptor + analysis + codegen into memory). "O0 IR"/"O2 IR": Qumir IR with/without the Qumir IR pipeline (locals2ssa, const_fold, inline).\n')

print('### Code size: .text bytes\n')
print('| program | ' + ' | '.join(ccfgs) + ' |')
print('|---|' + '---|' * len(ccfgs))
for p in progs:
    if not get(p, 'compile', 'tpde-O2'):
        continue
    print('| ' + p + ' | ' + ' | '.join(str(get(p, 'compile', c)['text_size']) for c in ccfgs) + ' |')

ecfgs = ['llvm-O0', 'llvm-O2', 'llvm-O3', 'tpde-O0', 'tpde-O2']
print('\n### Execution of the entry function, ms, median (min)\n')
print('| program | ' + ' | '.join(ecfgs) + ' | output |')
print('|---|' + '---|' * (len(ecfgs) + 1))
for p in progs:
    if not get(p, 'exec', 'llvm-O0'):
        continue
    row = [p]
    hashes = set()
    for c in ecfgs:
        r = get(p, 'exec', c)
        row.append(fmt([t['entry_ms'] for t in r['runs']]))
        hashes.update(t['stdout_md5'] for t in r['runs'])
    row.append('identical' if len(hashes) == 1 else f'{len(hashes)} variants')
    print('| ' + ' | '.join(row) + ' |')

print('\n### JIT path latency (no frontend): backend + JIT/map, ms, median\n')
print('| program | ' + ' | '.join(ecfgs) + ' |')
print('|---|' + '---|' * len(ecfgs))
for p in progs:
    if not get(p, 'exec', 'llvm-O0'):
        continue
    print('| ' + p + ' | ' + ' | '.join(fmt([jit_ms(t) for t in get(p, 'exec', c)['runs']]) for c in ecfgs) + ' |')
