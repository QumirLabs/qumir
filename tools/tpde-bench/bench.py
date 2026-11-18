#!/usr/bin/env python3
# Compile-time / object-size / execution benchmark: LLVM vs TPDE backends of qumiri.
import hashlib, json, os, re, statistics, subprocess, sys

HOME = os.environ.get('TPDE_BENCH_ROOT', os.path.expanduser('~/tpde-draft'))
QUMIRI = os.environ.get('QUMIRI', os.path.join(HOME, 'qumir/build/bin/qumiri'))
QDB_MOD = ['--module', os.path.join(HOME, 'qdb-oz/modules/qumirdb.oz')]
# qumirdb.oz with qdb_alloc/qdb_free bound to malloc/free (prepare.sh)
QDB_STUB = ['--module', os.path.join(HOME, 'qdb-oz/modules-stub/qumirdb.oz')]
RUNS = int(os.environ.get('RUNS', '9'))
KINDS = os.environ.get('KINDS', 'compile,exec').split(',')
CFGS = [c for c in os.environ.get('CFGS', '').split(',') if c]
OUT = os.environ.get('OUT', os.path.join(HOME, 'bench_results.json'))
PIN = ['taskset', '-c', '3']

PROGS = [
    # name, path, core, executable, extra args
    ('cg', 'progs/exec/cg.kum', False, True, []),
    ('gauss', 'progs/exec/gauss.kum', False, True, []),
    ('gmres', 'progs/exec/gmres.kum', False, True, []),
    ('heat3d', 'progs/exec/heat3d.kum', False, True, []),
    ('heat3d_implicit', 'progs/exec/heat3d_implicit.kum', False, True, []),
    ('lanczos2d', 'progs/exec/lanczos2d.kum', False, True, []),
    ('matmat', 'progs/exec/matmat.kum', False, True, []),
    ('poisson3d', 'progs/exec/poisson3d.kum', False, True, []),
    ('poisson3d_gs', 'progs/exec/poisson3d_gs.kum', False, True, []),
    ('strings', 'qumir/test/regtest/cases/math/strings.kum', False, True, []),
    ('dec_agg.oz', 'progs/dec_agg.oz', True, True, []),
    ('heapsort.oz', 'progs/heapsort.oz', True, True, []),
    ('qdb_all.oz', 'progs/qdb_all.oz', True, False, QDB_MOD),
    ('qdb_sort_strings.oz', 'progs/k_sort_strings.oz', True, False, QDB_MOD),
    ('qdb_row_table.oz', 'progs/k_row_table.oz', True, False, QDB_MOD),
    # qdb kernels with a driver main (prepare.sh)
    ('q_rh_agg.oz', 'progs/q_rh_agg.oz', True, True, QDB_STUB),
    ('q_row_agg.oz', 'progs/q_row_agg.oz', True, True, QDB_STUB),
    ('q_decimal.oz', 'progs/q_decimal.oz', True, True, QDB_STUB),
    ('q_radix.oz', 'progs/q_radix.oz', True, True, QDB_STUB),
    # explicit qdb vectors: <vec u8 16> case mapping, <vec u64 4> Bloom probe
    ('q_upper.oz', 'progs/q_upper.oz', True, True, QDB_STUB),
    ('q_bloom_scalar.oz', 'progs/q_bloom_scalar.oz', True, True, QDB_STUB),
    ('q_bloom_vector.oz', 'progs/q_bloom_vector.oz', True, True, QDB_STUB),
]

