#ifndef HU_MEMORY_H
#define HU_MEMORY_H

#include "core/allocator.h"
#include "core/error.h"
#include "core/slice.h"
#include "memory/trust.h"
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
    /* SOTA-2026 init-09 fields. Default 0 (UNTRUSTED) only when the row
     * predates the M5 migration AND the upgrade audit hasn't yet run;
     * production rows carry an explicit tier. `provenance` is JSON-encoded
     * channel/handle/source_ts when non-NULL and is caller-freed (via
     * `hu_memory_entry_free_fields`) just like the other string fields. */
    int trust_tier;
    const char *provenance;
    size_t provenance_len;
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

struct hu_legacy_memory_vtable;

typedef struct hu_legacy_memory {
    void *ctx;
    const struct hu_legacy_memory_vtable *vtable;
    const char *current_session_id;
    size_t current_session_id_len;
} hu_legacy_memory_t;

/* Legacy vector-store handle. W7 dispatching facade uses `hu_memory_facade_t`
 * in `human/memory/memory.h` — names no longer collide (Phase 0). */
typedef hu_legacy_memory_t hu_memory_t;

typedef struct hu_legacy_memory_vtable {
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
} hu_legacy_memory_vtable_t;

typedef hu_legacy_memory_vtable_t hu_memory_vtable_t;

/* ──────────────────────────────────────────────────────────────────────────
 * Factory functions (when HU_ENABLE_SQLITE: sqlite; else none)
 * ────────────────────────────────────────────────────────────────────────── */

/* Free heap-allocated fields of an entry (from list/recall). Does not free the struct. */
void hu_memory_entry_free_fields(hu_allocator_t *alloc, hu_memory_entry_t *e);

/* Export all memory entries to a JSON file at `output_path`. Streams entries
 * so arbitrarily large stores don't materialise as a single string. Returns
 * HU_ERR_IO on file-open failure, or the error from vtable->list. */
hu_error_t hu_memory_export_json(hu_memory_t *mem, hu_allocator_t *alloc, const char *output_path);

/* Store with source provenance: uses store_ex if available, else falls back to store. */
hu_error_t hu_memory_store_with_source(hu_memory_t *mem, const char *key, size_t key_len,
                                       const char *content, size_t content_len,
                                       const hu_memory_category_t *category, const char *session_id,
                                       size_t session_id_len, const char *source,
                                       size_t source_len);

/* Contact-scoped recall: wraps memory store/recall with contact filtering.
 * Stores with key prefix contact:<contact_id>:<key>; recalls by contact scope. */
hu_error_t hu_memory_store_for_contact(hu_memory_t *mem, const char *contact_id,
                                       size_t contact_id_len, const char *key, size_t key_len,
                                       const char *content, size_t content_len,
                                       const hu_memory_category_t *category, const char *session_id,
                                       size_t session_id_len);

hu_error_t hu_memory_recall_for_contact(hu_memory_t *mem, hu_allocator_t *alloc,
                                        const char *contact_id, size_t contact_id_len,
                                        const char *query, size_t query_len, size_t limit,
                                        const char *session_id, size_t session_id_len,
                                        hu_memory_entry_t **out, size_t *out_count);

hu_memory_t hu_none_memory_create(hu_allocator_t *alloc);
hu_memory_t hu_sqlite_memory_create(hu_allocator_t *alloc, const char *db_path);
hu_session_store_t hu_sqlite_memory_get_session_store(hu_memory_t *mem);

/* Semantic index (Phase 2, 2026-09-01). Attach an embedder + vector store to a
 * sqlite engine: every subsequent store() embeds the row (keyed by `key`).
 * Both pointers are borrowed, not owned. */
struct hu_embedder;
struct hu_vector_store;
void hu_sqlite_memory_set_semantic_index(hu_memory_t *mem, struct hu_embedder *embedder,
                                         struct hu_vector_store *store);
/* Embed every `memories` row missing from the index (up to `limit`, 0 = all).
 * HU_ERR_NOT_SUPPORTED when no index is attached — never a silent 0. */
hu_error_t hu_sqlite_memory_reindex_semantic(hu_memory_t *mem, size_t limit, size_t *indexed_out);

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
sqlite3 *hu_sqlite_memory_get_db(hu_memory_t *mem);
#endif

/* W15 envelope encryption opt-in. Attach a keystore to a sqlite
 * memory backend; when `encrypt_at_rest` is true and `ks` is
 * unlocked, every store wraps `content` via
 * `hu_encrypted_store_wrap` and every read transparently unwraps
 * rows that carry the envelope magic. Legacy plaintext rows
 * remain readable unchanged. Pass `ks=NULL` and
 * `encrypt_at_rest=false` to disable.
 *
 * Returns HU_ERR_NOT_SUPPORTED when `mem` is not a sqlite-backed
 * memory, or HU_ERR_INVALID_ARGUMENT when `encrypt_at_rest=true`
 * is requested without a keystore. */
struct hu_keystore;
hu_error_t hu_sqlite_memory_attach_keystore(hu_memory_t *mem, struct hu_keystore *ks,
                                            bool encrypt_at_rest);
hu_memory_t hu_markdown_memory_create(hu_allocator_t *alloc, const char *dir_path);

#endif /* HU_MEMORY_H */
