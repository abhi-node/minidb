#include "profiler.h"

#include <execinfo.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#define SAMPLE_INTERVAL_MICROSECONDS 1000
#define MAX_STACK_DEPTH 48
#define MAX_SAMPLES 200000  /* 200 seconds at 1 kHz */

typedef struct {
    int depth;
    int weight;  /* timer periods this sample stands for */
    void *frames[MAX_STACK_DEPTH];
} sample_t;

/* Preallocated so the signal handler never allocates. */
static sample_t *samples = NULL;
static volatile sig_atomic_t sample_count = 0;
static int64_t previous_sample_microseconds = 0;

static int64_t monotonic_microseconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

/*
 * A signal that arrives during an uninterruptible system call (an fsync,
 * say) is only delivered when the call returns, and the ticks that piled up
 * meanwhile collapse into one. The stack at that moment is still inside the
 * call, so the sample is given the weight of every period that went by.
 */
static void record_sample(int signal_number) {
    (void)signal_number;
    int64_t now = monotonic_microseconds();
    int64_t elapsed = now - previous_sample_microseconds;
    previous_sample_microseconds = now;

    if (sample_count >= MAX_SAMPLES) {
        return;
    }
    sample_t *sample = &samples[sample_count];
    int64_t periods =
        (elapsed + SAMPLE_INTERVAL_MICROSECONDS / 2) / SAMPLE_INTERVAL_MICROSECONDS;
    sample->weight = periods < 1 ? 1 : (int)periods;
    sample->depth = backtrace(sample->frames, MAX_STACK_DEPTH);
    sample_count++;
}

static bool set_timer(long interval_microseconds) {
    struct itimerval timer;
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_usec = interval_microseconds;
    timer.it_value = timer.it_interval;
    return setitimer(ITIMER_REAL, &timer, NULL) == 0;
}

bool profiler_start(void) {
    samples = malloc(MAX_SAMPLES * sizeof(*samples));
    if (samples == NULL) {
        return false;
    }
    sample_count = 0;
    previous_sample_microseconds = monotonic_microseconds();

    /* The first backtrace() call may load unwinder state; do it outside the
     * signal handler. */
    void *warm_up[2];
    backtrace(warm_up, 2);

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = record_sample;
    action.sa_flags = SA_RESTART;  /* interrupted system calls resume on their own */
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGALRM, &action, NULL) != 0) {
        return false;
    }
    return set_timer(SAMPLE_INTERVAL_MICROSECONDS);
}

bool profiler_stop(const char *output_path) {
    set_timer(0);
    signal(SIGALRM, SIG_IGN);

    FILE *output = fopen(output_path, "w");
    if (output == NULL) {
        return false;
    }
    for (int index = 0; index < sample_count; index++) {
        const sample_t *sample = &samples[index];
        fprintf(output, "%d", sample->weight);
        for (int frame = 0; frame < sample->depth; frame++) {
            fprintf(output, " %p", sample->frames[frame]);
        }
        fputc('\n', output);
    }
    fclose(output);
    free(samples);
    samples = NULL;
    return true;
}
