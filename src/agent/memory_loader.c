#include "human/agent/memory_loader.h"
#include "human/agent/world_model_bridge.h" /* hu_w7_render_world_model + hu_persona_context_t */
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/log.h"
#include "human/core/string.h"
#include "human/memory/personal_model.h"
#include "human/memory/retrieval/adaptive.h"
#include "human/memory/trust.h"
#include <string.h>
#include <time.h>
#ifdef HU_ENABLE_SQLITE
#include "human/memory.h"
#include "human/memory/retrieval/strategy_learner.h"
#endif
#include "human/core/gate_mode.h"
#include "human/memory/contact_insights_repo.h"
#include <stdatomic.h>

/* Append `text` to *out_context as a new section (newline-joined), creating the
 * context when absent. Silent on allocation failure: the prompt is still valid
 * without the supplement. */
static void append_section(hu_memory_loader_t *loader, char **out_context, size_t *out_context_len,
                           const char *text, size_t text_len) {
    if (!text || text_len == 0)
        return;
    if (*out_context) {
        size_t old_len = out_context_len ? *out_context_len : strlen(*out_context);
        size_t total = old_len + 1 + text_len;
        char *combined = (char *)loader->alloc->alloc(loader->alloc->ctx, total + 1);
        if (!combined)
            return;
        memcpy(combined, *out_context, old_len);
        combined[old_len] = '\n';
        memcpy(combined + old_len + 1, text, text_len);
        combined[total] = '\0';
        loader->alloc->free(loader->alloc->ctx, *out_context, old_len + 1);
        *out_context = combined;
        if (out_context_len)
            *out_context_len = total;
    } else {
        *out_context = hu_strndup(loader->alloc, text, text_len);
        if (*out_context && out_context_len)
            *out_context_len = text_len;
    }
}

/* HU_INSIGHT_STREAM gate. Default OFF; -1 = read env (test seam below). */
static int s_insight_mode_override = -1;

hu_gate_mode_t hu_memory_loader_insight_mode(void) {
    if (s_insight_mode_override >= 0)
        return (hu_gate_mode_t)s_insight_mode_override;
    return hu_gate_mode_from_env("HU_INSIGHT_STREAM", HU_GATE_OFF);
}

void hu_memory_loader_set_insight_mode_for_test(int mode) {
    s_insight_mode_override = mode;
}

/* Budget for the insights block: 8 short notes, under 1 KB. With the 24 KB
 * prompt budget this fits beside recall (~1.6 KB) and the personal model
 * (~2.1 KB) without trimming on an ordinary turn. */
#define HU_INSIGHT_MAX_ITEMS      8
#define HU_INSIGHT_MAX_BYTES      900
#define HU_INSIGHT_MIN_CONFIDENCE 0.5

static const char k_insight_header[] =
    "### What you actually remember about them (weave in naturally, never recite):\n";

static void append_contact_insights(hu_memory_loader_t *loader, const char *session_id,
                                    size_t session_id_len, char **out_context,
                                    size_t *out_context_len) {
    hu_gate_mode_t mode = hu_memory_loader_insight_mode();
    if (mode == HU_GATE_OFF || !loader->memory)
        return;
    char *lines = NULL;
    size_t lines_len = 0;
    hu_error_t rerr = hu_contact_insights_render(
        loader->memory, loader->alloc, session_id, session_id_len, HU_INSIGHT_MAX_ITEMS,
        HU_INSIGHT_MAX_BYTES, HU_INSIGHT_MIN_CONFIDENCE, &lines, &lines_len);
    if (rerr != HU_OK || !lines || lines_len == 0)
        return;
    static atomic_bool announced = false;
    hu_log_info_once(&announced, "insight-stream", NULL,
                     "insight stream active: mode=%s (set HU_INSIGHT_STREAM=off to disable)",
                     mode == HU_GATE_LIVE ? "live" : "shadow");
    if (mode == HU_GATE_SHADOW) {
        hu_log_info("insight-stream", NULL,
                    "shadow: would add %zu bytes of insights for %.*s (prompt unchanged)",
                    lines_len, (int)session_id_len, session_id);
    } else {
        const size_t hdr_len = sizeof(k_insight_header) - 1;
        size_t block_len = hdr_len + lines_len;
        char *block = (char *)loader->alloc->alloc(loader->alloc->ctx, block_len + 1);
        if (block) {
            memcpy(block, k_insight_header, hdr_len);
            memcpy(block + hdr_len, lines, lines_len);
            block[block_len] = '\0';
            append_section(loader, out_context, out_context_len, block, block_len);
            loader->alloc->free(loader->alloc->ctx, block, block_len + 1);
        }
    }
    loader->alloc->free(loader->alloc->ctx, lines, lines_len + 1);
}

