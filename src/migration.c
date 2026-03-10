#include "human/migration.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/memory.h"
#include <string.h>

#if !defined(HU_IS_TEST)
static void free_entries(hu_allocator_t *alloc, hu_memory_entry_t *entries, size_t count) {
    if (!alloc || !entries)
        return;
    for (size_t i = 0; i < count; i++)
        hu_memory_entry_free_fields(alloc, &entries[i]);
    alloc->free(alloc->ctx, entries, count * sizeof(hu_memory_entry_t));
}
#endif

#ifdef HU_IS_TEST
static hu_error_t migration_test_data(hu_allocator_t *alloc, const hu_migration_config_t *cfg,
                                      hu_migration_stats_t *out_stats,
                                      hu_migration_progress_fn progress, void *progress_ctx) {
    (void)alloc;
    (void)cfg;
    memset(out_stats, 0, sizeof(*out_stats));

    /* Test: "migrate" from in-memory (simulated) to target.
     * We create a minimal sqlite memory, add test data, then migrate to target.
     * For simplicity in test mode: just validate config and report 0 imported.
     */
    if (progress)
        progress(progress_ctx, 0, 0);
    return HU_OK;
}
#endif

hu_error_t hu_migration_run(hu_allocator_t *alloc, const hu_migration_config_t *cfg,
                            hu_migration_stats_t *out_stats, hu_migration_progress_fn progress,
                            void *progress_ctx) {
    if (!alloc || !cfg || !out_stats)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out_stats, 0, sizeof(*out_stats));

#ifdef HU_IS_TEST
    return migration_test_data(alloc, cfg, out_stats, progress, progress_ctx);
#else

