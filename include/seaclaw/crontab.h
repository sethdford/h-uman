#ifndef SC_CRONTAB_H
#define SC_CRONTAB_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct sc_crontab_entry {
    char *id;
    char *schedule;
    char *command;
    bool enabled;
} sc_crontab_entry_t;

/* Get path to crontab file. Caller frees with alloc->free(ctx, path, len+1). */
sc_error_t sc_crontab_get_path(sc_allocator_t *alloc, char **path, size_t *path_len);

/* Load entries from crontab file. Caller must free entries via sc_crontab_entries_free. */
sc_error_t sc_crontab_load(sc_allocator_t *alloc, const char *path,
    sc_crontab_entry_t **entries, size_t *count);

/* Save entries to crontab file. */
sc_error_t sc_crontab_save(sc_allocator_t *alloc, const char *path,
    const sc_crontab_entry_t *entries, size_t count);

/* Free entries array and their fields. */
void sc_crontab_entries_free(sc_allocator_t *alloc,
    sc_crontab_entry_t *entries, size_t count);

/* Add a new entry, returns new id (allocated, caller frees). */
sc_error_t sc_crontab_add(sc_allocator_t *alloc, const char *path,
    const char *schedule, size_t schedule_len,
    const char *command, size_t command_len,
    char **new_id);

/* Remove entry by id. */
sc_error_t sc_crontab_remove(sc_allocator_t *alloc, const char *path, const char *id);

#endif /* SC_CRONTAB_H */
