#include "human/agent/undo.h"
#include "human/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

struct hu_undo_stack {
    hu_allocator_t *alloc;
    hu_undo_entry_t *entries;
    size_t capacity;
    size_t count;
    size_t head; /* next write position (ring buffer) */
};

hu_undo_stack_t *hu_undo_stack_create(hu_allocator_t *alloc, size_t max_entries) {
    if (!alloc || max_entries == 0)
        return NULL;
    hu_undo_stack_t *stack = (hu_undo_stack_t *)alloc->alloc(alloc->ctx, sizeof(hu_undo_stack_t));
    if (!stack)
        return NULL;
    memset(stack, 0, sizeof(*stack));
    stack->alloc = alloc;
    stack->capacity = max_entries;
    stack->entries =
        (hu_undo_entry_t *)alloc->alloc(alloc->ctx, max_entries * sizeof(hu_undo_entry_t));
    if (!stack->entries) {
        alloc->free(alloc->ctx, stack, sizeof(hu_undo_stack_t));
        return NULL;
    }
    memset(stack->entries, 0, max_entries * sizeof(hu_undo_entry_t));
    return stack;
}

static void free_entry(hu_allocator_t *alloc, hu_undo_entry_t *e) {
    if (!e)
        return;
    if (e->description) {
        alloc->free(alloc->ctx, e->description, strlen(e->description) + 1);
        e->description = NULL;
    }
    if (e->path) {
        alloc->free(alloc->ctx, e->path, strlen(e->path) + 1);
        e->path = NULL;
    }
    if (e->original_content) {
        alloc->free(alloc->ctx, e->original_content, e->original_content_len + 1);
        e->original_content = NULL;
    }
}

void hu_undo_stack_destroy(hu_undo_stack_t *stack) {
    if (!stack || !stack->alloc)
        return;
    for (size_t i = 0; i < stack->capacity; i++)
        free_entry(stack->alloc, &stack->entries[i]);
    stack->alloc->free(stack->alloc->ctx, stack->entries,
                       stack->capacity * sizeof(hu_undo_entry_t));
    stack->alloc->free(stack->alloc->ctx, stack, sizeof(hu_undo_stack_t));
}

hu_error_t hu_undo_stack_push(hu_undo_stack_t *stack, const hu_undo_entry_t *entry) {
    if (!stack || !entry)
        return HU_ERR_INVALID_ARGUMENT;
    size_t idx = stack->head % stack->capacity;
    free_entry(stack->alloc, &stack->entries[idx]);

    stack->entries[idx].type = entry->type;
    stack->entries[idx].timestamp = entry->timestamp;
    stack->entries[idx].reversible = entry->reversible;
    stack->entries[idx].original_content_len = entry->original_content_len;

    stack->entries[idx].description =
        entry->description ? hu_strdup(stack->alloc, entry->description) : NULL;
    stack->entries[idx].path = entry->path ? hu_strdup(stack->alloc, entry->path) : NULL;
    stack->entries[idx].original_content = NULL;
    if (entry->original_content && entry->original_content_len > 0) {
        stack->entries[idx].original_content =
            (char *)stack->alloc->alloc(stack->alloc->ctx, entry->original_content_len + 1);
        if (stack->entries[idx].original_content) {
            memcpy(stack->entries[idx].original_content, entry->original_content,
                   entry->original_content_len);
            stack->entries[idx].original_content[entry->original_content_len] = '\0';
        }
    }

    stack->head++;
    if (stack->count < stack->capacity)
        stack->count++;
    return HU_OK;
}

size_t hu_undo_stack_count(const hu_undo_stack_t *stack) {
    return stack ? stack->count : 0;
}

hu_error_t hu_undo_stack_execute_undo(hu_undo_stack_t *stack, hu_allocator_t *alloc) {
    if (!stack || !alloc || stack->count == 0)
        return HU_ERR_INVALID_ARGUMENT;

    size_t idx = (stack->head - 1) % stack->capacity;
    hu_undo_entry_t *e = &stack->entries[idx];

#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)e;
    /* Skip actual I/O in tests; still pop the entry */
#else
    if (e->path) {
        if (e->type == HU_UNDO_FILE_WRITE && e->original_content) {
            FILE *f = fopen(e->path, "wb");
            if (f) {
                size_t n = fwrite(e->original_content, 1, e->original_content_len, f);
                fclose(f);
                if (n != e->original_content_len) {
                    return HU_ERR_IO;
                }
            } else {
                return HU_ERR_IO;
            }
        } else if (e->type == HU_UNDO_FILE_CREATE) {
#if defined(__unix__) || defined(__APPLE__)
            if (unlink(e->path) != 0)
                return HU_ERR_IO;
#endif
        }
    }
#endif

    free_entry(stack->alloc, e);
    memset(e, 0, sizeof(*e));
    stack->head = (stack->head > 0) ? stack->head - 1 : stack->capacity - 1;
    stack->count = (stack->count > 0) ? stack->count - 1 : 0;
    return HU_OK;
}

void hu_undo_entry_free(hu_allocator_t *alloc, hu_undo_entry_t *entry) {
    if (!alloc || !entry)
        return;
    free_entry(alloc, entry);
}
