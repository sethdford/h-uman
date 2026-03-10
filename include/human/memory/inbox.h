#ifndef HU_MEMORY_INBOX_H
#define HU_MEMORY_INBOX_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include "human/provider.h"
#include <stdbool.h>
#include <stddef.h>

#define HU_INBOX_DEFAULT_DIR   ".human/inbox"
#define HU_INBOX_PROCESSED_DIR ".human/inbox/processed"
#define HU_INBOX_MAX_FILES     100
#define HU_INBOX_MAX_FILE_SIZE (1024 * 1024)

typedef struct hu_inbox_watcher {
    hu_allocator_t *alloc;
    hu_memory_t *memory;
    hu_provider_t *provider; /* optional; enables binary file ingestion via LLM */
    const char *model;       /* model name for provider LLM calls; NULL uses provider default */
    size_t model_len;
    char *inbox_dir;
    size_t inbox_dir_len;
    char *processed_dir;
    size_t processed_dir_len;
    size_t files_ingested;
} hu_inbox_watcher_t;

hu_error_t hu_inbox_init(hu_inbox_watcher_t *watcher, hu_allocator_t *alloc, hu_memory_t *memory,
                         const char *inbox_dir, size_t inbox_dir_len);

hu_error_t hu_inbox_poll(hu_inbox_watcher_t *watcher, size_t *processed_count);

void hu_inbox_deinit(hu_inbox_watcher_t *watcher);

#endif
