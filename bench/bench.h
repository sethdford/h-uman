#ifndef BENCH_H
#define BENCH_H

#include <stdint.h>

void bench_start(uint64_t *out_start_ns);
void bench_end(uint64_t start_ns, uint64_t *out_elapsed_ns);
void bench_report(const char *name, uint64_t avg_ns, uint64_t min_ns, uint64_t max_ns,
                  unsigned iterations);

#define BENCH_RUN(name, iterations, body)                                                  \
    do {                                                                                   \
        uint64_t _sum = 0, _min = UINT64_MAX, _max = 0;                                    \
        for (unsigned _i = 0; _i < (iterations); _i++) {                                   \
            uint64_t _start, _elapsed;                                                      \
            bench_start(&_start);                                                           \
            do {                                                                            \
                body;                                                                       \
            } while (0);                                                                    \
            bench_end(_start, &_elapsed);                                                   \
            _sum += _elapsed;                                                               \
            if (_elapsed < _min)                                                            \
                _min = _elapsed;                                                            \
            if (_elapsed > _max)                                                            \
                _max = _elapsed;                                                            \
        }                                                                                   \
        bench_report((name), _sum / (iterations), _min, _max, (iterations));                \
    } while (0)

#endif /* BENCH_H */
