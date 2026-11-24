#!/usr/bin/env bash
# Runs every test binary under Valgrind's memcheck and fails on any error or
# leak. The crash tests kill their child processes on purpose, so reports
# from forked children are silenced; the parent is what gets checked.
#
#   scripts/memcheck.sh
set -euo pipefail

cd "$(dirname "$0")/.."
cmake -S . -B build > /dev/null
cmake --build build -j > /dev/null

for test in build/tests/test_*; do
    printf '%-28s' "$(basename "$test")"
    valgrind --quiet --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=all \
        --error-exitcode=1 --child-silent-after-fork=yes "$test" > /dev/null
    echo "clean"
done
