#ifndef HU_ACTION_PREVIEW_H
#define HU_ACTION_PREVIEW_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>

typedef struct hu_action_preview {
    const char *tool_name;
    char *description;      /* owned */
    const char *risk_level; /* "low", "medium", "high" */
} hu_action_preview_t;

hu_error_t hu_action_preview_generate(hu_allocator_t *alloc, const char *tool_name,
                                      const char *args_json, size_t args_json_len,
                                      hu_action_preview_t *out);
hu_error_t hu_action_preview_format(hu_allocator_t *alloc, const hu_action_preview_t *p, char **out,
                                    size_t *out_len);
void hu_action_preview_free(hu_allocator_t *alloc, hu_action_preview_t *p);

#endif
