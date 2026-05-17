#ifndef HU_AGENT_ACTIVATION_STEERING_H
#define HU_AGENT_ACTIVATION_STEERING_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>

#define HU_STEERING_DIRECTION_MAX_LEN 256
#define HU_STEERING_SET_INITIAL_CAP   8

typedef struct hu_steering_directive {
    char direction[HU_STEERING_DIRECTION_MAX_LEN];
    float weight; /* -1.0 (avoid) to +1.0 (lean toward) */
    bool active;
} hu_steering_directive_t;

typedef struct hu_steering_set {
    hu_steering_directive_t *directives;
    size_t count;
    size_t capacity;
    hu_allocator_t alloc;
} hu_steering_set_t;

hu_error_t hu_steering_set_init(hu_steering_set_t *set, hu_allocator_t alloc);

hu_error_t hu_steering_set_add(hu_steering_set_t *set,
                               const char *direction, float weight);

hu_error_t hu_steering_set_remove(hu_steering_set_t *set,
                                  const char *direction);

/* Render active directives into a system prompt fragment.
 * Positive weights produce: "When responding, lean toward: <dir> (strength: <w>)"
 * Negative weights produce: "When responding, avoid: <dir> (strength: <|w|>)"
 * Caller owns *out; free via alloc->free(alloc->ctx, *out, *out_len + 1).
 * If no active directives, *out is NULL and *out_len is 0. */
hu_error_t hu_steering_set_render(const hu_steering_set_t *set,
                                  hu_allocator_t *alloc,
                                  char **out, size_t *out_len);

void hu_steering_set_deinit(hu_steering_set_t *set);

#endif /* HU_AGENT_ACTIVATION_STEERING_H */
