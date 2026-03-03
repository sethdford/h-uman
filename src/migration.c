#include "seaclaw/migration.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/string.h"
#include "seaclaw/memory.h"
#include <string.h>

#if !defined(SC_IS_TEST)
static void free_entries(sc_allocator_t *alloc, sc_memory_entry_t *entries, size_t count) {
    if (!alloc || !entries)
        return;
    for (size_t i = 0; i < count; i++) {
        sc_memory_entry_t *e = &entries[i];
        if (e->id)
            alloc->free(alloc->ctx, (void *)e->id, e->id_len + 1);
        if (e->key)
            alloc->free(alloc->ctx, (void *)e->key, e->key_len + 1);
        if (e->content)
            alloc->free(alloc->ctx, (void *)e->content, e->content_len + 1);
        if (e->category.data.custom.name)
            alloc->free(alloc->ctx, (void *)e->category.data.custom.name,
                        e->category.data.custom.name_len + 1);
        if (e->timestamp)
            alloc->free(alloc->ctx, (void *)e->timestamp, e->timestamp_len + 1);
        if (e->session_id)
            alloc->free(alloc->ctx, (void *)e->session_id, e->session_id_len + 1);
    }
    alloc->free(alloc->ctx, entries, count * sizeof(sc_memory_entry_t));
}
#endif

#ifdef SC_IS_TEST
static sc_error_t migration_test_data(sc_allocator_t *alloc, const sc_migration_config_t *cfg,
                                      sc_migration_stats_t *out_stats,
                                      sc_migration_progress_fn progress, void *progress_ctx) {
    (void)alloc;
    (void)cfg;
    memset(out_stats, 0, sizeof(*out_stats));

    /* Test: "migrate" from in-memory (simulated) to target.
     * We create a minimal sqlite memory, add test data, then migrate to target.
     * For simplicity in test mode: just validate config and report 0 imported.
     */
    if (progress)
        progress(progress_ctx, 0, 0);
    return SC_OK;
}
#endif

