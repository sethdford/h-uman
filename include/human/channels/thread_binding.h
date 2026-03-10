#ifndef HU_THREAD_BINDING_H
#define HU_THREAD_BINDING_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum hu_thread_lifecycle {
    HU_THREAD_ACTIVE,
    HU_THREAD_IDLE,
    HU_THREAD_CLOSED,
} hu_thread_lifecycle_t;

typedef struct hu_thread_binding_entry {
    char *channel_name;
    char *thread_id;
    uint64_t agent_id;
    hu_thread_lifecycle_t lifecycle;
    int64_t created_at;
    int64_t last_active;
    uint32_t idle_timeout_secs;
} hu_thread_binding_entry_t;

typedef struct hu_thread_binding hu_thread_binding_t;

hu_thread_binding_t *hu_thread_binding_create(hu_allocator_t *alloc, uint32_t max_bindings);
void hu_thread_binding_destroy(hu_thread_binding_t *tb);

hu_error_t hu_thread_binding_bind(hu_thread_binding_t *tb, const char *channel_name,
                                  const char *thread_id, uint64_t agent_id,
                                  uint32_t idle_timeout_secs);

hu_error_t hu_thread_binding_unbind(hu_thread_binding_t *tb, const char *channel_name,
                                    const char *thread_id);

hu_error_t hu_thread_binding_lookup(hu_thread_binding_t *tb, const char *channel_name,
                                    const char *thread_id, uint64_t *out_agent_id);

hu_error_t hu_thread_binding_touch(hu_thread_binding_t *tb, const char *channel_name,
                                   const char *thread_id);

size_t hu_thread_binding_expire_idle(hu_thread_binding_t *tb, int64_t now);

hu_error_t hu_thread_binding_list(hu_thread_binding_t *tb, hu_allocator_t *alloc,
                                  hu_thread_binding_entry_t **out, size_t *out_count);

size_t hu_thread_binding_count(hu_thread_binding_t *tb);

#endif /* HU_THREAD_BINDING_H */
