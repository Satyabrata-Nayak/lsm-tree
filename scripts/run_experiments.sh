#!/usr/bin/env bash
# Controlled experiment sweep for MiniLSM.
#
# Runs every workload across the tuning grid and writes one JSON report per
# combination into benchmarks/results/. Override the measured operation count
# with BENCH_OPS (default 5000) to trade runtime against precision.
set -euo pipefail

cd "$(dirname "$0")/.."

BENCH=build/lsm_bench
RESULTS=benchmarks/results
OPS="${BENCH_OPS:-5000}"
DB=build/bench_sweep.db

WORKLOADS="write_heavy read_heavy mixed"
MEMTABLES="64K 256K 1M 4M"
BLOOMS="4 8 10 14"
COMPACTIONS="2 4 8"

mkdir -p "$RESULTS"

if [[ ! -x "$BENCH" ]]; then
  echo "error: $BENCH not built (run 'make build/lsm_bench')" >&2
  exit 1
fi

cleanup() { rm -rf "$DB"; }
trap cleanup EXIT

run_one() {
  local workload=$1 tag=$2
  shift 2
  ./"$BENCH" "benchmarks/workloads/$workload.conf" "$DB" \
    --ops "$OPS" --output "$RESULTS/${workload}__${tag}.json" "$@"
}

echo "== memtable sweep (bloom=10, compaction=4) =="
for w in $WORKLOADS; do
  for m in $MEMTABLES; do
    run_one "$w" "memtable_$m" --memtable "$m"
  done
done

echo "== bloom sweep (memtable=256K, compaction=4) =="
for w in $WORKLOADS; do
  for b in $BLOOMS; do
    run_one "$w" "bloom_$b" --bloom "$b"
  done
done

echo "== compaction sweep (memtable=256K, bloom=10) =="
for w in $WORKLOADS; do
  for c in $COMPACTIONS; do
    run_one "$w" "compaction_$c" --compaction "$c"
  done
done

echo "done: reports in $RESULTS"
