#!/bin/bash
# Prepares $TPDE_BENCH_ROOT/progs for bench.py: scaled copies of the
# test/regtest/cases/math programs, the oz-lang programs of this directory and,
# if QDB_SRC points to a qumirdb checkout, programs built from its kernel
# library (kernel/**/*.oz with modules/qumirdb.oz; only read).
set -e
ROOT=${TPDE_BENCH_ROOT:-$HOME/tpde-draft}
HERE=$(cd "$(dirname "$0")" && pwd)
M=$ROOT/qumir/test/regtest/cases/math
P=$ROOT/progs
mkdir -p $P/exec
sed -e "s/^  n := 16 /  n := 160 /" $M/cg.kum > $P/exec/cg.kum
sed -e "s/^  n := 64$/  n := 300/" $M/gauss.kum > $P/exec/gauss.kum
sed -e "s/^  n := 16 /  n := 120 /" $M/gmres.kum > $P/exec/gmres.kum
sed -e "s/^  N\([xyz]\) := 8/  N\1 := 20/" $M/heat3d.kum > $P/exec/heat3d.kum
sed -e "s/^  N\([xyz]\) := 8/  N\1 := 14/" $M/heat3d_implicit.kum > $P/exec/heat3d_implicit.kum
sed -e "s/^  N\([xy]\) := 8/  N\1 := 24/" $M/lanczos2d.kum > $P/exec/lanczos2d.kum
sed -e "s/^  N\([xyz]\) := 8/  N\1 := 18/" $M/poisson3d.kum > $P/exec/poisson3d.kum
sed -e "s/^  N\([xyz]\) := 8/  N\1 := 18/" $M/poisson3d_gs.kum > $P/exec/poisson3d_gs.kum
sed -e "s/^  n := 16$/  n := 160/" $M/matmat.kum > $P/exec/matmat.kum
cp $HERE/dec_agg.oz $HERE/heapsort.oz $P/
if [ -n "$QDB_SRC" ]; then
    mkdir -p $ROOT/qdb-oz
    (cd $QDB_SRC && tar cf - $(find kernel modules -name '*.oz')) | (cd $ROOT/qdb-oz && tar xf -)
    python3 $HERE/wrap_kernel.py $ROOT/qdb-oz/kernel/sort/sort_strings.oz > $P/k_sort_strings.oz
    python3 $HERE/wrap_kernel.py $ROOT/qdb-oz/kernel/row_table.oz > $P/k_row_table.oz
    bash $HERE/greedy_qdb.sh
fi
