#include "human/memory/factory.h"
#include "human/memory/engines.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HU_MEM_PATH_MAX 1024

hu_memory_t hu_memory_create_from_config(hu_allocator_t *alloc, const hu_config_t *cfg,
                                         const char *ws) {
    const char *backend = cfg->memory.backend;
    if (!backend)
        backend = "markdown";

#ifdef HU_ENABLE_SQLITE
    if (strcmp(backend, "sqlite") == 0) {
        const char *path = cfg->memory.sqlite_path;
        char buf[HU_MEM_PATH_MAX];
        if (!path) {
            const char *home = getenv("HOME");
            if (home) {
                int n = snprintf(buf, sizeof(buf), "%s/.human/memory.db", home);
                if (n > 0 && (size_t)n < sizeof(buf))
                    path = buf;
            }
        }
        if (path)
            return hu_sqlite_memory_create(alloc, path);
#ifdef HU_HAS_MARKDOWN_ENGINE
        return hu_markdown_memory_create(alloc, ws);
#endif
    }
#endif

#ifdef HU_HAS_NONE_ENGINE
    if (strcmp(backend, "none") == 0)
        return hu_none_memory_create(alloc);
#endif

    if (strcmp(backend, "lru") == 0 || strcmp(backend, "memory") == 0)
        return hu_memory_lru_create(alloc, 256);

#ifdef HU_HAS_LANCEDB_ENGINE
    if (strcmp(backend, "lancedb") == 0) {
        char buf2[HU_MEM_PATH_MAX];
        const char *home = getenv("HOME");
        if (home) {
            int n = snprintf(buf2, sizeof(buf2), "%s/.human/lancedb", home);
            if (n > 0 && (size_t)n < sizeof(buf2))
                return hu_lancedb_memory_create(alloc, buf2);
        }
        return hu_lancedb_memory_create(alloc, ".human/lancedb");
    }
#endif

#ifdef HU_HAS_LUCID_ENGINE
    if (strcmp(backend, "lucid") == 0) {
        char buf2[HU_MEM_PATH_MAX];
        const char *home = getenv("HOME");
        if (home) {
            int n = snprintf(buf2, sizeof(buf2), "%s/.human/lucid.db", home);
            if (n > 0 && (size_t)n < sizeof(buf2))
                return hu_lucid_memory_create(alloc, buf2, ws);
        }
        return hu_lucid_memory_create(alloc, ".human/lucid.db", ws);
    }
#endif

#ifdef HU_ENABLE_POSTGRES
    if (strcmp(backend, "postgres") == 0) {
        const char *url = cfg->memory.postgres_url;
        if (!url)
            url = "postgres://localhost/human";
        const char *schema = cfg->memory.postgres_schema;
        if (!schema)
            schema = "public";
        const char *table = cfg->memory.postgres_table;
        if (!table)
            table = "memories";
        return hu_postgres_memory_create(alloc, url, schema, table);
    }
#endif

#ifdef HU_ENABLE_REDIS_ENGINE
    if (strcmp(backend, "redis") == 0) {
        const char *host = cfg->memory.redis_host;
        if (!host)
            host = "localhost";
        unsigned short port = cfg->memory.redis_port;
        if (!port)
            port = 6379;
        const char *prefix = cfg->memory.redis_key_prefix;
        if (!prefix)
            prefix = "hu_mem";
        return hu_redis_memory_create(alloc, host, port, prefix);
    }
#endif

    if (strcmp(backend, "api") == 0) {
        const char *base = cfg->memory.api_base_url;
        if (!base)
            base = "https://api.example.com/memory";
        const char *key = cfg->memory.api_key;
        uint32_t timeout = cfg->memory.api_timeout_ms;
        if (!timeout)
            timeout = 5000;
        return hu_api_memory_create(alloc, base, key, timeout);
    }

#ifdef HU_HAS_MARKDOWN_ENGINE
    return hu_markdown_memory_create(alloc, ws);
#else
    (void)ws;
    hu_memory_t empty = {0};
    return empty;
#endif
}
