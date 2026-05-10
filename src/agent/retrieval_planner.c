/*
 * W12 — Retrieval planner core: vtable dispatch, executor, heuristic backend.
 *
 * The heuristic backend is intentionally tiny. It scans the goal text for a
 * small set of verbs and synthesizes 1-3 step plans. No tokenizer, no
 * keyword index, no mutable state — keep it deterministic so tests don't
 * flake on input variations.
 */
#include "human/agent/retrieval_planner.h"

#include "human/core/error.h"
#include "human/memory/memory.h"
#include "human/memory/pagerank.h"

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Allocator shorthands ───────────────────────────────────────────────── */

static inline void *xalloc(hu_allocator_t *a, size_t n) {
    return a->alloc(a->ctx, n);
}

static inline void xfree(hu_allocator_t *a, void *p, size_t n) {
    if (p) a->free(a->ctx, p, n);
}

/* ── Lifecycle / dispatch ───────────────────────────────────────────────── */

void hu_planner_close(hu_planner_t *p) {
    if (!p || !p->vt) return;
    if (p->vt->deinit) p->vt->deinit(p->ctx);
    p->vt = NULL;
    p->ctx = NULL;
}

/* Clamp `out` to the hard caps. Idempotent. */
static void clamp_plan(hu_retrieval_plan_t *out) {
    if (out->steps_count > HU_PLANNER_MAX_STEPS)
        out->steps_count = HU_PLANNER_MAX_STEPS;
    if (out->total_budget_ms > HU_PLANNER_MAX_TOTAL_BUDGET_MS)
        out->total_budget_ms = HU_PLANNER_MAX_TOTAL_BUDGET_MS;
    if (out->total_budget_ms < 0)
        out->total_budget_ms = 0;
    for (size_t i = 0; i < out->steps_count; i++) {
        hu_retrieval_step_t *s = &out->steps[i];
        if (s->hops > 3) s->hops = 3;
        if (s->budget_ms < 0) s->budget_ms = 0;
        if (s->budget_ms > HU_PLANNER_MAX_TOTAL_BUDGET_MS)
            s->budget_ms = HU_PLANNER_MAX_TOTAL_BUDGET_MS;
    }
}

hu_error_t hu_planner_plan(hu_planner_t *p, const char *goal, size_t goal_len,
                           const hu_world_model_t *wm, hu_retrieval_plan_t *out_plan) {
    if (!p || !p->vt || !p->vt->plan || !out_plan)
        return HU_ERR_INVALID_ARGUMENT;
    /* Adversarial input ceiling: a malicious goal cannot be unbounded. */
    if (goal && goal_len > HU_PLANNER_MAX_GOAL_LEN)
        goal_len = HU_PLANNER_MAX_GOAL_LEN;
    memset(out_plan, 0, sizeof(*out_plan));
    hu_error_t err = p->vt->plan(p->ctx, goal, goal_len, wm, out_plan);
    if (err != HU_OK) return err;
    clamp_plan(out_plan);
    return HU_OK;
}

/* ── Heuristic backend ──────────────────────────────────────────────────── */

/* Lower-case substring match within the first `n` bytes of `hay` (no allocs).
 * Both inputs are arbitrary bytes; we only ASCII-fold the haystack. */
static bool contains_word(const char *hay, size_t hay_len, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0 || hay_len < nlen) return false;
    for (size_t i = 0; i + nlen <= hay_len; i++) {
        size_t j = 0;
        for (; j < nlen; j++) {
            unsigned char hc = (unsigned char)hay[i + j];
            unsigned char nc = (unsigned char)needle[j];
            if (tolower(hc) != tolower(nc)) break;
        }
        if (j == nlen) {
            /* Crude word-boundary check: edges or non-alpha neighbours. */
            int left_ok  = (i == 0) || !isalpha((unsigned char)hay[i - 1]);
            int right_ok = (i + nlen == hay_len) || !isalpha((unsigned char)hay[i + nlen]);
            if (left_ok && right_ok) return true;
        }
    }
    return false;
}

