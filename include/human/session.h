#ifndef HU_SESSION_H
#define HU_SESSION_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

#define HU_SESSION_MAP_CAP         256
#define HU_SESSION_KEY_LEN         128
#define HU_SESSION_LABEL_LEN       128
#define HU_SESSION_MSG_CAP         4096
#define HU_SESSION_MSG_ROLE_LEN    32
#define HU_SESSION_MSG_CONTENT_CAP 65536

typedef struct hu_session_message {
    char role[HU_SESSION_MSG_ROLE_LEN];
    char *content;
} hu_session_message_t;

typedef struct hu_session {
    char session_key[HU_SESSION_KEY_LEN];
    char label[HU_SESSION_LABEL_LEN];
    int64_t created_at;
    int64_t last_active;
    uint64_t turn_count;
    hu_session_message_t *messages;
    size_t message_count;
    size_t message_cap;
} hu_session_t;

typedef struct hu_session_summary {
    char session_key[HU_SESSION_KEY_LEN];
    char label[HU_SESSION_LABEL_LEN];
    int64_t created_at;
    int64_t last_active;
    uint64_t turn_count;
} hu_session_summary_t;

typedef struct hu_session_entry {
    char key[HU_SESSION_KEY_LEN];
    hu_session_t *session;
    struct hu_session_entry *next;
} hu_session_entry_t;

typedef struct hu_session_manager {
    hu_allocator_t *alloc;
    hu_session_entry_t *buckets[HU_SESSION_MAP_CAP];
    size_t count;
} hu_session_manager_t;

hu_error_t hu_session_manager_init(hu_session_manager_t *mgr, hu_allocator_t *alloc);
void hu_session_manager_deinit(hu_session_manager_t *mgr);

hu_session_t *hu_session_get_or_create(hu_session_manager_t *mgr, const char *session_key);

hu_error_t hu_session_append_message(hu_session_t *s, hu_allocator_t *alloc, const char *role,
                                     const char *content);

char *hu_session_gen_id(hu_allocator_t *alloc);

size_t hu_session_evict_idle(hu_session_manager_t *mgr, uint64_t max_idle_secs);

size_t hu_session_count(const hu_session_manager_t *mgr);

hu_session_summary_t *hu_session_list(hu_session_manager_t *mgr, hu_allocator_t *alloc,
                                      size_t *out_count);

hu_error_t hu_session_delete(hu_session_manager_t *mgr, const char *session_key);

hu_error_t hu_session_patch(hu_session_manager_t *mgr, const char *session_key, const char *label);

hu_error_t hu_session_save(hu_session_manager_t *mgr, const char *path);
hu_error_t hu_session_load(hu_session_manager_t *mgr, const char *path);

#endif
