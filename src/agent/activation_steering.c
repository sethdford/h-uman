#include "human/agent/activation_steering.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

hu_error_t hu_steering_set_init(hu_steering_set_t *set, hu_allocator_t alloc)
{
    if (!set)
        return HU_ERR_INVALID_ARGUMENT;

    set->alloc = alloc;
    set->count = 0;
    set->capacity = HU_STEERING_SET_INITIAL_CAP;
    size_t sz = set->capacity * sizeof(hu_steering_directive_t);
    set->directives = (hu_steering_directive_t *)alloc.alloc(alloc.ctx, sz);
    if (!set->directives) {
        set->capacity = 0;
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(set->directives, 0, sz);
    return HU_OK;
}

static hu_error_t steering_set_grow(hu_steering_set_t *set)
{
    size_t new_cap = set->capacity * 2;
    size_t old_sz = set->capacity * sizeof(hu_steering_directive_t);
    size_t new_sz = new_cap * sizeof(hu_steering_directive_t);
    hu_steering_directive_t *new_buf =
        (hu_steering_directive_t *)set->alloc.realloc(
            set->alloc.ctx, set->directives, old_sz, new_sz);
    if (!new_buf)
        return HU_ERR_OUT_OF_MEMORY;
    memset(new_buf + set->capacity, 0,
           (new_cap - set->capacity) * sizeof(hu_steering_directive_t));
    set->directives = new_buf;
    set->capacity = new_cap;
    return HU_OK;
}

hu_error_t hu_steering_set_add(hu_steering_set_t *set,
                               const char *direction, float weight)
{
    if (!set || !direction || !direction[0])
        return HU_ERR_INVALID_ARGUMENT;
    if (weight < -1.0f || weight > 1.0f)
        return HU_ERR_INVALID_ARGUMENT;

    size_t dir_len = strlen(direction);
    if (dir_len >= HU_STEERING_DIRECTION_MAX_LEN)
        return HU_ERR_INVALID_ARGUMENT;

    for (size_t i = 0; i < set->count; i++) {
        if (strcmp(set->directives[i].direction, direction) == 0) {
            set->directives[i].weight = weight;
            set->directives[i].active = true;
            return HU_OK;
        }
    }

    if (set->count >= set->capacity) {
        hu_error_t err = steering_set_grow(set);
        if (err != HU_OK)
            return err;
    }

    hu_steering_directive_t *d = &set->directives[set->count];
    memcpy(d->direction, direction, dir_len + 1);
    d->weight = weight;
    d->active = true;
    set->count++;
    return HU_OK;
}

hu_error_t hu_steering_set_remove(hu_steering_set_t *set,
                                  const char *direction)
{
    if (!set || !direction)
        return HU_ERR_INVALID_ARGUMENT;

    for (size_t i = 0; i < set->count; i++) {
        if (strcmp(set->directives[i].direction, direction) == 0) {
            if (i < set->count - 1)
                set->directives[i] = set->directives[set->count - 1];
            set->count--;
            return HU_OK;
        }
    }
    return HU_ERR_NOT_FOUND;
}

hu_error_t hu_steering_set_render(const hu_steering_set_t *set,
                                  hu_allocator_t *alloc,
                                  char **out, size_t *out_len)
{
    if (!set || !alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    *out = NULL;
    *out_len = 0;

    size_t active_count = 0;
    for (size_t i = 0; i < set->count; i++) {
        if (set->directives[i].active)
            active_count++;
    }

    if (active_count == 0)
        return HU_OK;

    /* First pass: compute total buffer size needed. Each directive line:
     * "When responding, lean toward: <dir> (strength: X.XX)\n"  (max ~320 chars)
     * or
     * "When responding, avoid: <dir> (strength: X.XX)\n"  (max ~320 chars)   */
    size_t buf_cap = active_count * (HU_STEERING_DIRECTION_MAX_LEN + 80);
    char *buf = (char *)alloc->alloc(alloc->ctx, buf_cap);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    size_t offset = 0;
    for (size_t i = 0; i < set->count; i++) {
        const hu_steering_directive_t *d = &set->directives[i];
        if (!d->active)
            continue;

        int written;
        if (d->weight >= 0.0f) {
            written = snprintf(buf + offset, buf_cap - offset,
                               "When responding, lean toward: %s (strength: %.2f)\n",
                               d->direction, d->weight);
        } else {
            written = snprintf(buf + offset, buf_cap - offset,
                               "When responding, avoid: %s (strength: %.2f)\n",
                               d->direction, fabsf(d->weight));
        }
        if (written > 0)
            offset += (size_t)written;
    }

    if (offset > 0 && buf[offset - 1] == '\n') {
        offset--;
        buf[offset] = '\0';
    }

    *out = buf;
    *out_len = offset;
    return HU_OK;
}

void hu_steering_set_deinit(hu_steering_set_t *set)
{
    if (!set)
        return;
    if (set->directives && set->capacity > 0) {
        set->alloc.free(set->alloc.ctx, set->directives,
                        set->capacity * sizeof(hu_steering_directive_t));
    }
    set->directives = NULL;
    set->count = 0;
    set->capacity = 0;
}
