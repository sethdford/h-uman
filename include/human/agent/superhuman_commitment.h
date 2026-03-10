#ifndef HU_SUPERHUMAN_COMMITMENT_H
#define HU_SUPERHUMAN_COMMITMENT_H

#include "human/agent/commitment_store.h"
#include "human/agent/superhuman.h"
#include "human/core/error.h"
#include <stddef.h>

typedef struct hu_superhuman_commitment_ctx {
    hu_commitment_store_t *store;
    const char *session_id;
    size_t session_id_len;
} hu_superhuman_commitment_ctx_t;

hu_error_t hu_superhuman_commitment_service(hu_superhuman_commitment_ctx_t *ctx,
                                             hu_superhuman_service_t *out);

#endif /* HU_SUPERHUMAN_COMMITMENT_H */
