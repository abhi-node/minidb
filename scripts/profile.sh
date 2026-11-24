#!/usr/bin/env bash
# Profiles one benchmark workload and renders a flamegraph.
#
#   scripts/profile.sh OUTPUT.svg <minidb_bench arguments...>
#   scripts/profile.sh /tmp/insert.svg insert --rows 2000000
#
# Uses the sampler built into minidb_bench, which needs a statically linked
# binary so every sampled address can be named. With perf installed,
# `perf record -g build/bench/minidb_bench ...` shows the same picture plus
# kernel frames.
set -euo pipefail

if [ $# -lt 2 ]; then
    sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
fi
output=$1
shift

cd "$(dirname "$0")/.."
cmake -S . -B build-profile -DMINIDB_STATIC_BENCH=ON > /dev/null
cmake --build build-profile --target minidb_bench -j > /dev/null

samples=$(mktemp)
trap 'rm -f "$samples"' EXIT
build-profile/bench/minidb_bench "$@" --profile "$samples"
scripts/flamegraph.py "$samples" build-profile/bench/minidb_bench "$output" \
    --title "minidb_bench $*" --top 15
echo "wrote $output"
