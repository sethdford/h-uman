#ifndef SC_THREAD_BINDING_H
#define SC_THREAD_BINDING_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum sc_thread_lifecycle {
    SC_THREAD_ACTIVE,
    SC_THREAD_IDLE,
    SC_THREAD_CLOSED,
} sc_thread_lifecycle_t;

typedef struct sc_thread_binding_entry {
    char *channel_name;
    char *thread_id;
    uint64_t agent_id;
    sc_thread_lifecycle_t lifecycle;
    int64_t created_at;
    int64_t last_active;
    uint32_t idle_timeout_secs;
} sc_thread_binding_entry_t;

typedef struct sc_thread_binding sc_thread_binding_t;

sc_thread_binding_t *sc_thread_binding_create(sc_allocator_t *alloc, uint32_t max_bindings);
void sc_thread_binding_destroy(sc_thread_binding_t *tb);

sc_error_t sc_thread_binding_bind(sc_thread_binding_t *tb, const char *channel_name,
                                  const char *thread_id, uint64_t agent_id,
                                  uint32_t idle_timeout_secs);

sc_error_t sc_thread_binding_unbind(sc_thread_binding_t *tb, const char *channel_name,
                                    const char *thread_id);

sc_error_t sc_thread_binding_lookup(sc_thread_binding_t *tb, const char *channel_name,
                                    const char *thread_id, uint64_t *out_agent_id);

sc_error_t sc_thread_binding_touch(sc_thread_binding_t *tb, const char *channel_name,
                                   const char *thread_id);

size_t sc_thread_binding_expire_idle(sc_thread_binding_t *tb, int64_t now);

sc_error_t sc_thread_binding_list(sc_thread_binding_t *tb, sc_allocator_t *alloc,
                                  sc_thread_binding_entry_t **out, size_t *out_count);

size_t sc_thread_binding_count(sc_thread_binding_t *tb);

#endif /* SC_THREAD_BINDING_H */
