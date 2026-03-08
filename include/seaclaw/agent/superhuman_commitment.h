#ifndef SC_SUPERHUMAN_COMMITMENT_H
#define SC_SUPERHUMAN_COMMITMENT_H

#include "seaclaw/agent/commitment_store.h"
#include "seaclaw/agent/superhuman.h"
#include "seaclaw/core/error.h"
#include <stddef.h>

typedef struct sc_superhuman_commitment_ctx {
    sc_commitment_store_t *store;
    const char *session_id;
    size_t session_id_len;
} sc_superhuman_commitment_ctx_t;

sc_error_t sc_superhuman_commitment_service(sc_superhuman_commitment_ctx_t *ctx,
                                             sc_superhuman_service_t *out);

#endif /* SC_SUPERHUMAN_COMMITMENT_H */