static hu_retrieval_mode_t adaptive_to_retrieval_mode(hu_adaptive_strategy_t strategy) {
    switch (strategy) {
    case HU_ADAPTIVE_KEYWORD_ONLY:
        return HU_RETRIEVAL_KEYWORD;
    case HU_ADAPTIVE_VECTOR_ONLY:
        return HU_RETRIEVAL_SEMANTIC;
    case HU_ADAPTIVE_HYBRID:
    default:
        return HU_RETRIEVAL_HYBRID;
    }
}

static void free_recall_entries(hu_allocator_t *alloc, hu_memory_entry_t *entries, size_t count) {
    if (!alloc || !entries)
        return;
    for (size_t i = 0; i < count; i++)
        hu_memory_entry_free_fields(alloc, &entries[i]);
    alloc->free(alloc->ctx, entries, count * sizeof(hu_memory_entry_t));
}

hu_error_t hu_memory_loader_init(hu_memory_loader_t *loader, hu_allocator_t *alloc,
                                 hu_memory_t *memory, hu_retrieval_engine_t *retrieval_engine,
                                 size_t max_entries, size_t max_context_chars) {
    if (!loader || !alloc)
        return HU_ERR_INVALID_ARGUMENT;
    loader->alloc = alloc;
    loader->memory = memory;
    loader->retrieval_engine = retrieval_engine;
    loader->max_entries = max_entries ? max_entries : 10;
    loader->max_context_chars = max_context_chars ? max_context_chars : 4000;
    loader->facade = NULL;
    loader->personal_model = NULL;
    loader->persona_ctx = NULL;
    return HU_OK;
}

void hu_memory_loader_set_facade(hu_memory_loader_t *loader, struct hu_w7_facade *facade) {
    if (!loader)
        return;
    loader->facade = facade;
}

void hu_memory_loader_set_personal_model(hu_memory_loader_t *loader, struct hu_personal_model *pm) {
    if (!loader)
        return;
    loader->personal_model = pm;
}

void hu_memory_loader_set_persona_context(hu_memory_loader_t *loader,
                                          const struct hu_persona_context *ctx) {
    if (!loader)
        return;
    loader->persona_ctx = ctx;
}

