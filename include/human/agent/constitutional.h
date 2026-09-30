#ifndef HU_AGENT_CONSTITUTIONAL_H
#define HU_AGENT_CONSTITUTIONAL_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include <stdbool.h>
#include <stddef.h>

/*
 * Constitutional AI — self-critique against configurable principles.
 * Before sending a response, evaluates it against a set of principles
 * and rewrites if any are violated. Based on Bai et al. (2022).
 */

#define HU_CONST_MAX_PRINCIPLES 16

typedef struct hu_principle {
    const char *name;
    size_t name_len;
    const char *description;
    size_t description_len;
} hu_principle_t;

typedef enum hu_critique_verdict {
    HU_CRITIQUE_PASS = 0,
    HU_CRITIQUE_MINOR,   /* style issue, no rewrite needed */
    HU_CRITIQUE_REWRITE, /* needs revision */
} hu_critique_verdict_t;

typedef struct hu_critique_result {
    hu_critique_verdict_t verdict;
    char *revised_response; /* NULL if verdict == PASS */
    size_t revised_response_len;
    char *reasoning; /* why it was flagged */
    size_t reasoning_len;
    int principle_index; /* which principle triggered, -1 if none */
} hu_critique_result_t;

typedef struct hu_constitutional_config {
    hu_principle_t principles[HU_CONST_MAX_PRINCIPLES];
    size_t principle_count;
    bool enabled;
    bool rewrite_enabled; /* if false, only flags but doesn't rewrite */
} hu_constitutional_config_t;

/* Critique a response against principles. Uses LLM for evaluation.
 * Caller must free result with hu_critique_result_free. */
hu_error_t hu_constitutional_critique(hu_allocator_t *alloc, hu_provider_t *provider,
                                      const char *model, size_t model_len, const char *user_msg,
                                      size_t user_msg_len, const char *response,
                                      size_t response_len, const hu_constitutional_config_t *config,
                                      hu_critique_result_t *result);

void hu_critique_result_free(hu_allocator_t *alloc, hu_critique_result_t *result);

/* Default principles: helpful, harmless, honest */
hu_constitutional_config_t hu_constitutional_config_default(void);

#ifdef HU_IS_TEST
hu_critique_verdict_t hu_constitutional_test_parse_verdict(const char *resp, size_t resp_len,
                                                           int *principle_idx);
int hu_constitutional_test_parse_principle_index(const char *resp, size_t resp_len);
#endif

hu_constitutional_config_t hu_constitutional_config_persona(void);

/* Whether a reply turn is critiqued at all: not when it speaks in the
 * persona's voice (the rewrite has no conversation to work from and made
 * every observed reply worse, 2026-09-29); yes for assistant turns. */
bool hu_constitutional_runs_for_turn(bool persona_voice);

#endif
