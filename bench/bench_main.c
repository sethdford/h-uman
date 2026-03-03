/* Simple benchmark harness for seaclaw core operations.
 * Uses clock_gettime(CLOCK_MONOTONIC) for timing.
 * Compile with SC_ENABLE_BENCH=ON.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__) || defined(__MACH__)
#include <mach/mach_time.h>
static int bench_clock_monotonic = 0;
static mach_timebase_info_data_t bench_timebase;
#else
#include <time.h>
static int bench_clock_monotonic = 1;
#endif

static uint64_t bench_ticks_now(void) {
#if defined(__APPLE__) || defined(__MACH__)
    if (!bench_clock_monotonic) {
        mach_timebase_info(&bench_timebase);
        bench_clock_monotonic = 1;
    }
    return mach_absolute_time();
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

static uint64_t bench_ticks_to_ns(uint64_t ticks) {
#if defined(__APPLE__) || defined(__MACH__)
    return ticks * bench_timebase.numer / bench_timebase.denom;
#else
    (void)ticks;
    return 0; /* already ns on Linux */
#endif
}

void bench_start(uint64_t *out_start_ns) {
#if defined(__APPLE__) || defined(__MACH__)
    *out_start_ns = bench_ticks_to_ns(bench_ticks_now());
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    *out_start_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

void bench_end(uint64_t start_ns, uint64_t *out_elapsed_ns) {
#if defined(__APPLE__) || defined(__MACH__)
    uint64_t end = bench_ticks_to_ns(bench_ticks_now());
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t end = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
    *out_elapsed_ns = end - start_ns;
}

void bench_report(const char *name, uint64_t avg_ns, uint64_t min_ns, uint64_t max_ns,
                  unsigned iterations) {
    printf("BENCH %s: avg=%lluns min=%lluns max=%lluns (N=%u)\n", name,
           (unsigned long long)avg_ns, (unsigned long long)min_ns, (unsigned long long)max_ns,
           iterations);
}
