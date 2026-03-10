#ifndef HU_UNDO_H
#define HU_UNDO_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum hu_undo_type {
    HU_UNDO_FILE_WRITE,
    HU_UNDO_FILE_CREATE,
    HU_UNDO_SHELL_COMMAND,
} hu_undo_type_t;

typedef struct hu_undo_entry {
    hu_undo_type_t type;
    char *description;
    char *path;
    char *original_content;
    size_t original_content_len;
    int64_t timestamp;
    bool reversible;
} hu_undo_entry_t;

typedef struct hu_undo_stack hu_undo_stack_t;

hu_undo_stack_t *hu_undo_stack_create(hu_allocator_t *alloc, size_t max_entries);
void hu_undo_stack_destroy(hu_undo_stack_t *stack);
hu_error_t hu_undo_stack_push(hu_undo_stack_t *stack, const hu_undo_entry_t *entry);
size_t hu_undo_stack_count(const hu_undo_stack_t *stack);
hu_error_t hu_undo_stack_execute_undo(hu_undo_stack_t *stack, hu_allocator_t *alloc);
void hu_undo_entry_free(hu_allocator_t *alloc, hu_undo_entry_t *entry);

#endif
