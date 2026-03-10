#ifndef HU_MEMORY_INGEST_H
#define HU_MEMORY_INGEST_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include "human/provider.h"
#include <stddef.h>

typedef enum hu_ingest_file_type {
    HU_INGEST_TEXT,
    HU_INGEST_IMAGE,
    HU_INGEST_AUDIO,
    HU_INGEST_VIDEO,
    HU_INGEST_PDF,
    HU_INGEST_UNKNOWN
} hu_ingest_file_type_t;

typedef struct hu_ingest_result {
    char *content;
    size_t content_len;
    char *summary;
    size_t summary_len;
    char *source_path;
    size_t source_path_len;
} hu_ingest_result_t;

hu_ingest_file_type_t hu_ingest_detect_type(const char *path, size_t path_len);

hu_error_t hu_ingest_read_text(hu_allocator_t *alloc, const char *path, size_t path_len, char **out,
                               size_t *out_len);

hu_error_t hu_ingest_file(hu_allocator_t *alloc, hu_memory_t *memory, const char *path,
                          size_t path_len);

hu_error_t hu_ingest_build_extract_prompt(hu_allocator_t *alloc, const char *filename,
                                          size_t filename_len, hu_ingest_file_type_t type,
                                          char **out, size_t *out_len);

hu_error_t hu_ingest_file_with_provider(hu_allocator_t *alloc, hu_memory_t *memory,
                                        hu_provider_t *provider, const char *path, size_t path_len,
                                        const char *model, size_t model_len);

void hu_ingest_result_deinit(hu_ingest_result_t *result, hu_allocator_t *alloc);

#endif
