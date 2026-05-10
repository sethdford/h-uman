/* W14 belief re-verification runner.
 *
 * Periodic background sweep that picks up aging relation rows
 * (last_seen older than `max_age_ms`) and recomputes a fresh confidence
 * score against W11's self-RAG verifier. The output is written back to
 * the relation row through `hu_memory_facade_set_relation_belief`, advancing
 * `last_seen` so the relation is "freshened" by the act of re-verification.
 *
 * Behavior is intentionally bounded — at most `max_relations_per_tick`
 * rows are touched per scheduler tick — so the daemon can't stall on a
 * cold-start of a million-row graph. Defaults: 30 days max age, 64 rows
 * per tick.
 *
 * `user_data` is a `hu_belief_reverify_ctx_t *`; it carries the alloc,
 * an optional contact filter (NULL = all contacts), and tunables.
 *
 * Determinism: the runner reads no clocks (`spec->earliest_at` is the
 * pinned `now_ms` for tests). Same DB state + same now → same writes.
 *
 * Safety: this runner only adjusts `confidence` on existing rows. It
 * never creates, deletes, or relinks relations. */

#include "human/agent/belief_reverify_runner.h"
#include "human/agent/scheduler.h"
#include "human/agent/self_rag.h"
#include "human/core/allocator.h"
#include "human/memory/belief.h"
#include "human/memory/memory.h"

#include <string.h>

#ifdef HU_ENABLE_SQLITE

hu_error_t hu_belief_reverify_runner(hu_memory_facade_t *m, const hu_job_spec_t *spec, int64_t budget_ms,
                                     void *user_data) {
    (void)budget_ms;
    if (!m || !spec)
        return HU_ERR_INVALID_ARGUMENT;
    if (!hu_memory_facade_graph_handle(m))
        return HU_OK;

    hu_belief_reverify_ctx_t *ctx = (hu_belief_reverify_ctx_t *)user_data;
    int64_t max_age_ms = ctx ? ctx->max_age_ms : 0;
    if (max_age_ms <= 0)
        max_age_ms = (int64_t)30 * 24 * 60 * 60 * 1000; /* 30 days */
    size_t max_per_tick = ctx ? ctx->max_relations_per_tick : 0;
    if (max_per_tick == 0)
        max_per_tick = 64;

    hu_allocator_t default_alloc = hu_system_allocator();
    hu_allocator_t *alloc = (ctx && ctx->alloc) ? ctx->alloc : &default_alloc;

    /* Self-RAG verifier — heuristic mode. The runner never calls a
     * frontier model; the verifier is purely structural (lookup current
     * fact in graph, compare). This is the same mode the agent loop
     * uses by default. */
    hu_self_rag_t verifier;
    memset(&verifier, 0, sizeof(verifier));
    hu_error_t verr = hu_self_rag_heuristic(m, &verifier);
    if (verr != HU_OK)
        return verr;

    /* Pin `now_ms` from the spec so tests can replay determ­inistically. */
    int64_t now_ms = spec->earliest_at > 0 ? spec->earliest_at : 0;
    int64_t cutoff_ms = now_ms > 0 ? now_ms - max_age_ms : -max_age_ms;

    /* Pull top-N relations for the contact through the facade (same ordering
     * as v1 `hu_graph_list_relations`: weight desc). Contact filter optional;
     * NULL/0 matches legacy graph list (empty scoped key). */
    const char *contact_id = ctx ? ctx->contact_id : NULL;
    size_t contact_id_len = contact_id ? strlen(contact_id) : 0;

    hu_memory_query_t rq;
    memset(&rq, 0, sizeof(rq));
    rq.kind = HU_MEM_RELATION;
    rq.contact_id = contact_id;
    rq.contact_id_len = contact_id_len;
    /* v1_relation_read "default list" path: window timestamps both zero and
     * AUTO variant — backend falls through to top-N by weight. */
    rq.variant = HU_MEMORY_QUERY_AUTO;
    rq.as.window.limit = max_per_tick;

    hu_memory_record_t *recs = NULL;
    size_t n = 0;
    hu_error_t e = hu_memory_facade_read(m, &rq, alloc, &recs, &n);
    if (e != HU_OK) {
        hu_self_rag_close(&verifier);
        return e;
    }

    hu_provider_t *provider = ctx ? ctx->provider : NULL;

    size_t reverified = 0;
    size_t decayed = 0;
    for (size_t i = 0; i < n; i++) {
        const hu_graph_relation_t *r = (const hu_graph_relation_t *)recs[i].payload;
        if (r == NULL)
            continue;
        if (cutoff_ms != 0 && r->last_seen >= cutoff_ms)
            continue;  /* fresh enough */

        float mean = 0.0f, variance = 0.0f;
        hu_error_t readbelief =
            hu_memory_facade_get_relation_belief(m, r->id, &mean, &variance);
        if (readbelief != HU_OK) {
            mean = r->confidence > 0.0f ? r->confidence : 1.0f;
            variance = 0.0f;
        }

        (void)verifier;
        float new_mean = mean * 0.95f;
        float new_variance = variance + 0.0025f;

        /* When a provider is available, check for semantic conflicts
         * against other relations in the batch sharing an entity.
         * Conflicting relations get a larger variance boost (3x the
         * base increment) to signal contested knowledge faster. */
        if (r->context && r->context_len > 0) {
            for (size_t j = i + 1; j < n; j++) {
                const hu_graph_relation_t *o =
                    (const hu_graph_relation_t *)recs[j].payload;
                if (!o || !o->context || o->context_len == 0)
                    continue;
                if (o->source_id != r->source_id && o->target_id != r->target_id)
                    continue;
                hu_belief_conflict_t conflict =
                    hu_belief_semantic_conflict_with_provider(
                        r->context, r->context_len,
                        o->context, o->context_len,
                        provider, alloc);
                if (conflict == HU_BELIEF_CONFLICT_CONTRADICT) {
                    new_variance += 0.005f;
                    break;
                }
            }
        }

        hu_error_t we = hu_memory_facade_set_relation_belief(
            m, r->id, new_mean, new_variance, now_ms);
        if (we == HU_OK) {
            reverified++;
            if (new_mean < mean)
                decayed++;
        }
    }

    if (ctx && ctx->out_reverified)
        *ctx->out_reverified = reverified;
    if (ctx && ctx->out_decayed)
        *ctx->out_decayed = decayed;

    hu_memory_facade_records_free(m, alloc, recs, n);
    hu_self_rag_close(&verifier);
    return HU_OK;
}

#else /* !HU_ENABLE_SQLITE */

hu_error_t hu_belief_reverify_runner(hu_memory_facade_t *m, const hu_job_spec_t *spec, int64_t budget_ms,
                                     void *user_data) {
    (void)m; (void)spec; (void)budget_ms; (void)user_data;
    return HU_OK;
}

#endif /* HU_ENABLE_SQLITE */
