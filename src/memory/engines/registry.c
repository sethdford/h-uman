#include "seaclaw/memory/engines/registry.h"
#include "seaclaw/memory/engines.h"
#include "seaclaw/core/allocator.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* Static descriptors for enabled backends. Build options control which exist.
 * Returns pointer to array of descriptor pointers. */
static const sc_backend_descriptor_t *const *get_descriptors(size_t *out_count);
#if 0
static size_t descriptor_count(void);
#endif

/* ──────────────────────────────────────────────────────────────────────────
 * Descriptor definitions (one per enabled backend)
 * ────────────────────────────────────────────────────────────────────────── */

static const sc_backend_descriptor_t desc_none = {
    .name = "none",
    .label = "None — disable persistent memory",
    .auto_save_default = false,
    .capabilities = {
        .supports_keyword_rank = false,
        .supports_session_store = false,
        .supports_transactions = false,
        .supports_outbox = false,
    },
    .needs_db_path = false,
    .needs_workspace = false,
};

static const sc_backend_descriptor_t desc_markdown = {
    .name = "markdown",
    .label = "Markdown files — simple, human-readable",
    .auto_save_default = true,
    .capabilities = {
        .supports_keyword_rank = false,
        .supports_session_store = false,
        .supports_transactions = false,
        .supports_outbox = false,
    },
    .needs_db_path = false,
    .needs_workspace = true,
};

static const sc_backend_descriptor_t desc_memory = {
    .name = "memory",
    .label = "In-memory LRU — no persistence, ideal for testing",
    .auto_save_default = false,
    .capabilities = {
        .supports_keyword_rank = false,
        .supports_session_store = false,
        .supports_transactions = false,
        .supports_outbox = false,
    },
    .needs_db_path = false,
    .needs_workspace = false,
};

static const sc_backend_descriptor_t desc_sqlite = {
    .name = "sqlite",
    .label = "SQLite with FTS5 search (recommended)",
    .auto_save_default = true,
    .capabilities = {
        .supports_keyword_rank = true,
        .supports_session_store = true,
        .supports_transactions = true,
        .supports_outbox = true,
    },
    .needs_db_path = true,
    .needs_workspace = false,
};

#ifdef SC_ENABLE_POSTGRES
static const sc_backend_descriptor_t desc_postgres = {
    .name = "postgres",
    .label = "PostgreSQL — remote/shared memory store",
    .auto_save_default = true,
    .capabilities = {
        .supports_keyword_rank = false,
        .supports_session_store = true,
        .supports_transactions = true,
        .supports_outbox = true,
    },
    .needs_db_path = false,
    .needs_workspace = false,
};
#endif

static const sc_backend_descriptor_t desc_api = {
    .name = "api",
    .label = "Remote API — delegated memory via HTTP",
    .auto_save_default = true,
    .capabilities = {
        .supports_keyword_rank = false,
        .supports_session_store = false,
        .supports_transactions = false,
        .supports_outbox = false,
    },
    .needs_db_path = false,
    .needs_workspace = false,
};

#ifdef SC_ENABLE_REDIS_ENGINE
static const sc_backend_descriptor_t desc_redis = {
    .name = "redis",
    .label = "Redis — in-memory key-value store",
    .auto_save_default = true,
    .capabilities = {
        .supports_keyword_rank = false,
        .supports_session_store = false,
        .supports_transactions = false,
        .supports_outbox = false,
    },
    .needs_db_path = false,
    .needs_workspace = false,
};
#endif

static const sc_backend_descriptor_t desc_lucid = {
    .name = "lucid",
    .label = "Lucid — SQLite-backed with contextual retrieval",
    .auto_save_default = true,
    .capabilities = {
        .supports_keyword_rank = true,
        .supports_session_store = true,
        .supports_transactions = true,
        .supports_outbox = false,
    },
    .needs_db_path = true,
    .needs_workspace = false,
};

static const sc_backend_descriptor_t desc_lancedb = {
    .name = "lancedb",
    .label = "LanceDB — SQLite-backed with vector search",
    .auto_save_default = true,
    .capabilities = {
        .supports_keyword_rank = true,
        .supports_session_store = false,
        .supports_transactions = true,
        .supports_outbox = false,
    },
    .needs_db_path = true,
    .needs_workspace = false,
};

