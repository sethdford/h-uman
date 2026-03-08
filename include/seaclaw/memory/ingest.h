#ifndef SC_MEMORY_INGEST_H
#define SC_MEMORY_INGEST_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/memory.h"
#include "seaclaw/provider.h"
#include <stddef.h>

typedef enum sc_ingest_file_type {
    SC_INGEST_TEXT,
    SC_INGEST_IMAGE,
    SC_INGEST_AUDIO,
    SC_INGEST_VIDEO,
    SC_INGEST_PDF,
    SC_INGEST_UNKNOWN
} sc_ingest_file_type_t;

typedef struct sc_ingest_result {
    char *content;
    size_t content_len;
    char *summary;
    size_t summary_len;
    char *source_path;
    size_t source_path_len;
} sc_ingest_result_t;

sc_ingest_file_type_t sc_ingest_detect_type(const char *path, size_t path_len);

sc_error_t sc_ingest_read_text(sc_allocator_t *alloc, const char *path, size_t path_len, char **out,
                               size_t *out_len);

sc_error_t sc_ingest_file(sc_allocator_t *alloc, sc_memory_t *memory, const char *path,
                          size_t path_len);

sc_error_t sc_ingest_build_extract_prompt(sc_allocator_t *alloc, const char *filename,
                                          size_t filename_len, sc_ingest_file_type_t type,
                                          char **out, size_t *out_len);

sc_error_t sc_ingest_file_with_provider(sc_allocator_t *alloc, sc_memory_t *memory,
                                        sc_provider_t *provider, const char *path, size_t path_len,
                                        const char *model, size_t model_len);

void sc_ingest_result_deinit(sc_ingest_result_t *result, sc_allocator_t *alloc);

#endif
