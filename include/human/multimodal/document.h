#ifndef HU_MULTIMODAL_DOCUMENT_H
#define HU_MULTIMODAL_DOCUMENT_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>

typedef enum hu_doc_type {
    HU_DOC_PLAINTEXT,
    HU_DOC_MARKDOWN,
    HU_DOC_JSON,
    HU_DOC_CSV,
    HU_DOC_CODE,
    HU_DOC_UNKNOWN_TYPE,
} hu_doc_type_t;

typedef struct hu_doc_chunk {
    char *content;
    size_t content_len;
    size_t start_offset;
    size_t end_offset;
} hu_doc_chunk_t;

/* Detect document type from extension */
hu_doc_type_t hu_doc_detect_type(const char *filename, size_t filename_len);

/* Chunk a document into overlapping segments for memory storage */
hu_error_t hu_doc_chunk(hu_allocator_t *alloc, const char *content, size_t content_len,
                        size_t chunk_size, size_t overlap, hu_doc_chunk_t **out, size_t *out_count);

/* Build extraction prompt for a document chunk */
hu_error_t hu_doc_build_extract_prompt(hu_allocator_t *alloc, const char *filename,
                                       size_t filename_len, hu_doc_type_t type, const char *chunk,
                                       size_t chunk_len, char **out, size_t *out_len);

/* Free chunks */
void hu_doc_chunks_free(hu_allocator_t *alloc, hu_doc_chunk_t *chunks, size_t count);

#endif