/* Pick a primary anchor entity from the world model (top entity by mention
 * count — already pre-sorted in W9). Returns 0 if the world model is empty. */
static int64_t primary_anchor(const hu_world_model_t *wm) {
    if (!wm || wm->entities_count == 0) return 0;
    return wm->entities[0].id;
}

/* Pick a secondary anchor (second-most mentioned), 0 if absent. */
static int64_t secondary_anchor(const hu_world_model_t *wm) {
    if (!wm || wm->entities_count < 2) return 0;
    return wm->entities[1].id;
}

static void set_contact(hu_memory_query_t *q, const hu_world_model_t *wm) {
    if (!wm) {
        q->contact_id = NULL;
        q->contact_id_len = 0;
        return;
    }
    q->contact_id = wm->contact_id;
    /* contact_id is a fixed-size buffer; compute length once. */
    size_t maxlen = sizeof(wm->contact_id);
    size_t cidlen = 0;
    while (cidlen < maxlen && wm->contact_id[cidlen]) cidlen++;
    q->contact_id_len = cidlen;
}

static hu_retrieval_step_t step_relations_window(const hu_world_model_t *wm,
                                                 int64_t from_ts, int64_t to_ts,
                                                 size_t limit, bool verify) {
    hu_retrieval_step_t s;
    memset(&s, 0, sizeof(s));
    s.kind = HU_MEM_RELATION;
    s.query.kind = HU_MEM_RELATION;
    s.query.variant = HU_MEMORY_QUERY_WINDOW;
    set_contact(&s.query, wm);
    s.query.as.window.from_ts = from_ts;
    s.query.as.window.to_ts   = to_ts;
    s.query.as.window.limit   = limit;
    s.hops = 1;
    s.budget_ms = 100;
    s.verify_after = verify;
    return s;
}

static hu_retrieval_step_t step_neighbors(const hu_world_model_t *wm,
                                          int64_t anchor, size_t hops,
                                          size_t limit, bool verify) {
    hu_retrieval_step_t s;
    memset(&s, 0, sizeof(s));
    s.kind = HU_MEM_ENTITY;
    s.query.kind = HU_MEM_ENTITY;
    /* P4 — explicit variant tag eliminates the union-aliasing AUTO heuristic
     * in v1_entity_read. Without this, `neighbors.entity_id` (an int64) would
     * alias with `by_name.name` (a pointer) and the v1 backend would attempt
     * to dereference a small integer as a string pointer. See
     * `tests/test_w7_memory_facade.c::test_w7_p3_neighbors_query_with_variant_tag_safe`. */
    s.query.variant = HU_MEMORY_QUERY_NEIGHBORS;
    set_contact(&s.query, wm);
    s.query.as.neighbors.entity_id = anchor;
    s.query.as.neighbors.hops      = hops;
    s.query.as.neighbors.limit     = limit;
    s.hops = hops;
    s.budget_ms = 150;
    s.verify_after = verify;
    return s;
}

/* Default minimal plan: list a few relations in an open window. Always valid. */
static void default_plan(const hu_world_model_t *wm, hu_retrieval_plan_t *out) {
    out->steps[0] = step_relations_window(wm, 0, INT64_MAX, 16, false);
    out->steps_count = 1;
    out->total_budget_ms = 200;
}