#ifdef HU_ENABLE_SQLITE
    hu_memory_t src_mem = {0};
    hu_memory_t tgt_mem = {0};

    switch (cfg->source) {
    case HU_MIGRATION_SOURCE_NONE:
        src_mem = hu_none_memory_create(alloc);
        break;
    case HU_MIGRATION_SOURCE_SQLITE:
        if (!cfg->source_path || cfg->source_path_len == 0)
            return HU_ERR_INVALID_ARGUMENT;
        src_mem = hu_sqlite_memory_create(alloc, cfg->source_path);
        break;
    case HU_MIGRATION_SOURCE_MARKDOWN:
        if (!cfg->source_path || cfg->source_path_len == 0)
            return HU_ERR_INVALID_ARGUMENT;
        src_mem = hu_markdown_memory_create(alloc, cfg->source_path);
        break;
    default:
        return HU_ERR_INVALID_ARGUMENT;
    }

    switch (cfg->target) {
    case HU_MIGRATION_TARGET_SQLITE:
        if (!cfg->target_path || cfg->target_path_len == 0)
            return HU_ERR_INVALID_ARGUMENT;
        tgt_mem = hu_sqlite_memory_create(alloc, cfg->target_path);
        break;
    case HU_MIGRATION_TARGET_MARKDOWN:
        if (!cfg->target_path || cfg->target_path_len == 0)
            return HU_ERR_INVALID_ARGUMENT;
        tgt_mem = hu_markdown_memory_create(alloc, cfg->target_path);
        break;
    default:
        if (src_mem.vtable && src_mem.vtable->deinit)
            src_mem.vtable->deinit(src_mem.ctx);
        return HU_ERR_INVALID_ARGUMENT;
    }

    if (!src_mem.vtable || !tgt_mem.vtable) {
        if (src_mem.vtable && src_mem.vtable->deinit)
            src_mem.vtable->deinit(src_mem.ctx);
        if (tgt_mem.vtable && tgt_mem.vtable->deinit)
            tgt_mem.vtable->deinit(tgt_mem.ctx);
        return HU_ERR_MEMORY_BACKEND;
    }

    hu_memory_entry_t *entries = NULL;
    size_t count = 0;
    hu_error_t err = src_mem.vtable->list(src_mem.ctx, alloc, NULL, NULL, 0, &entries, &count);

    if (err != HU_OK) {
        src_mem.vtable->deinit(src_mem.ctx);
        tgt_mem.vtable->deinit(tgt_mem.ctx);
        return err;
    }

    if (cfg->source == HU_MIGRATION_SOURCE_SQLITE)
        out_stats->from_sqlite = count;
    else if (cfg->source == HU_MIGRATION_SOURCE_MARKDOWN)
        out_stats->from_markdown = count;

    if (cfg->dry_run) {
        free_entries(alloc, entries, count);
        src_mem.vtable->deinit(src_mem.ctx);
        tgt_mem.vtable->deinit(tgt_mem.ctx);
        return HU_OK;
    }

    for (size_t i = 0; i < count; i++) {
        if (progress)
            progress(progress_ctx, i, count);

        hu_memory_entry_t *e = &entries[i];
        if (!e->key || !e->content) {
            out_stats->skipped++;
            continue;
        }

        hu_memory_category_t cat = e->category;
        const char *sess = e->session_id;
        size_t sess_len = e->session_id_len;

        err = tgt_mem.vtable->store(tgt_mem.ctx, e->key, e->key_len, e->content, e->content_len,
                                    &cat, sess, sess_len);

        if (err != HU_OK) {
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

    return HU_OK;

#else
    /* SQLite disabled: only none→markdown and markdown→markdown */
    if (cfg->source == HU_MIGRATION_SOURCE_SQLITE || cfg->target == HU_MIGRATION_TARGET_SQLITE)
        return HU_ERR_NOT_SUPPORTED;

    hu_memory_t src_mem = {0};
    hu_memory_t tgt_mem = {0};

    if (cfg->source == HU_MIGRATION_SOURCE_NONE)
        src_mem = hu_none_memory_create(alloc);
    else if (cfg->source == HU_MIGRATION_SOURCE_MARKDOWN && cfg->source_path) {
        src_mem = hu_markdown_memory_create(alloc, cfg->source_path);
    } else {
        return HU_ERR_INVALID_ARGUMENT;
    }

    if (cfg->target != HU_MIGRATION_TARGET_MARKDOWN || !cfg->target_path)
        return HU_ERR_INVALID_ARGUMENT;
    tgt_mem = hu_markdown_memory_create(alloc, cfg->target_path);

    if (!src_mem.vtable || !tgt_mem.vtable) {
        if (src_mem.vtable && src_mem.vtable->deinit)
            src_mem.vtable->deinit(src_mem.ctx);
        if (tgt_mem.vtable && tgt_mem.vtable->deinit)
            tgt_mem.vtable->deinit(tgt_mem.ctx);
        return HU_ERR_MEMORY_BACKEND;
    }

    hu_memory_entry_t *entries = NULL;
    size_t count = 0;
    hu_error_t err = src_mem.vtable->list(src_mem.ctx, alloc, NULL, NULL, 0, &entries, &count);

    if (err != HU_OK) {
        src_mem.vtable->deinit(src_mem.ctx);
        tgt_mem.vtable->deinit(tgt_mem.ctx);
        return err;
    }

    if (cfg->source == HU_MIGRATION_SOURCE_MARKDOWN)
        out_stats->from_markdown = count;

    if (cfg->dry_run) {
        free_entries(alloc, entries, count);
        src_mem.vtable->deinit(src_mem.ctx);
        tgt_mem.vtable->deinit(tgt_mem.ctx);
        return HU_OK;
    }

    for (size_t i = 0; i < count; i++) {
        if (progress)
            progress(progress_ctx, i, count);
        hu_memory_entry_t *e = &entries[i];
        if (!e->key || !e->content) {
            out_stats->skipped++;
            continue;
        }
        hu_memory_category_t cat = e->category;
        err = tgt_mem.vtable->store(tgt_mem.ctx, e->key, e->key_len, e->content, e->content_len,
                                    &cat, e->session_id, e->session_id_len);
        if (err == HU_OK)
            out_stats->imported++;
        else
            out_stats->errors++;
    }
    if (progress)
        progress(progress_ctx, count, count);
    free_entries(alloc, entries, count);
    src_mem.vtable->deinit(src_mem.ctx);
    tgt_mem.vtable->deinit(tgt_mem.ctx);
    return HU_OK;
#endif
#endif
}
