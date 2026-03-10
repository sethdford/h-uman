#ifndef HU_RAG_H
#define HU_RAG_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct hu_datasheet_chunk {
    const char *board;   /* NULL for generic */
    const char *source;  /* file path */
    const char *content; /* chunk text */
} hu_datasheet_chunk_t;

typedef struct hu_rag {
    hu_allocator_t *alloc;
    hu_datasheet_chunk_t *chunks;
    size_t chunk_count;
    size_t chunk_cap;
} hu_rag_t;

hu_error_t hu_rag_init(hu_rag_t *rag, hu_allocator_t *alloc);
void hu_rag_free(hu_rag_t *rag);

hu_error_t hu_rag_add_chunk(hu_rag_t *rag, const char *board, const char *source,
                            const char *content);

hu_error_t hu_rag_retrieve(hu_rag_t *rag, hu_allocator_t *alloc, const char *query,
                           size_t query_len, const char *const *boards, size_t board_count,
                           size_t limit, const hu_datasheet_chunk_t ***out_results,
                           size_t *out_count);

/* Legacy API (kept for compatibility) */
hu_error_t hu_rag_query(hu_allocator_t *alloc, const char *query, size_t query_len,
                        char **out_response, size_t *out_len);

#endif /* HU_RAG_H */