static hu_error_t heuristic_plan(void *ctx, const char *goal, size_t goal_len,
                                 const hu_world_model_t *wm,
                                 hu_retrieval_plan_t *out) {
    (void)ctx;
    if (!out) return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    /* Empty world model is fine — we still emit the default plan; the
     * executor will simply return zero records. */
    if (!goal || goal_len == 0) {
        default_plan(wm, out);
        return HU_OK;
    }

    bool has_when    = contains_word(goal, goal_len, "when")
                    || contains_word(goal, goal_len, "last");
    bool has_where   = contains_word(goal, goal_len, "where");
    bool has_who     = contains_word(goal, goal_len, "who");
    bool has_between = contains_word(goal, goal_len, "between");
    bool has_with    = contains_word(goal, goal_len, "with");

    int64_t a = primary_anchor(wm);
    int64_t b = secondary_anchor(wm);

    /* Relationship query (multi-hop): "between X and Y" or "with X". A 3-hop
     * shape: anchor entity -> 1-hop neighbours -> 1-hop intersect -> time-
     * filtered relations. We emit the structural shape; the executor walks.
     * Requires at least one anchor entity; without one, neighbour expansion
     * is meaningless and the backend rejects entity_id=0. Fall through to
     * the temporal/default branches if no anchor is available. */
    if ((has_between || has_with) && a != 0) {
        size_t i = 0;
        out->steps[i++] = step_neighbors(wm, a, 1, 16, true);
        if (b != 0)
            out->steps[i++] = step_neighbors(wm, b, 1, 16, true);
        out->steps[i++] = step_relations_window(wm, 0, INT64_MAX, 16, true);
        out->steps_count = i;
        out->total_budget_ms = 350;
        return HU_OK;
    }

    /* Temporal query: "when did ...", "last time ...". One step: windowed
     * relations sorted by recency. Verify because "when" answers are high-
     * stakes for hallucination. */
    if (has_when) {
        out->steps[0] = step_relations_window(wm, 0, INT64_MAX, 16, true);
        out->steps_count = 1;
        out->total_budget_ms = 150;
        return HU_OK;
    }

    /* "where" or "who": entity-shaped lookup. If we have an anchor, expand
     * its neighbours; otherwise fall back to default. */
    if ((has_where || has_who) && a != 0) {
        out->steps[0] = step_neighbors(wm, a, 1, 16, true);
        out->steps_count = 1;
        out->total_budget_ms = 200;
        return HU_OK;
    }

    /* Catch-all: list relations. Cheap and never wrong. */
    default_plan(wm, out);
    return HU_OK;
}

static hu_planner_vtable_t s_heuristic_vt = {
    .name = "heuristic",
    .plan = heuristic_plan,
    .deinit = NULL,
};

hu_error_t hu_planner_heuristic(hu_planner_t *out) {
    if (!out) return HU_ERR_INVALID_ARGUMENT;
    out->vt = &s_heuristic_vt;
    out->ctx = NULL;
    return HU_OK;
}

/* ── Executor ───────────────────────────────────────────────────────────── */

static int64_t mono_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

/* Compact aggregator: array of summary records keyed by (kind, id). */
typedef struct agg {
    hu_memory_record_t *items;
    size_t count;
    size_t cap;
    hu_allocator_t *alloc;
} agg_t;

static hu_error_t agg_push(agg_t *a, const hu_memory_record_t *src) {
    /* Dedupe scan — O(n) but n is bounded by step result caps. */
    for (size_t i = 0; i < a->count; i++) {
        if (a->items[i].kind == src->kind && a->items[i].id == src->id)
            return HU_OK;
    }
    if (a->count == a->cap) {
        size_t new_cap = a->cap == 0 ? 16 : a->cap * 2;
        size_t old_bytes = a->cap * sizeof(*a->items);
        size_t new_bytes = new_cap * sizeof(*a->items);
        void *p = a->alloc->realloc(a->alloc->ctx, a->items, old_bytes, new_bytes);
        if (!p) return HU_ERR_OUT_OF_MEMORY;
        a->items = p;
        a->cap = new_cap;
    }
    /* Strip non-portable owned fields — the planner output is metadata-only. */
    hu_memory_record_t r = *src;
    r.payload = NULL;
    r.payload_len = 0;
    r.provenance = NULL;
    r.provenance_len = 0;
    a->items[a->count++] = r;
    return HU_OK;
}

void hu_planner_records_free(hu_allocator_t *alloc, hu_memory_record_t *records,
                             size_t count) {
    if (!alloc || !records) return;
    /* Records are opaque summaries — no nested owned allocations. Free the
     * top-level block. The size hint matches the realloc trail's last
     * `new_bytes = cap * sizeof`, but the system allocator ignores size and
     * the tracking allocator only cares that frees match an alloc. We pass
     * `count * sizeof` as a best-effort hint; tests use the system
     * allocator which discards the hint. */
    xfree(alloc, records, count * sizeof(*records));
}

