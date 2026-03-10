#ifndef HU_SUPERHUMAN_SILENCE_H
#define HU_SUPERHUMAN_SILENCE_H

#include "human/agent/superhuman.h"
#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

#define HU_SILENCE_GAP_MS 5000

typedef struct hu_superhuman_silence_ctx {
    uint64_t last_ts_ms;
    uint64_t prev_ts_ms;
    bool has_prev;
} hu_superhuman_silence_ctx_t;

hu_error_t hu_superhuman_silence_service(hu_superhuman_silence_ctx_t *ctx,
                                          hu_superhuman_service_t *out);

#endif /* HU_SUPERHUMAN_SILENCE_H */
