#ifndef SC_MEMORY_INBOX_H
#define SC_MEMORY_INBOX_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/memory.h"
#include "seaclaw/provider.h"
#include <stdbool.h>
#include <stddef.h>

#define SC_INBOX_DEFAULT_DIR   ".seaclaw/inbox"
#define SC_INBOX_PROCESSED_DIR ".seaclaw/inbox/processed"
#define SC_INBOX_MAX_FILES     100
#define SC_INBOX_MAX_FILE_SIZE (1024 * 1024)

typedef struct sc_inbox_watcher {
    sc_allocator_t *alloc;
    sc_memory_t *memory;
    sc_provider_t *provider; /* optional; enables binary file ingestion via LLM */
    const char *model;       /* model name for provider LLM calls; NULL uses provider default */
    size_t model_len;
    char *inbox_dir;
    size_t inbox_dir_len;
    char *processed_dir;
    size_t processed_dir_len;
    size_t files_ingested;
} sc_inbox_watcher_t;

sc_error_t sc_inbox_init(sc_inbox_watcher_t *watcher, sc_allocator_t *alloc, sc_memory_t *memory,
                         const char *inbox_dir, size_t inbox_dir_len);

sc_error_t sc_inbox_poll(sc_inbox_watcher_t *watcher, size_t *processed_count);

void sc_inbox_deinit(sc_inbox_watcher_t *watcher);

#endif
