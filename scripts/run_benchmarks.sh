#!/usr/bin/env bash
# Runs every benchmark behind the figures in docs/ and stores the raw output
# in docs/benchmarks/. Plot it with scripts/plot_benchmarks.py.
#
#   scripts/run_benchmarks.sh [scratch database path]
#
# Takes about five minutes. Put the scratch database on the disk you want to
# measure: fsync cost is the whole story for the "full" sync mode.
set -euo pipefail

cd "$(dirname "$0")/.."
bench=build/bench/minidb_bench
database=${1:-/tmp/minidb_bench.db}
results=docs/benchmarks
mkdir -p "$results"

cmake -S . -B build > /dev/null
cmake --build build --target minidb_bench -j > /dev/null

echo "sync modes and key order..."
{
    "$bench" insert --db "$database" --sync full --rows 30000
    for run in 1 2 3; do
        "$bench" insert --db "$database" --sync group --rows 2000000
        "$bench" insert --db "$database" --sync off --rows 2000000
        "$bench" insert --db "$database" --sync group --rows 500000 --random
    done
    "$bench" insert --db "$database" --sync group --rows 2000000 --random
} > "$results/insert_modes.txt"

echo "group-commit delay sweep..."
{
    "$bench" insert --db "$database" --sync group --delay-ms 0 --rows 30000
    for delay in 1 2 5 10 20 50; do
        for run in 1 2 3; do
            "$bench" insert --db "$database" --sync group --delay-ms "$delay" --rows 1000000
        done
    done
} > "$results/insert_delay.txt"

echo "sustained insert timeline..."
"$bench" insert --db "$database" --sync group --rows 10000000 \
    --timeline "$results/insert_timeline.csv" > "$results/insert_sustained.txt"

echo "point lookups by table size..."
for rows in 1000 10000 100000 1000000 4000000; do
    "$bench" lookup --db "$database" --rows "$rows" --lookups 200000 --cache-pages 131072
done > "$results/lookup.txt"

echo "joins by table size..."
{
    for rows in 250 500 1000 2000 4000 8000; do
        "$bench" join --db "$database" --rows "$rows"
    done
    for rows in 16000 64000 256000; do
        "$bench" join --db "$database" --rows "$rows" --skip-nested-loop
    done
} > "$results/join.txt"

echo "done: results in $results/"
