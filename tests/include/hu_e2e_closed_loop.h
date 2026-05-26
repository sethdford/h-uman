/* tests/include/hu_e2e_closed_loop.h — Phase 6 E2E closed-loop test seam. */
#ifndef HU_E2E_CLOSED_LOOP_H
#define HU_E2E_CLOSED_LOOP_H

#include "human/agent/reaction_handler.h"
#include "human/channels/reaction_event.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/dpo.h"
#include "human/ml/rl_trainer.h"
#include "human/provider.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef HU_E2E_TMP_ROOT
#define HU_E2E_TMP_ROOT "tests/_tmp"
#endif

static inline int64_t hu_e2e_monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

static inline const char *hu_e2e_tmp_root(void) {
    return HU_E2E_TMP_ROOT;
}

static inline void hu_e2e_tmp_path(char *buf, size_t cap, const char *suffix) {
    if (!buf || cap == 0 || !suffix)
        return;
    snprintf(buf, cap, "%s/%s", HU_E2E_TMP_ROOT, suffix);
}

typedef struct hu_e2e_reaction_aux {
    const char *prompt;
    const char *response_chosen;
    const char *response_rejected;
} hu_e2e_reaction_aux_t;

typedef struct hu_e2e_closed_loop_input {
    hu_provider_t *provider;
    hu_rl_trainer_t *trainer;
    hu_dpo_collector_t *collector;
    const hu_reaction_event_t *reaction_events;
    const hu_e2e_reaction_aux_t *reaction_aux;
    size_t reaction_event_count;
    const char *system_prompt;
    size_t system_prompt_len;
    const char *user_message;
    size_t user_message_len;
    const char *model;
    size_t model_len;
    double temperature;
    const char *adapter_out_path;
    const char *adapter_id;
} hu_e2e_closed_loop_input_t;

typedef struct hu_e2e_closed_loop_output {
    char *before_response;
    size_t before_response_len;
    char *after_response;
    size_t after_response_len;
    bool responses_differ;
    double pairs_consumed;
    int64_t elapsed_ms;
    char adapter_path[1024];
} hu_e2e_closed_loop_output_t;

void hu_e2e_reaction_aux_free(hu_allocator_t *alloc, hu_e2e_reaction_aux_t *aux, size_t n);

hu_error_t hu_e2e_closed_loop_run(const hu_e2e_closed_loop_input_t *in, hu_allocator_t *alloc,
                                  hu_e2e_closed_loop_output_t *out);

void hu_e2e_closed_loop_output_free(hu_allocator_t *alloc, hu_e2e_closed_loop_output_t *out);

#ifdef __cplusplus
}
#endif

#endif /* HU_E2E_CLOSED_LOOP_H */
