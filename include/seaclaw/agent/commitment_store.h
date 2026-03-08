#ifndef SC_AGENT_COMMITMENT_STORE_H
#define SC_AGENT_COMMITMENT_STORE_H

#include "seaclaw/agent/commitment.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/memory.h"
#include <stddef.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Commitment store — persist and query commitments via memory backend
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct sc_commitment_store sc_commitment_store_t;

sc_error_t sc_commitment_store_create(sc_allocator_t *alloc, sc_memory_t *memory,
                                      sc_commitment_store_t **out);

sc_error_t sc_commitment_store_save(sc_commitment_store_t *store,
                                    const sc_commitment_t *commitment,
                                    const char *session_id, size_t session_id_len);

sc_error_t sc_commitment_store_list_active(sc_commitment_store_t *store, sc_allocator_t *alloc,
                                           const char *session_id, size_t session_id_len,
                                           sc_commitment_t **out, size_t *out_count);

sc_error_t sc_commitment_store_build_context(sc_commitment_store_t *store, sc_allocator_t *alloc,
                                              const char *session_id, size_t session_id_len,
                                              char **out, size_t *out_len);

void sc_commitment_store_destroy(sc_commitment_store_t *store);

#endif /* SC_AGENT_COMMITMENT_STORE_H */