COMPILE_CFGS = [
    ('llvm-O0', ['-O0'], {}),
    ('llvm-O0-fastisel', ['-O0'], {'QUMIR_LLVM_CODEGEN_NONE': '1'}),
    ('llvm-O2', ['-O2'], {}),
    ('llvm-O3', ['-O3'], {}),
    ('tpde-O0', ['--tpde', '-O0'], {}),
    ('tpde-O2', ['--tpde', '-O2'], {}),
]
EXEC_CFGS = [
    ('llvm-O0', ['--jit', '-O0'], {}),
    ('llvm-O0-fastisel', ['--jit', '-O0'], {'QUMIR_LLVM_CODEGEN_NONE': '1'}),
    ('llvm-O2', ['--jit', '-O2'], {}),
    ('llvm-O2-fastisel', ['--jit', '-O2'], {'QUMIR_LLVM_CODEGEN_NONE': '1'}),
    ('tpde-O0', ['--tpde', '-O0'], {}),
    ('tpde-O2', ['--tpde', '-O2'], {}),
]
# Run only when named in CFGS: LLVM without Advanced SIMD (vectors scalarized).
EXTRA_EXEC_CFGS = [
    ('llvm-O0-noneon', ['--jit', '-O0'], {'QUMIR_LLVM_FEATURES': '-neon'}),
    ('llvm-O0-fastisel-noneon', ['--jit', '-O0'], {'QUMIR_LLVM_CODEGEN_NONE': '1', 'QUMIR_LLVM_FEATURES': '-neon'}),
    ('llvm-O1-noneon', ['--jit', '-O1'], {'QUMIR_LLVM_FEATURES': '-neon'}),
    ('llvm-O2-noneon', ['--jit', '-O2'], {'QUMIR_LLVM_FEATURES': '-neon'}),
]

def run(prog, args, env_extra):
    name, path, core, _, extra = prog
    cmd = PIN + [QUMIRI] + (['--core'] if core else []) + extra + args + ['--timings', '-i', os.path.join(HOME, path)]
    env = dict(os.environ, **env_extra)
    p = subprocess.run(cmd, capture_output=True, env=env, timeout=600)
    err = p.stderr.decode(errors='replace')
    m = re.search(r'^timings (.*)$', err, re.M)
    if not m:
        raise RuntimeError(f'{name} {args}: no timings, rc={p.returncode}: {err[-500:]}')
    t = {}
    for kv in re.findall(r'(\w+)=(\S+)', m.group(1)):
        k, v = kv
        try:
            t[k] = float(v)
        except ValueError:
            t[k] = v
    t['unsupported'] = re.findall(r'unsupported\[([^\]]*)\]=(\d+)', m.group(1))
    t['stdout_md5'] = hashlib.md5(p.stdout).hexdigest()
    t['rc'] = p.returncode
    return t

def text_size(path):
    out = subprocess.run(['/usr/lib/llvm-22/bin/llvm-size', '-A', path], capture_output=True, text=True).stdout
    total = 0
    for line in out.splitlines():
        parts = line.split()
        if parts and parts[0].startswith('.text'):
            total += int(parts[1])
    return total

results = []
only = sys.argv[1:]
for prog in PROGS:
    if only and prog[0] not in only:
        continue
    for cfg, args, env in COMPILE_CFGS if 'compile' in KINDS else []:
        if CFGS and cfg not in CFGS:
            continue
        runs = [run(prog, ['--compile-only'] + args, env) for _ in range(RUNS)]
        obj = f'/tmp/bench_{prog[0]}_{cfg}.o'
        dump = dict(env, QUMIR_LLVM_DUMP_OBJECT=obj, QUMIR_TPDE_DUMP_OBJECT=obj)
        run(prog, ['--compile-only'] + args, dump)
        rec = {'prog': prog[0], 'kind': 'compile', 'cfg': cfg, 'runs': runs, 'text_size': text_size(obj)}
        results.append(rec)
        print(json.dumps({k: v for k, v in rec.items() if k != 'runs'}), runs[0].get('backend'), flush=True)
    if not prog[3] or 'exec' not in KINDS:
        continue
    for cfg, args, env in EXEC_CFGS + [c for c in EXTRA_EXEC_CFGS if c[0] in CFGS]:
        if CFGS and cfg not in CFGS:
            continue
        try:
            runs = [run(prog, args, env) for _ in range(max(7, RUNS - 2))]
        except RuntimeError as e:
            print(prog[0], cfg, 'failed:', str(e)[-200:], flush=True)
            continue
        rec = {'prog': prog[0], 'kind': 'exec', 'cfg': cfg, 'runs': runs}
        results.append(rec)
        print(prog[0], cfg, runs[0].get('backend'), runs[0]['stdout_md5'], flush=True)

with open(OUT, 'w') as f:
    json.dump(results, f)
