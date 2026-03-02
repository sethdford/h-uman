#ifndef SC_RAG_H
#define SC_RAG_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stddef.h>
#include <stdbool.h>

typedef struct sc_datasheet_chunk {
    const char *board;    /* NULL for generic */
    const char *source;   /* file path */
    const char *content;  /* chunk text */
} sc_datasheet_chunk_t;

typedef struct sc_rag {
    sc_allocator_t *alloc;
    sc_datasheet_chunk_t *chunks;
    size_t chunk_count;
    size_t chunk_cap;
} sc_rag_t;

sc_error_t sc_rag_init(sc_rag_t *rag, sc_allocator_t *alloc);
void sc_rag_free(sc_rag_t *rag);

sc_error_t sc_rag_add_chunk(sc_rag_t *rag,
    const char *board, const char *source, const char *content);

sc_error_t sc_rag_retrieve(sc_rag_t *rag, sc_allocator_t *alloc,
    const char *query, size_t query_len,
    const char *const *boards, size_t board_count,
    size_t limit,
    const sc_datasheet_chunk_t ***out_results, size_t *out_count);

/* Legacy API (kept for compatibility) */
sc_error_t sc_rag_query(sc_allocator_t *alloc, const char *query, size_t query_len,
    char **out_response, size_t *out_len);

#endif /* SC_RAG_H */