/* Per-step W11 verifier filter (P0 #2).
 *
 * When a plan step has `verify_after` and the caller supplied a
 * non-NULL `self_rag` handle, we filter records by W8 confidence
 * before they propagate to the next hop or aggregate. The threshold
 * mirrors W11's `abstain_threshold` default (0.3). Records with
 * `confidence == 0` (no W8 belief recorded yet — legacy writes) are
 * kept under "innocent until proven guilty" semantics: dropping them
 * silently would invalidate every memory written before W8 landed.
 *
 * Memory contract: we cannot rearrange records[] in place because the
 * payload pointers are heap-allocated by the backend and freed by
 * `hu_memory_facade_records_free` walking ALL n slots. Aliasing two slots'
 * payloads (struct copies) leads to double-free in records_free.
 * Instead, we return a parallel keep_mask[] so the caller iterates
 * selectively. records_free runs over the unmodified array.
 *
 * Returns count kept; sets *out_abstain_ratio = (n - kept) / scored,
 * where `scored` excludes confidence==0 (no W8 belief) records. */
static size_t verifier_filter_records(const hu_memory_record_t *records, size_t n,
                                      float kept_threshold, bool *keep_mask,
                                      float *out_abstain_ratio) {
    if (n == 0) {
        if (out_abstain_ratio) *out_abstain_ratio = 0.0f;
        return 0;
    }
    size_t kept = 0;
    size_t scored = 0;
    for (size_t i = 0; i < n; i++) {
        const hu_memory_record_t *r = &records[i];
        bool drop = false;
        if (r->confidence > 0.0f) {
            scored++;
            if (r->confidence < kept_threshold) drop = true;
        }
        keep_mask[i] = !drop;
        if (!drop) kept++;
    }
    size_t denom = scored > 0 ? scored : n;
    if (out_abstain_ratio) *out_abstain_ratio = (float)(n - kept) / (float)denom;
    return kept;
}