hu_error_t hu_memory_loader_load(hu_memory_loader_t *loader, const char *query, size_t query_len,
                                 const char *session_id, size_t session_id_len, char **out_context,
                                 size_t *out_context_len) {
    if (!loader || !out_context)
        return HU_ERR_INVALID_ARGUMENT;
    *out_context = NULL;
    if (out_context_len)
        *out_context_len = 0;

    hu_memory_entry_t *entries = NULL;
    size_t count = 0;
    hu_error_t err;

    if (loader->retrieval_engine && loader->retrieval_engine->ctx &&
        loader->retrieval_engine->vtable) {
        hu_adaptive_config_t acfg = {
            .enabled = true, .keyword_max_tokens = 3, .vector_min_tokens = 5};
        hu_query_analysis_t qa = hu_adaptive_analyze_query(query ? query : "", query_len, &acfg);

#ifdef HU_ENABLE_SQLITE
        /* Strategy learner: override with learned preference if available.
         * When the learner recommends HU_RSTRAT_GRAPH, skip the retrieval
         * engine and route directly to the W12 planner. */
        bool use_planner_path = false;
        if (loader->memory && loader->memory->ctx) {
            sqlite3 *sl_db = hu_sqlite_memory_get_db(loader->memory);
            if (sl_db) {
                hu_strategy_learner_t sl;
                if (hu_strategy_learner_create(loader->alloc, sl_db, &sl) == HU_OK) {
                    hu_query_category_t qcat =
                        hu_strategy_classify_query(query ? query : "", query_len);
                    hu_retrieval_strategy_t learned = hu_strategy_learner_recommend(&sl, qcat);
                    switch (learned) {
                    case HU_RSTRAT_KEYWORD:
                        qa.recommended_strategy = HU_ADAPTIVE_KEYWORD_ONLY;
                        break;
                    case HU_RSTRAT_VECTOR:
                        qa.recommended_strategy = HU_ADAPTIVE_VECTOR_ONLY;
                        break;
                    case HU_RSTRAT_GRAPH:
                        if (loader->facade)
                            use_planner_path = true;
                        break;
                    default:
                        break;
                    }
                    hu_strategy_learner_deinit(&sl);
                }
            }
        }
#endif

        if (
#ifdef HU_ENABLE_SQLITE
            use_planner_path &&
#endif
            loader->facade) {
            char *planner_text = NULL;
            size_t planner_len = 0;
            hu_error_t pe = hu_w12_planner_recall(
                loader->facade, loader->alloc, session_id ? session_id : "", session_id_len,
                query ? query : "", query_len, loader->max_entries, loader->max_context_chars,
                &planner_text, &planner_len);
#ifdef HU_ENABLE_SQLITE
            if (loader->memory && loader->memory->ctx) {
                sqlite3 *sl_db = hu_sqlite_memory_get_db(loader->memory);
                if (sl_db) {
                    hu_strategy_learner_t sl;
                    if (hu_strategy_learner_create(loader->alloc, sl_db, &sl) == HU_OK) {
                        hu_strategy_learner_init_tables(&sl);
                        hu_query_category_t qcat =
                            hu_strategy_classify_query(query ? query : "", query_len);
                        hu_strategy_learner_record(&sl, qcat, HU_RSTRAT_GRAPH,
                                                   pe == HU_OK && planner_len > 0,
                                                   (int64_t)time(NULL));
                        hu_strategy_learner_deinit(&sl);
                    }
                }
            }
#endif
            if (pe == HU_OK && planner_text && planner_len > 0) {
                *out_context = planner_text;
                if (out_context_len)
                    *out_context_len = planner_len;
                return HU_OK;
            }
        }

        hu_retrieval_options_t opts = {
            .mode = adaptive_to_retrieval_mode(qa.recommended_strategy),
            .limit = loader->max_entries,
            .min_score = 0.0,
            .use_reranking = false,
            .temporal_decay_factor = 0.0,
        };
        hu_retrieval_result_t res = {0};
        err =
            loader->retrieval_engine->vtable->retrieve(loader->retrieval_engine->ctx, loader->alloc,
                                                       query ? query : "", query_len, &opts, &res);
        if (err == HU_ERR_JSON_PARSE || err == HU_ERR_PROVIDER_RATE_LIMITED ||
            err == HU_ERR_PROVIDER_AUTH || err == HU_ERR_IO) {
            hu_log_warn("memory_loader", NULL,
                        "retrieval engine error (%s); falling back to v1 recall",
                        hu_error_string(err));
            memset(&res, 0, sizeof(res));
            err = HU_OK;
        } else if (err != HU_OK) {
            return err;
        }
        if (res.count > 0) {
            entries = res.entries;
            count = res.count;
            if (res.scores)
                loader->alloc->free(loader->alloc->ctx, res.scores, count * sizeof(double));
            res.entries = NULL;
            res.count = 0;
            res.scores = NULL;

#ifdef HU_ENABLE_SQLITE
            if (loader->memory && loader->memory->ctx && count > 0) {
                sqlite3 *sl_db = hu_sqlite_memory_get_db(loader->memory);
                if (sl_db) {
                    hu_strategy_learner_t sl;
                    if (hu_strategy_learner_create(loader->alloc, sl_db, &sl) == HU_OK) {
                        hu_strategy_learner_init_tables(&sl);
                        hu_query_category_t qcat =
                            hu_strategy_classify_query(query ? query : "", query_len);
                        hu_retrieval_strategy_t used_strat;
                        switch (qa.recommended_strategy) {
                        case HU_ADAPTIVE_KEYWORD_ONLY:
                            used_strat = HU_RSTRAT_KEYWORD;
                            break;
                        case HU_ADAPTIVE_VECTOR_ONLY:
                            used_strat = HU_RSTRAT_VECTOR;
                            break;
                        default:
                            used_strat = HU_RSTRAT_HYBRID;
                            break;
                        }
                        hu_strategy_learner_record(&sl, qcat, used_strat, count > 0,
                                                   (int64_t)time(NULL));
                        hu_strategy_learner_deinit(&sl);
                    }
                }
            }
#endif
        }
        /* W12: when retrieval yields nothing, try the goal-conditioned
         * planner before falling back to v1 recall. The planner's multi-hop
         * PageRank traversal provides better entity-affinity scoring. */
        if (count == 0 && loader->facade) {
            char *planner_text = NULL;
            size_t planner_len = 0;
            hu_error_t pe = hu_w12_planner_recall(
                loader->facade, loader->alloc, session_id ? session_id : "", session_id_len,
                query ? query : "", query_len, loader->max_entries, loader->max_context_chars,
                &planner_text, &planner_len);
            if (pe == HU_OK && planner_text && planner_len > 0) {
                *out_context = planner_text;
                if (out_context_len)
                    *out_context_len = planner_len;
                return HU_OK;
            }
        }
        if (count == 0 && loader->memory && loader->memory->vtable &&
            loader->memory->vtable->recall) {
            err = loader->memory->vtable->recall(loader->memory->ctx, loader->alloc,
                                                 query ? query : "", query_len, loader->max_entries,
                                                 session_id ? session_id : "", session_id_len,
                                                 &entries, &count);
            if (err != HU_OK)
                return err;
        }
    } else if (loader->facade) {
        /* W12: route through the goal-conditioned planner when available. */
        char *planner_text = NULL;
        size_t planner_len = 0;
        hu_error_t pe = hu_w12_planner_recall(
            loader->facade, loader->alloc, session_id ? session_id : "", session_id_len,
            query ? query : "", query_len, loader->max_entries, loader->max_context_chars,
            &planner_text, &planner_len);
        if (pe == HU_OK && planner_text && planner_len > 0) {
            *out_context = planner_text;
            if (out_context_len)
                *out_context_len = planner_len;
            return HU_OK;
        }
        /* Planner failed or empty — fall through to v1 recall. */
        if (loader->memory && loader->memory->vtable && loader->memory->vtable->recall) {
            err = loader->memory->vtable->recall(loader->memory->ctx, loader->alloc,
                                                 query ? query : "", query_len, loader->max_entries,
                                                 session_id ? session_id : "", session_id_len,
                                                 &entries, &count);
            if (err != HU_OK)
                return err;
        } else {
            return HU_OK;
        }
    } else if (loader->memory && loader->memory->vtable && loader->memory->vtable->recall) {
        err = loader->memory->vtable->recall(
            loader->memory->ctx, loader->alloc, query ? query : "", query_len, loader->max_entries,
            session_id ? session_id : "", session_id_len, &entries, &count);
        if (err != HU_OK)
            return err;
    } else {
        return HU_OK;
    }
    if (!entries || count == 0)
        goto supplement;

    hu_json_buf_t buf;
    err = hu_json_buf_init(&buf, loader->alloc);
    if (err != HU_OK) {
        free_recall_entries(loader->alloc, entries, count);
        return err;
    }

    size_t total_len = 0;
    for (size_t i = 0; i < count && total_len < loader->max_context_chars; i++) {
        const hu_memory_entry_t *e = &entries[i];

        /* SOTA-2026 init-09 sec 2.9: trust gate.
         *
         * UNTRUSTED entries are silently suppressed from recall context.
         * THIRD_PARTY entries are suppressed when a same-key FIRST_PARTY+
         * entry exists in the same batch (the higher-trust statement
         * shadows the lower-trust one). Otherwise the entry surfaces but
         * the frontier model is expected to weight it less based on the
         * source field (the [Unverified hints] convention from sec 2.9
         * is enforced for hu_personal_model facts in the prompt builder,
         * not here in the recall list). */
        if (e->trust_tier == (int)HU_TRUST_UNTRUSTED)
            continue;
        if (e->trust_tier <= (int)HU_TRUST_THIRD_PARTY && e->key && e->key_len > 0) {
            bool shadowed = false;
            for (size_t k = 0; k < count; k++) {
                if (k == i)
                    continue;
                const hu_memory_entry_t *o = &entries[k];
                if (o->trust_tier >= (int)HU_TRUST_FIRST_PARTY && o->key &&
                    o->key_len == e->key_len && memcmp(o->key, e->key, e->key_len) == 0) {
                    shadowed = true;
                    break;
                }
            }
            if (shadowed)
                continue;
        }

        const char *key = e->key ? e->key : "unknown";
        size_t key_len = e->key_len ? e->key_len : strlen(key);
        const char *content = e->content ? e->content : "";
        size_t content_len = e->content_len;
        const char *timestamp = e->timestamp ? e->timestamp : "";
        size_t timestamp_len = e->timestamp_len ? e->timestamp_len : strlen(timestamp);

        /* Format: ### Memory: {key}\n{content}\n(stored: {timestamp})\n\n */
        size_t overhead = 26 + key_len + timestamp_len;
        size_t block_len = overhead + content_len;
        if (total_len + block_len > loader->max_context_chars) {
            size_t remain = loader->max_context_chars - total_len;
            if (remain <= overhead)
                break;
            content_len = remain - overhead;
            block_len = remain;
        }

        err = hu_json_buf_append_raw(&buf, "### Memory: ", 12);
        if (err != HU_OK)
            goto cleanup;
        err = hu_json_buf_append_raw(&buf, key, key_len);
        if (err != HU_OK)
            goto cleanup;
        err = hu_json_buf_append_raw(&buf, "\n", 1);
        if (err != HU_OK)
            goto cleanup;

        if (content_len > 0) {
            err = hu_json_buf_append_raw(&buf, content, content_len);
            if (err != HU_OK)
                goto cleanup;
        }
        err = hu_json_buf_append_raw(&buf, "\n(stored: ", 10);
        if (err != HU_OK)
            goto cleanup;
        err = hu_json_buf_append_raw(&buf, timestamp, timestamp_len);
        if (err != HU_OK)
            goto cleanup;
        err = hu_json_buf_append_raw(&buf, ")\n\n", 3);
        if (err != HU_OK)
            goto cleanup;

        total_len += block_len;
    }

    if (buf.len > 0) {
        *out_context = hu_strndup(loader->alloc, buf.ptr, buf.len);
        if (!*out_context) {
            err = HU_ERR_OUT_OF_MEMORY;
            goto cleanup;
        }
        /* Length MUST match the returned buffer, not buf.len. hu_strndup
         * truncates its copy at the first embedded NUL (memchr in the first
         * n bytes), so when recalled content contains a NUL byte the
         * allocation is SHORTER than buf.len. Reporting buf.len made every
         * consumer (the data-quality UTF-8 validator, then the prompt
         * assembler) read past the allocation — a heap-buffer-overflow that
         * crashlooped the daemon 2026-07-13. strlen == the strndup'd length. */
        if (out_context_len)
            *out_context_len = strlen(*out_context);
    }

cleanup:
    hu_json_buf_free(&buf);
    free_recall_entries(loader->alloc, entries, count);

supplement:
    /* Supplementary graph context: always inject the world model summary for
     * this contact so entity/relation knowledge is available on every turn,
     * not just when the strategy learner selects HU_RSTRAT_GRAPH. Planner
     * paths return early above and already use the graph internally. */
    if (err == HU_OK && loader->facade && session_id && session_id_len > 0) {
        char *graph_text = NULL;
        size_t graph_len = 0;
        hu_error_t ge = hu_w7_render_world_model(
            loader->facade, loader->alloc, session_id, session_id_len, 0, &graph_text, &graph_len,
            NULL, 0, NULL, 0, NULL, 0, (hu_personal_model_t *)loader->personal_model,
            loader->persona_ctx);
        if (ge == HU_OK && graph_text && graph_len > 0) {
            const size_t alloc_len = graph_len; /* free contract: original len + 1 */
            const size_t graph_cap = 500;
            if (graph_len > graph_cap)
                graph_len = graph_cap;
            append_section(loader, out_context, out_context_len, graph_text, graph_len);
            loader->alloc->free(loader->alloc->ctx, graph_text, alloc_len + 1);
        }
    }

    /* Insight stream (better-than-human item 3, HU_INSIGHT_STREAM). Appended
     * LAST inside the memory section on purpose: the value-aware trim cuts
     * the memory span head-first, so these survive longest; and the model
     * reads them closest to the guard tail. */
    if (err == HU_OK && session_id && session_id_len > 0)
        append_contact_insights(loader, session_id, session_id_len, out_context, out_context_len);
    return err;
}
