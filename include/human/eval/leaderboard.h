#ifndef HU_EVAL_LEADERBOARD_H
#define HU_EVAL_LEADERBOARD_H

/* Phase 5 Task 4 — cached gold-judge leaderboard runners. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum hu_leaderboard_kind {
    HU_LEADERBOARD_MT_BENCH = 0,
    HU_LEADERBOARD_ALPACA_EVAL = 1,
    HU_LEADERBOARD_IFEVAL = 2,
} hu_leaderboard_kind_t;

typedef struct hu_leaderboard_config {
    const char *canned_path;
    uint32_t seed;
} hu_leaderboard_config_t;

struct hu_leaderboard_runner;

typedef struct hu_leaderboard_runner_vtable {
    hu_error_t (*run)(struct hu_leaderboard_runner *self, hu_allocator_t *alloc,
                      const char *const *prompts, const char *const *responses, size_t n,
                      double *out_scores);
    const char *(*name)(struct hu_leaderboard_runner *self);
    void (*deinit)(struct hu_leaderboard_runner *self, hu_allocator_t *alloc);
} hu_leaderboard_runner_vtable_t;

typedef struct hu_leaderboard_runner {
    const hu_leaderboard_runner_vtable_t *vtable;
    void *ctx;
} hu_leaderboard_runner_t;

hu_error_t hu_leaderboard_create_mt_bench(hu_allocator_t *alloc,
                                          const hu_leaderboard_config_t *cfg,
                                          hu_leaderboard_runner_t *out);
hu_error_t hu_leaderboard_create_alpaca_eval(hu_allocator_t *alloc,
                                             const hu_leaderboard_config_t *cfg,
                                             hu_leaderboard_runner_t *out);
hu_error_t hu_leaderboard_create_ifeval(hu_allocator_t *alloc,
                                        const hu_leaderboard_config_t *cfg,
                                        hu_leaderboard_runner_t *out);

#ifdef __cplusplus
}
#endif

#endif /* HU_EVAL_LEADERBOARD_H */
