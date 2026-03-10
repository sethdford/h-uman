---
title: Memory API
description: hu_memory_t vtable, session store, and retrieval engines
updated: 2026-03-02
---

# Memory API

Memory backends implement the `hu_memory_t` vtable for storing and recalling context. Session store and retrieval engines extend this.

## Types

### Categories and Entries

```c
typedef enum hu_memory_category_tag {
    HU_MEMORY_CATEGORY_CORE,
    HU_MEMORY_CATEGORY_DAILY,
    HU_MEMORY_CATEGORY_CONVERSATION,
    HU_MEMORY_CATEGORY_CUSTOM,
} hu_memory_category_tag_t;

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
    const char *session_id;
    size_t session_id_len;
    double score;
} hu_memory_entry_t;
```

### Memory Vtable

```c
typedef struct hu_memory {
    void *ctx;
    const struct hu_memory_vtable *vtable;
} hu_memory_t;

typedef struct hu_memory_vtable {
    const char *(*name)(void *ctx);
    hu_error_t (*store)(void *ctx,
        const char *key, size_t key_len,
        const char *content, size_t content_len,
        const hu_memory_category_t *category,
        const char *session_id, size_t session_id_len);
    hu_error_t (*recall)(void *ctx, hu_allocator_t *alloc,
        const char *query, size_t query_len,
        size_t limit,
        const char *session_id, size_t session_id_len,
        hu_memory_entry_t **out, size_t *out_count);
    hu_error_t (*get)(void *ctx, hu_allocator_t *alloc,
        const char *key, size_t key_len,
        hu_memory_entry_t *out, bool *found);
    hu_error_t (*list)(void *ctx, hu_allocator_t *alloc,
        const hu_memory_category_t *category,
        const char *session_id, size_t session_id_len,
        hu_memory_entry_t **out, size_t *out_count);
    hu_error_t (*forget)(void *ctx,
        const char *key, size_t key_len,
        bool *deleted);
    hu_error_t (*count)(void *ctx, size_t *out);
    bool (*health_check)(void *ctx);
    void (*deinit)(void *ctx);
} hu_memory_vtable_t;
```

### Session Store

```c
typedef struct hu_session_store {
    void *ctx;
    const struct hu_session_store_vtable *vtable;
} hu_session_store_t;

typedef struct hu_session_store_vtable {
    hu_error_t (*save_message)(void *ctx,
        const char *session_id, size_t session_id_len,
        const char *role, size_t role_len,
        const char *content, size_t content_len);
    hu_error_t (*load_messages)(void *ctx, hu_allocator_t *alloc,
        const char *session_id, size_t session_id_len,
        hu_message_entry_t **out, size_t *out_count);
    hu_error_t (*clear_messages)(void *ctx,
        const char *session_id, size_t session_id_len);
    hu_error_t (*clear_auto_saved)(void *ctx,
        const char *session_id, size_t session_id_len);
} hu_session_store_vtable_t;
```

## Factory Functions

```c
hu_memory_t hu_none_memory_create(hu_allocator_t *alloc);
hu_memory_t hu_sqlite_memory_create(hu_allocator_t *alloc, const char *db_path);
hu_session_store_t hu_sqlite_memory_get_session_store(hu_memory_t *mem);
hu_memory_t hu_markdown_memory_create(hu_allocator_t *alloc, const char *dir_path);
```

## Retrieval Engine

```c
typedef enum hu_retrieval_mode {
    HU_RETRIEVAL_KEYWORD,
    HU_RETRIEVAL_SEMANTIC,
    HU_RETRIEVAL_HYBRID,
} hu_retrieval_mode_t;

typedef struct hu_retrieval_options {
    hu_retrieval_mode_t mode;
    size_t limit;
    double min_score;
    bool use_reranking;
    double temporal_decay_factor;
} hu_retrieval_options_t;

hu_retrieval_engine_t hu_retrieval_create(hu_allocator_t *alloc, hu_memory_t *backend);
hu_retrieval_engine_t hu_retrieval_create_with_vector(hu_allocator_t *alloc,
    hu_memory_t *backend, hu_embedder_t *embedder, hu_vector_store_t *vector_store);

void hu_retrieval_result_free(hu_allocator_t *alloc, hu_retrieval_result_t *r);
```

## Helper

```c
void hu_memory_entry_free_fields(hu_allocator_t *alloc, hu_memory_entry_t *e);
```

## Usage Example

```c
hu_allocator_t alloc = hu_system_allocator();
hu_memory_t mem = hu_sqlite_memory_create(&alloc, "/tmp/mem.db");

hu_memory_category_t cat = { .tag = HU_MEMORY_CATEGORY_DAILY };
mem.vtable->store(mem.ctx, "note", 4, "User prefers dark mode", 22,
    &cat, "sess-1", 7);

hu_memory_entry_t *entries;
size_t count;
mem.vtable->recall(mem.ctx, &alloc, "dark mode", 9, 5, NULL, 0, &entries, &count);
for (size_t i = 0; i < count; i++) {
    printf("%.*s\n", (int)entries[i].content_len, entries[i].content);
}
hu_memory_entry_free_fields(&alloc, entries);
alloc.free(alloc.ctx, entries, count * sizeof(hu_memory_entry_t));

mem.vtable->deinit(mem.ctx);
```
