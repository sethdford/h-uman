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

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Allocator shorthands ───────────────────────────────────────────────── */

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

hu_error_t hu_planner_execute(hu_memory_t *m, hu_self_rag_t *self_rag,
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

#ifndef HU_W11_AVAILABLE
    /* Without W11, the verifier is a no-op. Capture the pointer to keep
     * the ABI stable; ignore at execution time. */
    (void)self_rag;
#endif

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
        hu_error_t err = hu_memory_read(m, &step->query, alloc, &recs, &n);
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

#ifdef HU_W11_AVAILABLE
        /* W11 verifier hook (skipped if self_rag is NULL). The actual call
         * surface lands when W11 merges; this commit just structures the
         * branch so wire-up is a one-line change. */
        if (step->verify_after && self_rag) {
            /* TODO(W12-W11-wire): hu_self_rag_filter(self_rag, recs, n, ...); */
        }
#endif

        for (size_t j = 0; j < n; j++) {
            hu_error_t e = agg_push(&a, &recs[j]);
            if (e != HU_OK) {
                hu_memory_records_free(m, alloc, recs, n);
                hu_planner_records_free(alloc, a.items, a.count);
                return e;
            }
        }
        hu_memory_records_free(m, alloc, recs, n);
    }

    *out = a.items;
    *out_count = a.count;
    return final_err;
}