/* Known names (all possible backends, not necessarily enabled) */
static const char *const known_names[] = {
    "none", "markdown", "memory", "api", "sqlite", "lucid", "redis", "lancedb", "postgres",
};
static const size_t known_count = sizeof(known_names) / sizeof(known_names[0]);

static const sc_backend_descriptor_t *const *get_descriptors(size_t *out_count) {
    static const sc_backend_descriptor_t *list[16];
    size_t n = 0;
#ifdef SC_HAS_NONE_ENGINE
    list[n++] = &desc_none;
#endif
#ifdef SC_HAS_MARKDOWN_ENGINE
    list[n++] = &desc_markdown;
#endif
#ifdef SC_HAS_MEMORY_LRU_ENGINE
    list[n++] = &desc_memory;
#endif
#ifdef SC_ENABLE_SQLITE
    list[n++] = &desc_sqlite;
#endif
#ifdef SC_ENABLE_POSTGRES
    list[n++] = &desc_postgres;
#endif
    list[n++] = &desc_api;
#ifdef SC_ENABLE_REDIS_ENGINE
    list[n++] = &desc_redis;
#endif
#ifdef SC_HAS_LUCID_ENGINE
    list[n++] = &desc_lucid;
#endif
#ifdef SC_HAS_LANCEDB_ENGINE
    list[n++] = &desc_lancedb;
#endif
    *out_count = n;
    return (n > 0) ? list : NULL;
}

#if 0
static size_t descriptor_count(void) {
    size_t c;
    get_descriptors(&c);
    return c;
}
#endif

const sc_backend_descriptor_t *sc_registry_find_backend(const char *name, size_t name_len) {
    if (!name) return NULL;
    size_t n;
    const sc_backend_descriptor_t *const *list = get_descriptors(&n);
    if (!list) return NULL;
    for (size_t i = 0; i < n; i++) {
        const sc_backend_descriptor_t *d = list[i];
        if (!d) continue;
        size_t dlen = strlen(d->name);
        if (dlen == name_len && memcmp(d->name, name, name_len) == 0)
            return d;
    }
    return NULL;
}

bool sc_registry_is_known_backend(const char *name, size_t name_len) {
    if (!name) return false;
    for (size_t i = 0; i < known_count; i++) {
        size_t dlen = strlen(known_names[i]);
        if (dlen == name_len && memcmp(known_names[i], name, name_len) == 0)
            return true;
    }
    return false;
}

const char *sc_registry_engine_token_for_backend(const char *name, size_t name_len) {
    if (!name) return NULL;
    static const struct { const char *n; const char *t; } map[] = {
        {"none", "none"}, {"markdown", "markdown"}, {"memory", "memory"},
        {"api", "api"}, {"sqlite", "sqlite"}, {"lucid", "lucid"},
        {"redis", "redis"}, {"lancedb", "lancedb"}, {"postgres", "postgres"},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        size_t dlen = strlen(map[i].n);
        if (dlen == name_len && memcmp(map[i].n, name, name_len) == 0)
            return map[i].t;
    }
    return NULL;
}

char *sc_registry_format_enabled_backends(sc_allocator_t *alloc) {
    size_t n;
    const sc_backend_descriptor_t *const *list = get_descriptors(&n);
    if (!list || n == 0) {
        char *out = (char *)alloc->alloc(alloc->ctx, 8);
        if (out) {
            memcpy(out, "(none)", 7);
            out[7] = '\0';
        }
        return out;
    }
    /* Estimate size: names + commas + spaces */
    size_t total = 0;
    for (size_t i = 0; i < n; i++) {
        if (list[i]) total += strlen(list[i]->name) + 2;
    }
    char *buf = (char *)alloc->alloc(alloc->ctx, total + 1);
    if (!buf) return NULL;
    char *p = buf;
    for (size_t i = 0; i < n; i++) {
        const sc_backend_descriptor_t *d = list[i];
        if (!d) continue;
        if (i > 0) { *p++ = ','; *p++ = ' '; }
        size_t len = strlen(d->name);
        memcpy(p, d->name, len + 1);
        p += len;
    }
    *p = '\0';
    return buf;
}
