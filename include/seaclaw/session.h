#ifndef SC_SESSION_H
#define SC_SESSION_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stddef.h>
#include <stdint.h>

#define SC_SESSION_MAP_CAP 256
#define SC_SESSION_KEY_LEN 128
#define SC_SESSION_LABEL_LEN 128
#define SC_SESSION_MSG_CAP 4096
#define SC_SESSION_MSG_ROLE_LEN 32
#define SC_SESSION_MSG_CONTENT_CAP 65536

typedef struct sc_session_message {
    char role[SC_SESSION_MSG_ROLE_LEN];
    char *content;
} sc_session_message_t;

typedef struct sc_session {
    char session_key[SC_SESSION_KEY_LEN];
    char label[SC_SESSION_LABEL_LEN];
    int64_t created_at;
    int64_t last_active;
    uint64_t turn_count;
    sc_session_message_t *messages;
    size_t message_count;
    size_t message_cap;
} sc_session_t;

typedef struct sc_session_summary {
    char session_key[SC_SESSION_KEY_LEN];
    char label[SC_SESSION_LABEL_LEN];
    int64_t created_at;
    int64_t last_active;
    uint64_t turn_count;
} sc_session_summary_t;

typedef struct sc_session_entry {
    char key[SC_SESSION_KEY_LEN];
    sc_session_t *session;
    struct sc_session_entry *next;
} sc_session_entry_t;

typedef struct sc_session_manager {
    sc_allocator_t *alloc;
    sc_session_entry_t *buckets[SC_SESSION_MAP_CAP];
    size_t count;
} sc_session_manager_t;

sc_error_t sc_session_manager_init(sc_session_manager_t *mgr, sc_allocator_t *alloc);
void sc_session_manager_deinit(sc_session_manager_t *mgr);

sc_session_t *sc_session_get_or_create(sc_session_manager_t *mgr, const char *session_key);

sc_error_t sc_session_append_message(sc_session_t *s, sc_allocator_t *alloc,
    const char *role, const char *content);

char *sc_session_gen_id(sc_allocator_t *alloc);

size_t sc_session_evict_idle(sc_session_manager_t *mgr, uint64_t max_idle_secs);

size_t sc_session_count(const sc_session_manager_t *mgr);

sc_session_summary_t *sc_session_list(sc_session_manager_t *mgr,
    sc_allocator_t *alloc, size_t *out_count);

sc_error_t sc_session_delete(sc_session_manager_t *mgr, const char *session_key);

sc_error_t sc_session_patch(sc_session_manager_t *mgr, const char *session_key,
    const char *label);

sc_error_t sc_session_save(sc_session_manager_t *mgr, const char *path);
sc_error_t sc_session_load(sc_session_manager_t *mgr, const char *path);

#endif
