#ifndef SC_MULTIMODAL_DOCUMENT_H
#define SC_MULTIMODAL_DOCUMENT_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stddef.h>

typedef enum sc_doc_type {
    SC_DOC_PLAINTEXT,
    SC_DOC_MARKDOWN,
    SC_DOC_JSON,
    SC_DOC_CSV,
    SC_DOC_CODE,
    SC_DOC_UNKNOWN_TYPE,
} sc_doc_type_t;

typedef struct sc_doc_chunk {
    char *content;
    size_t content_len;
    size_t start_offset;
    size_t end_offset;
} sc_doc_chunk_t;

/* Detect document type from extension */
sc_doc_type_t sc_doc_detect_type(const char *filename, size_t filename_len);

/* Chunk a document into overlapping segments for memory storage */
sc_error_t sc_doc_chunk(sc_allocator_t *alloc, const char *content, size_t content_len,
                        size_t chunk_size, size_t overlap, sc_doc_chunk_t **out, size_t *out_count);

/* Build extraction prompt for a document chunk */
sc_error_t sc_doc_build_extract_prompt(sc_allocator_t *alloc, const char *filename,
                                       size_t filename_len, sc_doc_type_t type, const char *chunk,
                                       size_t chunk_len, char **out, size_t *out_len);

/* Free chunks */
void sc_doc_chunks_free(sc_allocator_t *alloc, sc_doc_chunk_t *chunks, size_t count);

#endif
