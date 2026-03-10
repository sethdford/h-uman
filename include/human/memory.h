#ifndef HU_MEMORY_H
#define HU_MEMORY_H

#include "core/allocator.h"
#include "core/error.h"
#include "core/slice.h"
#include <stdbool.h>
#include <stddef.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Memory types — categories, entries, session store
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum hu_memory_category_tag {
    HU_MEMORY_CATEGORY_CORE,
    HU_MEMORY_CATEGORY_DAILY,
    HU_MEMORY_CATEGORY_CONVERSATION,
    HU_MEMORY_CATEGORY_INSIGHT,
    HU_MEMORY_CATEGORY_CUSTOM,
} hu_memory_category_tag_t;

typedef struct hu_memory_category {
    hu_memory_category_tag_t tag;
    union {
        struct { /* HU_MEMORY_CATEGORY_CUSTOM */
            const char *name;
            size_t name_len;
        } custom;
    } data;
} hu_memory_category_t;

typedef struct hu_memory_entry {
    const char *id;
    size_t id_len;
    const char *key;
    size_t key_len;
    const char *content;
    size_t content_len;
    hu_memory_category_t category;
    const char *timestamp;
    size_t timestamp_len;
    const char *session_id; /* optional, NULL if none */
    size_t session_id_len;  /* 0 if session_id is NULL */
    const char *source;     /* optional provenance URI, NULL if none */
    size_t source_len;      /* 0 if source is NULL */
    double score;           /* optional, NAN if not set */
} hu_memory_entry_t;

typedef struct hu_message_entry {
    const char *role;
    size_t role_len;
    const char *content;
    size_t content_len;
} hu_message_entry_t;

/* ──────────────────────────────────────────────────────────────────────────
 * SessionStore vtable
 * ────────────────────────────────────────────────────────────────────────── */

struct hu_session_store_vtable;

typedef struct hu_session_store {
    void *ctx;
    const struct hu_session_store_vtable *vtable;
} hu_session_store_t;

typedef struct hu_session_store_vtable {
    hu_error_t (*save_message)(void *ctx, const char *session_id, size_t session_id_len,
                               const char *role, size_t role_len, const char *content,
                               size_t content_len);
    hu_error_t (*load_messages)(void *ctx, hu_allocator_t *alloc, const char *session_id,
                                size_t session_id_len, hu_message_entry_t **out, size_t *out_count);
    hu_error_t (*clear_messages)(void *ctx, const char *session_id, size_t session_id_len);
    hu_error_t (*clear_auto_saved)(void *ctx, const char *session_id,
                                   size_t session_id_len); /* NULL = all sessions */
} hu_session_store_vtable_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Extended store options (for store_ex)
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_memory_store_opts {
    const char *source;
    size_t source_len;
    double importance; /* <0 = unset */
} hu_memory_store_opts_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Memory vtable
 * ────────────────────────────────────────────────────────────────────────── */

struct hu_memory_vtable;

typedef struct hu_memory {
    void *ctx;
    const struct hu_memory_vtable *vtable;
    const char *current_session_id;
    size_t current_session_id_len;
} hu_memory_t;

typedef struct hu_memory_vtable {
    const char *(*name)(void *ctx);
    hu_error_t (*store)(void *ctx, const char *key, size_t key_len, const char *content,
                        size_t content_len, const hu_memory_category_t *category,
                        const char *session_id, size_t session_id_len);
    hu_error_t (*store_ex)(void *ctx, const char *key, size_t key_len, const char *content,
                           size_t content_len, const hu_memory_category_t *category,
                           const char *session_id, size_t session_id_len,
                           const hu_memory_store_opts_t *opts);
    hu_error_t (*recall)(void *ctx, hu_allocator_t *alloc, const char *query, size_t query_len,
                         size_t limit, const char *session_id, size_t session_id_len,
                         hu_memory_entry_t **out, size_t *out_count);
    hu_error_t (*get)(void *ctx, hu_allocator_t *alloc, const char *key, size_t key_len,
                      hu_memory_entry_t *out, bool *found);
    hu_error_t (*list)(void *ctx, hu_allocator_t *alloc,
                       const hu_memory_category_t *category, /* NULL = all */
                       const char *session_id, size_t session_id_len, hu_memory_entry_t **out,
                       size_t *out_count);
    hu_error_t (*forget)(void *ctx, const char *key, size_t key_len, bool *deleted);
    hu_error_t (*count)(void *ctx, size_t *out);
    bool (*health_check)(void *ctx);
    void (*deinit)(void *ctx);
} hu_memory_vtable_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Factory functions (when HU_ENABLE_SQLITE: sqlite; else none)
 * ────────────────────────────────────────────────────────────────────────── */

/* Free heap-allocated fields of an entry (from list/recall). Does not free the struct. */
void hu_memory_entry_free_fields(hu_allocator_t *alloc, hu_memory_entry_t *e);

/* Store with source provenance: uses store_ex if available, else falls back to store. */
hu_error_t hu_memory_store_with_source(hu_memory_t *mem, const char *key, size_t key_len,
                                       const char *content, size_t content_len,
                                       const hu_memory_category_t *category, const char *session_id,
                                       size_t session_id_len, const char *source,
                                       size_t source_len);

hu_memory_t hu_none_memory_create(hu_allocator_t *alloc);
hu_memory_t hu_sqlite_memory_create(hu_allocator_t *alloc, const char *db_path);
hu_session_store_t hu_sqlite_memory_get_session_store(hu_memory_t *mem);
hu_memory_t hu_markdown_memory_create(hu_allocator_t *alloc, const char *dir_path);

#endif /* HU_MEMORY_H */