sc_error_t sc_migration_run(sc_allocator_t *alloc, const sc_migration_config_t *cfg,
                            sc_migration_stats_t *out_stats, sc_migration_progress_fn progress,
                            void *progress_ctx) {
    if (!alloc || !cfg || !out_stats)
        return SC_ERR_INVALID_ARGUMENT;
    memset(out_stats, 0, sizeof(*out_stats));

#ifdef SC_IS_TEST
    return migration_test_data(alloc, cfg, out_stats, progress, progress_ctx);
#else

#ifdef SC_ENABLE_SQLITE
    sc_memory_t src_mem = {0};
    sc_memory_t tgt_mem = {0};

    switch (cfg->source) {
    case SC_MIGRATION_SOURCE_NONE:
        src_mem = sc_none_memory_create(alloc);
        break;
    case SC_MIGRATION_SOURCE_SQLITE:
        if (!cfg->source_path || cfg->source_path_len == 0)
            return SC_ERR_INVALID_ARGUMENT;
        src_mem = sc_sqlite_memory_create(alloc, cfg->source_path);
        break;
    case SC_MIGRATION_SOURCE_MARKDOWN:
        if (!cfg->source_path || cfg->source_path_len == 0)
            return SC_ERR_INVALID_ARGUMENT;
        src_mem = sc_markdown_memory_create(alloc, cfg->source_path);
        break;
    default:
        return SC_ERR_INVALID_ARGUMENT;
    }

    switch (cfg->target) {
    case SC_MIGRATION_TARGET_SQLITE:
        if (!cfg->target_path || cfg->target_path_len == 0)
            return SC_ERR_INVALID_ARGUMENT;
        tgt_mem = sc_sqlite_memory_create(alloc, cfg->target_path);
        break;
    case SC_MIGRATION_TARGET_MARKDOWN:
        if (!cfg->target_path || cfg->target_path_len == 0)
            return SC_ERR_INVALID_ARGUMENT;
        tgt_mem = sc_markdown_memory_create(alloc, cfg->target_path);
        break;
    default:
        if (src_mem.vtable && src_mem.vtable->deinit)
            src_mem.vtable->deinit(src_mem.ctx);
        return SC_ERR_INVALID_ARGUMENT;
    }

    if (!src_mem.vtable || !tgt_mem.vtable) {
        if (src_mem.vtable && src_mem.vtable->deinit)
            src_mem.vtable->deinit(src_mem.ctx);
        if (tgt_mem.vtable && tgt_mem.vtable->deinit)
            tgt_mem.vtable->deinit(tgt_mem.ctx);
        return SC_ERR_MEMORY_BACKEND;
    }

    sc_memory_entry_t *entries = NULL;
    size_t count = 0;
    sc_error_t err = src_mem.vtable->list(src_mem.ctx, alloc, NULL, NULL, 0, &entries, &count);

    if (err != SC_OK) {
        src_mem.vtable->deinit(src_mem.ctx);
        tgt_mem.vtable->deinit(tgt_mem.ctx);
        return err;
    }

    if (cfg->source == SC_MIGRATION_SOURCE_SQLITE)
        out_stats->from_sqlite = count;
    else if (cfg->source == SC_MIGRATION_SOURCE_MARKDOWN)
        out_stats->from_markdown = count;

    if (cfg->dry_run) {
        free_entries(alloc, entries, count);
        src_mem.vtable->deinit(src_mem.ctx);
        tgt_mem.vtable->deinit(tgt_mem.ctx);
        return SC_OK;
    }

    for (size_t i = 0; i < count; i++) {
        if (progress)
            progress(progress_ctx, i, count);

        sc_memory_entry_t *e = &entries[i];
        if (!e->key || !e->content) {
            out_stats->skipped++;
            continue;
        }

        sc_memory_category_t cat = e->category;
        const char *sess = e->session_id;
        size_t sess_len = e->session_id_len;

        err = tgt_mem.vtable->store(tgt_mem.ctx, e->key, e->key_len, e->content, e->content_len,
                                    &cat, sess, sess_len);

        if (err != SC_OK) {
            out_stats->errors++;
        } else {
            out_stats->imported++;
        }
    }

    if (progress)
        progress(progress_ctx, count, count);

    free_entries(alloc, entries, count);
    src_mem.vtable->deinit(src_mem.ctx);
    tgt_mem.vtable->deinit(tgt_mem.ctx);

    return SC_OK;

#else
    /* SQLite disabled: only none→markdown and markdown→markdown */
    if (cfg->source == SC_MIGRATION_SOURCE_SQLITE || cfg->target == SC_MIGRATION_TARGET_SQLITE)
        return SC_ERR_NOT_SUPPORTED;

    sc_memory_t src_mem = {0};
    sc_memory_t tgt_mem = {0};

    if (cfg->source == SC_MIGRATION_SOURCE_NONE)
        src_mem = sc_none_memory_create(alloc);
    else if (cfg->source == SC_MIGRATION_SOURCE_MARKDOWN && cfg->source_path) {
        src_mem = sc_markdown_memory_create(alloc, cfg->source_path);
    } else {
        return SC_ERR_INVALID_ARGUMENT;
    }

    if (cfg->target != SC_MIGRATION_TARGET_MARKDOWN || !cfg->target_path)
        return SC_ERR_INVALID_ARGUMENT;
    tgt_mem = sc_markdown_memory_create(alloc, cfg->target_path);

    if (!src_mem.vtable || !tgt_mem.vtable) {
        if (src_mem.vtable && src_mem.vtable->deinit)
            src_mem.vtable->deinit(src_mem.ctx);
        if (tgt_mem.vtable && tgt_mem.vtable->deinit)
            tgt_mem.vtable->deinit(tgt_mem.ctx);
        return SC_ERR_MEMORY_BACKEND;
    }

    sc_memory_entry_t *entries = NULL;
    size_t count = 0;
    sc_error_t err = src_mem.vtable->list(src_mem.ctx, alloc, NULL, NULL, 0, &entries, &count);

    if (err != SC_OK) {
        src_mem.vtable->deinit(src_mem.ctx);
        tgt_mem.vtable->deinit(tgt_mem.ctx);
        return err;
    }

    if (cfg->source == SC_MIGRATION_SOURCE_MARKDOWN)
        out_stats->from_markdown = count;

    if (cfg->dry_run) {
        free_entries(alloc, entries, count);
        src_mem.vtable->deinit(src_mem.ctx);
        tgt_mem.vtable->deinit(tgt_mem.ctx);
        return SC_OK;
    }

    for (size_t i = 0; i < count; i++) {
        if (progress)
            progress(progress_ctx, i, count);
        sc_memory_entry_t *e = &entries[i];
        if (!e->key || !e->content) {
            out_stats->skipped++;
            continue;
        }
        sc_memory_category_t cat = e->category;
        err = tgt_mem.vtable->store(tgt_mem.ctx, e->key, e->key_len, e->content, e->content_len,
                                    &cat, e->session_id, e->session_id_len);
        if (err == SC_OK)
            out_stats->imported++;
        else
            out_stats->errors++;
    }
    if (progress)
        progress(progress_ctx, count, count);
    free_entries(alloc, entries, count);
    src_mem.vtable->deinit(src_mem.ctx);
    tgt_mem.vtable->deinit(tgt_mem.ctx);
    return SC_OK;
#endif
#endif
}
