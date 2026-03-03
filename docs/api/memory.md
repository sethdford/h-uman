---
title: Memory API
description: sc_memory_t vtable, session store, and retrieval engines
updated: 2026-03-02
---

# Memory API

Memory backends implement the `sc_memory_t` vtable for storing and recalling context. Session store and retrieval engines extend this.

## Types

### Categories and Entries

```c
typedef enum sc_memory_category_tag {
    SC_MEMORY_CATEGORY_CORE,
    SC_MEMORY_CATEGORY_DAILY,
    SC_MEMORY_CATEGORY_CONVERSATION,
    SC_MEMORY_CATEGORY_CUSTOM,
} sc_memory_category_tag_t;

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
    const char *session_id;
    size_t session_id_len;
    double score;
} sc_memory_entry_t;
```

### Memory Vtable

```c
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
        const sc_memory_category_t *category,
        const char *session_id, size_t session_id_len,
        sc_memory_entry_t **out, size_t *out_count);
    sc_error_t (*forget)(void *ctx,
        const char *key, size_t key_len,
        bool *deleted);
    sc_error_t (*count)(void *ctx, size_t *out);
    bool (*health_check)(void *ctx);
    void (*deinit)(void *ctx);
} sc_memory_vtable_t;
```

### Session Store

```c
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
        const char *session_id, size_t session_id_len);
} sc_session_store_vtable_t;
```

## Factory Functions

```c
sc_memory_t sc_none_memory_create(sc_allocator_t *alloc);
sc_memory_t sc_sqlite_memory_create(sc_allocator_t *alloc, const char *db_path);
sc_session_store_t sc_sqlite_memory_get_session_store(sc_memory_t *mem);
sc_memory_t sc_markdown_memory_create(sc_allocator_t *alloc, const char *dir_path);
```

## Retrieval Engine

```c
typedef enum sc_retrieval_mode {
    SC_RETRIEVAL_KEYWORD,
    SC_RETRIEVAL_SEMANTIC,
    SC_RETRIEVAL_HYBRID,
} sc_retrieval_mode_t;

typedef struct sc_retrieval_options {
    sc_retrieval_mode_t mode;
    size_t limit;
    double min_score;
    bool use_reranking;
    double temporal_decay_factor;
} sc_retrieval_options_t;

sc_retrieval_engine_t sc_retrieval_create(sc_allocator_t *alloc, sc_memory_t *backend);
sc_retrieval_engine_t sc_retrieval_create_with_vector(sc_allocator_t *alloc,
    sc_memory_t *backend, sc_embedder_t *embedder, sc_vector_store_t *vector_store);

void sc_retrieval_result_free(sc_allocator_t *alloc, sc_retrieval_result_t *r);
```

## Helper

```c
void sc_memory_entry_free_fields(sc_allocator_t *alloc, sc_memory_entry_t *e);
```

## Usage Example

```c
sc_allocator_t alloc = sc_system_allocator();
sc_memory_t mem = sc_sqlite_memory_create(&alloc, "/tmp/mem.db");

sc_memory_category_t cat = { .tag = SC_MEMORY_CATEGORY_DAILY };
mem.vtable->store(mem.ctx, "note", 4, "User prefers dark mode", 22,
    &cat, "sess-1", 7);

sc_memory_entry_t *entries;
size_t count;
mem.vtable->recall(mem.ctx, &alloc, "dark mode", 9, 5, NULL, 0, &entries, &count);
for (size_t i = 0; i < count; i++) {
    printf("%.*s\n", (int)entries[i].content_len, entries[i].content);
}
sc_memory_entry_free_fields(&alloc, entries);
alloc.free(alloc.ctx, entries, count * sizeof(sc_memory_entry_t));

mem.vtable->deinit(mem.ctx);
```
