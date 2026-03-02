#ifndef SC_MEMORY_H
#define SC_MEMORY_H

#include "core/allocator.h"
#include "core/error.h"
#include "core/slice.h"
#include <stdbool.h>
#include <stddef.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Memory types — categories, entries, session store
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum sc_memory_category_tag {
    SC_MEMORY_CATEGORY_CORE,
    SC_MEMORY_CATEGORY_DAILY,
    SC_MEMORY_CATEGORY_CONVERSATION,
    SC_MEMORY_CATEGORY_CUSTOM,
} sc_memory_category_tag_t;

typedef struct sc_memory_category {
    sc_memory_category_tag_t tag;
    union {
        struct { /* SC_MEMORY_CATEGORY_CUSTOM */
            const char *name;
            size_t name_len;
        } custom;
    } data;
} sc_memory_category_t;

typedef struct sc_memory_entry {
    const char *id;
    size_t id_len;
    const char *key;
    size_t key_len;
    const char *content;
    size_t content_len;
    sc_memory_category_t category;
    const char *timestamp;
    size_t timestamp_len;
    const char *session_id;  /* optional, NULL if none */
    size_t session_id_len;    /* 0 if session_id is NULL */
    double score;             /* optional, NAN if not set */
} sc_memory_entry_t;

typedef struct sc_message_entry {
    const char *role;
    size_t role_len;
    const char *content;
    size_t content_len;
} sc_message_entry_t;

/* ──────────────────────────────────────────────────────────────────────────
 * SessionStore vtable
 * ────────────────────────────────────────────────────────────────────────── */

struct sc_session_store_vtable;

typedef struct sc_session_store {
    void *ctx;
    const struct sc_session_store_vtable *vtable;
} sc_session_store_t;

typedef struct sc_session_store_vtable {
    sc_error_t (*save_message)(void *ctx,
        const char *session_id, size_t session_id_len,
        const char *role, size_t role_len,
        const char *content, size_t content_len);
    sc_error_t (*load_messages)(void *ctx, sc_allocator_t *alloc,
        const char *session_id, size_t session_id_len,
        sc_message_entry_t **out, size_t *out_count);
    sc_error_t (*clear_messages)(void *ctx,
        const char *session_id, size_t session_id_len);
    sc_error_t (*clear_auto_saved)(void *ctx,
        const char *session_id, size_t session_id_len); /* NULL = all sessions */
} sc_session_store_vtable_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Memory vtable
 * ────────────────────────────────────────────────────────────────────────── */

struct sc_memory_vtable;

typedef struct sc_memory {
    void *ctx;
    const struct sc_memory_vtable *vtable;
} sc_memory_t;

typedef struct sc_memory_vtable {
    const char *(*name)(void *ctx);
    sc_error_t (*store)(void *ctx,
        const char *key, size_t key_len,
        const char *content, size_t content_len,
        const sc_memory_category_t *category,
        const char *session_id, size_t session_id_len);
    sc_error_t (*recall)(void *ctx, sc_allocator_t *alloc,
        const char *query, size_t query_len,
        size_t limit,
        const char *session_id, size_t session_id_len,
        sc_memory_entry_t **out, size_t *out_count);
    sc_error_t (*get)(void *ctx, sc_allocator_t *alloc,
        const char *key, size_t key_len,
        sc_memory_entry_t *out, bool *found);
    sc_error_t (*list)(void *ctx, sc_allocator_t *alloc,
        const sc_memory_category_t *category, /* NULL = all */
        const char *session_id, size_t session_id_len,
        sc_memory_entry_t **out, size_t *out_count);
    sc_error_t (*forget)(void *ctx,
        const char *key, size_t key_len,
        bool *deleted);
    sc_error_t (*count)(void *ctx, size_t *out);
    bool (*health_check)(void *ctx);
    void (*deinit)(void *ctx);
} sc_memory_vtable_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Factory functions (when SC_ENABLE_SQLITE: sqlite; else none)
 * ────────────────────────────────────────────────────────────────────────── */

/* Free heap-allocated fields of an entry (from list/recall). Does not free the struct. */
void sc_memory_entry_free_fields(sc_allocator_t *alloc, sc_memory_entry_t *e);

sc_memory_t sc_none_memory_create(sc_allocator_t *alloc);
sc_memory_t sc_sqlite_memory_create(sc_allocator_t *alloc, const char *db_path);
sc_session_store_t sc_sqlite_memory_get_session_store(sc_memory_t *mem);
sc_memory_t sc_markdown_memory_create(sc_allocator_t *alloc, const char *dir_path);

#endif /* SC_MEMORY_H */
