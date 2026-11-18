#!/bin/bash
# Greedily joins the qdb kernel files that compile together (LLVM -O0) into
# progs/qdb_all.oz, in file name order.
ROOT=${TPDE_BENCH_ROOT:-$HOME/tpde-draft}
HERE=$(cd "$(dirname "$0")" && pwd)
cd $ROOT
keep=""
for f in $(find qdb-oz/kernel -name "*.oz" | sort); do
    python3 $HERE/wrap_kernel.py $keep $f > /tmp/qdb_try.oz
    if ./qumir/build/bin/qumiri --core --compile-only -O0 --module qdb-oz/modules/qumirdb.oz -i /tmp/qdb_try.oz >/dev/null 2>&1; then
        keep="$keep $f"
    fi
done
python3 $HERE/wrap_kernel.py $keep > progs/qdb_all.oz
echo $keep > progs/qdb_all.files
