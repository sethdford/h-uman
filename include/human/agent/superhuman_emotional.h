#ifndef HU_SUPERHUMAN_EMOTIONAL_H
#define HU_SUPERHUMAN_EMOTIONAL_H

#include "human/agent/superhuman.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>

#define HU_EMOTIONAL_LAST_TEXT_CAP 4096

typedef struct hu_superhuman_emotional_ctx {
    char last_text[HU_EMOTIONAL_LAST_TEXT_CAP];
    size_t last_text_len;
    char last_role[64];
    size_t last_role_len;
} hu_superhuman_emotional_ctx_t;

hu_error_t hu_superhuman_emotional_service(hu_superhuman_emotional_ctx_t *ctx,
                                           hu_superhuman_service_t *out);

#endif /* HU_SUPERHUMAN_EMOTIONAL_H */
