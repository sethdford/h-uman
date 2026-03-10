#ifndef HU_CRONTAB_H
#define HU_CRONTAB_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct hu_crontab_entry {
    char *id;
    char *schedule;
    char *command;
    bool enabled;
} hu_crontab_entry_t;

/* Get path to crontab file. Caller frees with alloc->free(ctx, path, len+1). */
hu_error_t hu_crontab_get_path(hu_allocator_t *alloc, char **path, size_t *path_len);

/* Load entries from crontab file. Caller must free entries via hu_crontab_entries_free. */
hu_error_t hu_crontab_load(hu_allocator_t *alloc, const char *path, hu_crontab_entry_t **entries,
                           size_t *count);

/* Save entries to crontab file. */
hu_error_t hu_crontab_save(hu_allocator_t *alloc, const char *path,
                           const hu_crontab_entry_t *entries, size_t count);

/* Free entries array and their fields. */
void hu_crontab_entries_free(hu_allocator_t *alloc, hu_crontab_entry_t *entries, size_t count);

/* Add a new entry, returns new id (allocated, caller frees). */
hu_error_t hu_crontab_add(hu_allocator_t *alloc, const char *path, const char *schedule,
                          size_t schedule_len, const char *command, size_t command_len,
                          char **new_id);

/* Remove entry by id. */
hu_error_t hu_crontab_remove(hu_allocator_t *alloc, const char *path, const char *id);

#endif /* HU_CRONTAB_H */
