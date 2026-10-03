#ifndef HU_MODERATION_H
#define HU_MODERATION_H
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/security/self_harm.h"
#include <stdbool.h>
#include <stddef.h>
typedef struct hu_moderation_result {
    bool flagged;
    bool violence;
    bool hate;
    bool sexual;
    bool self_harm;
    double violence_score;
    double hate_score;
    double sexual_score;
    double self_harm_score;
    /* HU_CRISIS_TIERS live: the canonical tier (THIRD_PERSON leaves self_harm
     * false). off/shadow: derived from the legacy flags (flagged -> EXPLICIT,
     * unflagged self_harm -> LOW). */
    hu_self_harm_tier_t self_harm_tier;
} hu_moderation_result_t;
hu_error_t hu_moderation_check(hu_allocator_t *alloc, const char *text, size_t text_len,
                               hu_moderation_result_t *out);
hu_error_t hu_moderation_check_local(hu_allocator_t *alloc, const char *text, size_t text_len,
                                     hu_moderation_result_t *out);
hu_error_t hu_crisis_response_build(hu_allocator_t *alloc, char **out, size_t *out_len);
#endif
