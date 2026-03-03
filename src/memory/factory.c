#include "seaclaw/memory/factory.h"
#include "seaclaw/memory/engines.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SC_MEM_PATH_MAX 1024

sc_memory_t sc_memory_create_from_config(sc_allocator_t *alloc, const sc_config_t *cfg,
                                         const char *ws) {
    const char *backend = cfg->memory.backend;
    if (!backend)
        backend = "markdown";

#ifdef SC_ENABLE_SQLITE
    if (strcmp(backend, "sqlite") == 0) {
        const char *path = cfg->memory.sqlite_path;
        char buf[SC_MEM_PATH_MAX];
        if (!path) {
            const char *home = getenv("HOME");
            if (home) {
                int n = snprintf(buf, sizeof(buf), "%s/.seaclaw/memory.db", home);
                if (n > 0 && (size_t)n < sizeof(buf))
                    path = buf;
            }
        }
        if (path)
            return sc_sqlite_memory_create(alloc, path);
#ifdef SC_HAS_MARKDOWN_ENGINE
        return sc_markdown_memory_create(alloc, ws);
#endif
    }
#endif

#ifdef SC_HAS_NONE_ENGINE
    if (strcmp(backend, "none") == 0)
        return sc_none_memory_create(alloc);
#endif

    if (strcmp(backend, "lru") == 0 || strcmp(backend, "memory") == 0)
        return sc_memory_lru_create(alloc, 256);

#ifdef SC_HAS_LANCEDB_ENGINE
    if (strcmp(backend, "lancedb") == 0) {
        char buf2[SC_MEM_PATH_MAX];
        const char *home = getenv("HOME");
        if (home) {
            int n = snprintf(buf2, sizeof(buf2), "%s/.seaclaw/lancedb", home);
            if (n > 0 && (size_t)n < sizeof(buf2))
                return sc_lancedb_memory_create(alloc, buf2);
        }
        return sc_lancedb_memory_create(alloc, ".seaclaw/lancedb");
    }
#endif

#ifdef SC_HAS_LUCID_ENGINE
    if (strcmp(backend, "lucid") == 0) {
        char buf2[SC_MEM_PATH_MAX];
        const char *home = getenv("HOME");
        if (home) {
            int n = snprintf(buf2, sizeof(buf2), "%s/.seaclaw/lucid.db", home);
            if (n > 0 && (size_t)n < sizeof(buf2))
                return sc_lucid_memory_create(alloc, buf2, ws);
        }
        return sc_lucid_memory_create(alloc, ".seaclaw/lucid.db", ws);
    }
#endif

#ifdef SC_ENABLE_POSTGRES
    if (strcmp(backend, "postgres") == 0) {
        const char *url = cfg->memory.postgres_url;
        if (!url)
            url = "postgres://localhost/seaclaw";
        const char *schema = cfg->memory.postgres_schema;
        if (!schema)
            schema = "public";
        const char *table = cfg->memory.postgres_table;
        if (!table)
            table = "memories";
        return sc_postgres_memory_create(alloc, url, schema, table);
    }
#endif

#ifdef SC_ENABLE_REDIS_ENGINE
    if (strcmp(backend, "redis") == 0) {
        const char *host = cfg->memory.redis_host;
        if (!host)
            host = "localhost";
        unsigned short port = cfg->memory.redis_port;
        if (!port)
            port = 6379;
        const char *prefix = cfg->memory.redis_key_prefix;
        if (!prefix)
            prefix = "sc_mem";
        return sc_redis_memory_create(alloc, host, port, prefix);
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
        return sc_api_memory_create(alloc, base, key, timeout);
    }

#ifdef SC_HAS_MARKDOWN_ENGINE
    return sc_markdown_memory_create(alloc, ws);
#else
    (void)ws;
    sc_memory_t empty = {0};
    return empty;
#endif
}
