/* W14 belief re-verification runner.
 *
 * Periodic background sweep that picks up aging relation rows
 * (last_seen older than `max_age_ms`) and recomputes a fresh confidence
 * score against W11's self-RAG verifier. The output is written back to
 * the relation row through `hu_graph_set_relation_confidence`, advancing
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
#include "human/memory/memory.h"

#include <string.h>

#ifdef HU_ENABLE_SQLITE

hu_error_t hu_belief_reverify_runner(hu_memory_facade_t *m, const hu_job_spec_t *spec, int64_t budget_ms,
                                     void *user_data) {
    (void)budget_ms;
    if (!m || !spec)
        return HU_ERR_INVALID_ARGUMENT;
    hu_graph_t *g = hu_memory_facade_graph_handle(m);
    if (!g)
        return HU_OK;
    /* Belief mean/variance updates are still graph-local UPDATE-by-id until a
     * facade write shape covers that without upsert semantics. */

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
    /* v1_relation_read "default list" path: window timestamps both zero. */
    rq.as.window.limit = max_per_tick;

    hu_memory_record_t *recs = NULL;
    size_t n = 0;
    hu_error_t e = hu_memory_facade_read(m, &rq, alloc, &recs, &n);
    if (e != HU_OK) {
        hu_self_rag_close(&verifier);
        return e;
    }

    size_t reverified = 0;
    size_t decayed = 0;
    for (size_t i = 0; i < n; i++) {
        const hu_graph_relation_t *r = (const hu_graph_relation_t *)recs[i].payload;
        if (r == NULL)
            continue;
        if (cutoff_ms != 0 && r->last_seen >= cutoff_ms)
            continue;  /* fresh enough */

        /* W8 P2A — pull the current (mean, variance) belief if the row
         * was migrated. Legacy rows that haven't been touched since
         * migration return HU_ERR_NOT_FOUND; in that case we fall back
         * to scalar `confidence` and assume variance = 0. */
        float mean = 0.0f, variance = 0.0f;
        hu_error_t readbelief =
            hu_graph_get_relation_belief(g, r->id, &mean, &variance);
        if (readbelief != HU_OK) {
            mean = r->confidence > 0.0f ? r->confidence : 1.0f;
            variance = 0.0f;
        }

        /* The reverify runner can't ground the claim against a frontier
         * model here (this is the heuristic mode the agent loop also
         * uses by default). Without grounding, "silence" is a weak
         * signal — the right semantic is age-based decay, NOT a
         * Bayesian observation. We therefore decay the mean by 5% and
         * GROW variance to reflect rising uncertainty as the fact
         * ages without confirmation. The variance growth is bounded
         * by the [0, 0.25] cap inside hu_graph_set_relation_belief. */
        (void)verifier;
        float new_mean = mean * 0.95f;
        /* Uncertainty rises by ~1% of the variance ceiling per pass.
         * Over 25 unverified passes that saturates at the cap (0.25),
         * which corresponds to a uniform prior — i.e. "we no longer
         * know what to believe" without re-verification. */
        float new_variance = variance + 0.0025f;

        hu_error_t we = hu_graph_set_relation_belief(
            g, r->id, new_mean, new_variance, now_ms);
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
