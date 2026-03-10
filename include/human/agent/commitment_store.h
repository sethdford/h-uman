#ifndef HU_AGENT_COMMITMENT_STORE_H
#define HU_AGENT_COMMITMENT_STORE_H

#include "human/agent/commitment.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include <stddef.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Commitment store — persist and query commitments via memory backend
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_commitment_store hu_commitment_store_t;

hu_error_t hu_commitment_store_create(hu_allocator_t *alloc, hu_memory_t *memory,
                                      hu_commitment_store_t **out);

hu_error_t hu_commitment_store_save(hu_commitment_store_t *store,
                                    const hu_commitment_t *commitment,
                                    const char *session_id, size_t session_id_len);

hu_error_t hu_commitment_store_list_active(hu_commitment_store_t *store, hu_allocator_t *alloc,
                                           const char *session_id, size_t session_id_len,
                                           hu_commitment_t **out, size_t *out_count);

hu_error_t hu_commitment_store_build_context(hu_commitment_store_t *store, hu_allocator_t *alloc,
                                              const char *session_id, size_t session_id_len,
                                              char **out, size_t *out_len);

void hu_commitment_store_destroy(hu_commitment_store_t *store);

#endif /* HU_AGENT_COMMITMENT_STORE_H */
