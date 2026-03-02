#include "seaclaw/memory/retrieval.h"
#include "seaclaw/memory.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"

#define SC_RRF_K 60
/* RRF: score = sum(1/(k+rank)) - used when combining keyword + semantic */

/* Hybrid retrieval: keyword + optional semantic, combined with RRF when both available */
sc_error_t sc_hybrid_retrieve(sc_allocator_t *alloc, sc_memory_t *backend,
    const char *query, size_t query_len,
    const sc_retrieval_options_t *opts,
    sc_retrieval_result_t *out) {
    /* For now only keyword is available - use it */
    return sc_keyword_retrieve(alloc, backend, query, query_len, opts, out);
}
