#ifndef MINIDB_BENCH_PROFILER_H
#define MINIDB_BENCH_PROFILER_H

#include <stdbool.h>

/*
 * A small sampling profiler for the benchmark binary.
 *
 * A wall-clock timer interrupts the process about 1000 times a second and
 * the signal handler records the call stack. profiler_stop() writes the raw
 * samples -- one line per sample: a weight, then return addresses, innermost
 * first -- which scripts/flamegraph.py turns into a flamegraph.
 *
 * It samples on wall-clock time, not CPU time, so time spent blocked in
 * fsync() shows up in proportion (a sample's weight counts the timer periods
 * that elapsed while a system call held the signal back). `perf record -g` gives the same picture
 * with kernel frames included, where perf is available.
 */

/* Starts sampling. Returns false if the timer could not be installed. */
bool profiler_start(void);

/* Stops sampling and writes the samples to `output_path`. */
bool profiler_stop(const char *output_path);

#endif