hu_error_t hu_planner_execute(hu_memory_facade_t *m, hu_self_rag_t *self_rag,
                              const hu_retrieval_plan_t *plan, hu_allocator_t *alloc,
                              hu_memory_record_t **out, size_t *out_count) {
    if (!m || !plan || !alloc || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    if (plan->steps_count > HU_PLANNER_MAX_STEPS)
        return HU_ERR_INVALID_ARGUMENT;
    if (plan->total_budget_ms < 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (plan->total_budget_ms > HU_PLANNER_MAX_TOTAL_BUDGET_MS)
        return HU_ERR_INVALID_ARGUMENT;

    /* W11 verifier wiring (P0 #2):
     *
     * When `self_rag` is non-NULL AND `step->verify_after` is true, we
     * filter the per-step records by W8 confidence (the W11 belief
     * layer's primary signal). Records below `kept_threshold` (0.3 —
     * mirrors W11's `abstain_threshold`) are dropped before
     * propagation. If the abstain ratio crosses 0.5, the rest of the
     * plan is aborted: continuing would compound unsupported evidence
     * and is the exact failure mode the W11 inline-per-step loop
     * exists to prevent.
     *
     * `self_rag == NULL` keeps the v1 behaviour (no filtering) so
     * existing call sites that haven't migrated to the verifier loop
     * yet are unaffected. The post-response verifier in
     * `world_model_bridge.c::hu_w11_self_rag_verify` continues to run
     * on the final draft regardless. */
    const float kept_threshold = 0.3f;
    const float abort_ratio = 0.5f;

    *out = NULL;
    *out_count = 0;

    agg_t a;
    memset(&a, 0, sizeof(a));
    a.alloc = alloc;

    int64_t start_ms = mono_ms();
    hu_error_t final_err = HU_OK;

    for (size_t i = 0; i < plan->steps_count; i++) {
        /* Budget enforcement: 0 = unlimited, anything >0 stops on overrun. */
        if (plan->total_budget_ms > 0) {
            int64_t elapsed = mono_ms() - start_ms;
            if (elapsed >= plan->total_budget_ms) break;
        }

        const hu_retrieval_step_t *step = &plan->steps[i];
        hu_memory_record_t *recs = NULL;
        size_t n = 0;
        hu_error_t err = hu_memory_facade_read(m, &step->query, alloc, &recs, &n);
        if (err == HU_ERR_NOT_FOUND) {
            /* Empty result — keep walking; not a failure. */
            continue;
        }
        if (err == HU_ERR_NOT_SUPPORTED) {
            /* Backend missing for this kind — record and skip the step. */
            continue;
        }
        if (err != HU_OK) {
            /* First hard error wins; we still return what we accumulated so
             * the caller can use partial results, but report the error. */
            final_err = err;
            break;
        }

        bool abort_plan = false;
        bool *keep_mask = NULL;
        size_t kept = n;
        if (self_rag && step->verify_after && n > 0) {
            keep_mask = (bool *)alloc->alloc(alloc->ctx, n * sizeof(*keep_mask));
            if (!keep_mask) {
                hu_memory_facade_records_free(m, alloc, recs, n);
                hu_planner_records_free(alloc, a.items, a.count);
                return HU_ERR_OUT_OF_MEMORY;
            }
            float abstain_ratio = 0.0f;
            kept = verifier_filter_records(recs, n, kept_threshold, keep_mask, &abstain_ratio);
            if (abstain_ratio >= abort_ratio) {
                abort_plan = true;
            }
        }

        for (size_t j = 0; j < n; j++) {
            if (keep_mask && !keep_mask[j]) continue;
            hu_error_t e = agg_push(&a, &recs[j]);
            if (e != HU_OK) {
                if (keep_mask)
                    alloc->free(alloc->ctx, keep_mask, n * sizeof(*keep_mask));
                hu_memory_facade_records_free(m, alloc, recs, n);
                hu_planner_records_free(alloc, a.items, a.count);
                return e;
            }
        }
        (void)kept;
        if (keep_mask)
            alloc->free(alloc->ctx, keep_mask, n * sizeof(*keep_mask));
        hu_memory_facade_records_free(m, alloc, recs, n);

        if (abort_plan) break;
    }

    *out = a.items;
    *out_count = a.count;
    return final_err;
}

/* ── Goal-conditioned (PageRank) backend ──────────────────────────────── */

typedef struct gc_ctx {
    hu_memory_facade_t     *m;
    hu_allocator_t  *alloc;
} gc_ctx_t;

/* Top-K entities to expand neighbors for after PageRank scoring. Beyond 4
 * the budget pressure dominates and extra steps rarely help. */
#define GC_TOP_K 4

static hu_error_t gc_plan(void *raw_ctx, const char *goal, size_t goal_len,
                          const hu_world_model_t *wm, hu_retrieval_plan_t *out) {
    gc_ctx_t *ctx = (gc_ctx_t *)raw_ctx;
    if (!out) return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    /* No world model or no entities: delegate to heuristic plan. */
    if (!wm || wm->entities_count == 0 || !ctx || !ctx->m) {
        return heuristic_plan(NULL, goal, goal_len, wm, out);
    }

    /* Extract seed entity IDs from the world model. */
    size_t seeds_count = wm->entities_count;
    int64_t *seeds = xalloc(ctx->alloc, seeds_count * sizeof(*seeds));
    if (!seeds) return HU_ERR_OUT_OF_MEMORY;
    for (size_t i = 0; i < seeds_count; i++)
        seeds[i] = wm->entities[i].id;

    /* Compute contact_id length. */
    size_t cid_len = 0;
    size_t cid_max = sizeof(wm->contact_id);
    while (cid_len < cid_max && wm->contact_id[cid_len]) cid_len++;

    /* Run personalized PageRank. */
    int64_t *pr_ids = NULL;
    float   *pr_scores = NULL;
    size_t   pr_count = 0;
    hu_error_t err = hu_memory_pagerank_seeds(
        ctx->m, ctx->alloc, wm->contact_id, cid_len,
        seeds, seeds_count,
        0.0f, 0, /* defaults */
        &pr_ids, &pr_scores, &pr_count);

    xfree(ctx->alloc, seeds, seeds_count * sizeof(*seeds));

    if (err != HU_OK || pr_count == 0) {
        xfree(ctx->alloc, pr_ids, pr_count * sizeof(*pr_ids));
        xfree(ctx->alloc, pr_scores, pr_count * sizeof(*pr_scores));
        return heuristic_plan(NULL, goal, goal_len, wm, out);
    }

    /* Build plan steps: expand neighbors for the top-K ranked entities.
     * PageRank output is already sorted by score descending. */
    size_t top_k = pr_count < GC_TOP_K ? pr_count : GC_TOP_K;
    size_t avail = HU_PLANNER_MAX_STEPS - 1; /* reserve last slot for relations */
    if (top_k > avail) top_k = avail;

    size_t step_idx = 0;
    for (size_t i = 0; i < top_k; i++) {
        out->steps[step_idx] = step_neighbors(wm, pr_ids[i], 1, 16, true);
        step_idx++;
    }

    /* Final step: time-windowed relations for completeness. */
    out->steps[step_idx] = step_relations_window(wm, 0, INT64_MAX, 16, true);
    step_idx++;

    out->steps_count = step_idx;
    out->total_budget_ms = (int)(step_idx * 120);
    if (out->total_budget_ms > HU_PLANNER_MAX_TOTAL_BUDGET_MS)
        out->total_budget_ms = HU_PLANNER_MAX_TOTAL_BUDGET_MS;

    xfree(ctx->alloc, pr_ids, pr_count * sizeof(*pr_ids));
    xfree(ctx->alloc, pr_scores, pr_count * sizeof(*pr_scores));
    return HU_OK;
}

static void gc_deinit(void *raw_ctx) {
    gc_ctx_t *ctx = (gc_ctx_t *)raw_ctx;
    if (ctx) ctx->alloc->free(ctx->alloc->ctx, ctx, sizeof(*ctx));
}

static hu_planner_vtable_t s_gc_vt = {
    .name   = "goal_conditioned",
    .plan   = gc_plan,
    .deinit = gc_deinit,
};

hu_error_t hu_planner_goal_conditioned(hu_memory_facade_t *m, hu_allocator_t *alloc,
                                       hu_planner_t *out) {
    if (!m || !alloc || !out) return HU_ERR_INVALID_ARGUMENT;
    gc_ctx_t *ctx = xalloc(alloc, sizeof(*ctx));
    if (!ctx) return HU_ERR_OUT_OF_MEMORY;
    ctx->m = m;
    ctx->alloc = alloc;
    out->vt = &s_gc_vt;
    out->ctx = ctx;
    return HU_OK;
}

/* ── Multi-hop traversal ──────────────────────────────────────────────── */

hu_error_t hu_planner_multi_hop(hu_memory_facade_t *m, hu_allocator_t *alloc,
                                const hu_memory_query_t *initial_query,
                                size_t max_hops,
                                hu_memory_record_t **out, size_t *out_count) {
    if (!m || !alloc || !initial_query || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;

    *out = NULL;
    *out_count = 0;

    if (max_hops == 0) max_hops = HU_PLANNER_MULTI_HOP_DEFAULT;
    if (max_hops > HU_PLANNER_MULTI_HOP_MAX) max_hops = HU_PLANNER_MULTI_HOP_MAX;

    agg_t a;
    memset(&a, 0, sizeof(a));
    a.alloc = alloc;

    /* Hop 0: execute the initial query directly. */
    hu_memory_record_t *recs = NULL;
    size_t n = 0;
    hu_error_t err = hu_memory_facade_read(m, initial_query, alloc, &recs, &n);
    if (err != HU_OK && err != HU_ERR_NOT_FOUND && err != HU_ERR_NOT_SUPPORTED) {
        return err;
    }

    for (size_t i = 0; i < n; i++) {
        hu_error_t e = agg_push(&a, &recs[i]);
        if (e != HU_OK) {
            hu_memory_facade_records_free(m, alloc, recs, n);
            hu_planner_records_free(alloc, a.items, a.count);
            return e;
        }
    }
    if (recs) hu_memory_facade_records_free(m, alloc, recs, n);

    /* Iterative hops: extract entity IDs from current aggregate, run
     * PageRank, expand neighbors for top-scored entities. */
    for (size_t hop = 1; hop <= max_hops; hop++) {
        if (a.count == 0) break;

        /* Collect entity IDs from the aggregate as PageRank seeds. Only
         * entity-kind records contribute meaningful seed IDs. */
        size_t seed_cap = a.count;
        int64_t *seeds = xalloc(alloc, seed_cap * sizeof(*seeds));
        if (!seeds) {
            hu_planner_records_free(alloc, a.items, a.count);
            return HU_ERR_OUT_OF_MEMORY;
        }
        size_t seeds_count = 0;
        for (size_t i = 0; i < a.count; i++) {
            if (a.items[i].kind == HU_MEM_ENTITY && a.items[i].id != 0) {
                /* Deduplicate seeds. */
                bool dup = false;
                for (size_t j = 0; j < seeds_count; j++) {
                    if (seeds[j] == a.items[i].id) { dup = true; break; }
                }
                if (!dup) seeds[seeds_count++] = a.items[i].id;
            }
        }

        if (seeds_count == 0) {
            xfree(alloc, seeds, seed_cap * sizeof(*seeds));
            break;
        }

        /* Run PageRank to prioritize which entities to expand. */
        int64_t *pr_ids = NULL;
        float   *pr_scores = NULL;
        size_t   pr_count = 0;
        hu_error_t pr_err = hu_memory_pagerank_seeds(
            m, alloc, initial_query->contact_id, initial_query->contact_id_len,
            seeds, seeds_count, 0.0f, 0, &pr_ids, &pr_scores, &pr_count);

        xfree(alloc, seeds, seed_cap * sizeof(*seeds));

        if (pr_err != HU_OK || pr_count == 0) {
            xfree(alloc, pr_ids, pr_count * sizeof(*pr_ids));
            xfree(alloc, pr_scores, pr_count * sizeof(*pr_scores));
            break;
        }

        /* Expand neighbors for top-K PageRank-scored entities. */
        size_t expand_k = pr_count < GC_TOP_K ? pr_count : GC_TOP_K;
        for (size_t k = 0; k < expand_k; k++) {
            hu_memory_query_t nq;
            memset(&nq, 0, sizeof(nq));
            nq.kind = HU_MEM_ENTITY;
            nq.variant = HU_MEMORY_QUERY_NEIGHBORS; /* P4: prevent union-aliasing AUTO heuristic */
            nq.contact_id = initial_query->contact_id;
            nq.contact_id_len = initial_query->contact_id_len;
            nq.as.neighbors.entity_id = pr_ids[k];
            nq.as.neighbors.hops = 1;
            nq.as.neighbors.limit = 16;

            hu_memory_record_t *hop_recs = NULL;
            size_t hop_n = 0;
            hu_error_t he = hu_memory_facade_read(m, &nq, alloc, &hop_recs, &hop_n);
            if (he == HU_OK) {
                for (size_t j = 0; j < hop_n; j++) {
                    hu_error_t pe = agg_push(&a, &hop_recs[j]);
                    if (pe != HU_OK) {
                        hu_memory_facade_records_free(m, alloc, hop_recs, hop_n);
                        xfree(alloc, pr_ids, pr_count * sizeof(*pr_ids));
                        xfree(alloc, pr_scores, pr_count * sizeof(*pr_scores));
                        hu_planner_records_free(alloc, a.items, a.count);
                        return pe;
                    }
                }
                hu_memory_facade_records_free(m, alloc, hop_recs, hop_n);
            }
        }

        xfree(alloc, pr_ids, pr_count * sizeof(*pr_ids));
        xfree(alloc, pr_scores, pr_count * sizeof(*pr_scores));
    }

    *out = a.items;
    *out_count = a.count;
    return HU_OK;
}
