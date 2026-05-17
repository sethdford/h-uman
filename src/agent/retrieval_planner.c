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

/* Case-insensitive substring match without word-boundary enforcement.
 * Used for the "compact entity name in a multi-word query" case: an entity
 * called "PineNuts" should still match the user typing "pine nuts" because
 * the alphanumeric tokens overlap completely. We strip non-alnum from both
 * sides before comparing, so "PineNuts" ⊆ "pinenuts" ⊆ "pine nuts" with
 * spaces folded out. */
static bool contains_compact(const char *hay, size_t hay_len, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0 || hay_len == 0) return false;
    /* Build a compacted lowercase copy of the haystack: alphanumeric only.
     * Caps at 256 bytes — anything longer is goal text we don't care about. */
    char compact[256];
    size_t ci = 0;
    for (size_t i = 0; i < hay_len && ci + 1 < sizeof(compact); i++) {
        unsigned char c = (unsigned char)hay[i];
        if (isalnum(c)) compact[ci++] = (char)tolower(c);
    }
    compact[ci] = '\0';
    /* And the needle. */
    char ncompact[128];
    size_t nci = 0;
    for (size_t i = 0; i < nlen && nci + 1 < sizeof(ncompact); i++) {
        unsigned char c = (unsigned char)needle[i];
        if (isalnum(c)) ncompact[nci++] = (char)tolower(c);
    }
    ncompact[nci] = '\0';
    if (nci == 0 || ci < nci) return false;
    for (size_t i = 0; i + nci <= ci; i++) {
        if (memcmp(compact + i, ncompact, nci) == 0) return true;
    }
    return false;
}

/* Scan `goal` for any entity name in the world model. Returns the first
 * matched entity's id, or 0 if none. Walks `wm->entities` in W9 mention
 * order so the top-mentioned matching entity wins on ties.
 *
 * Match strategy (two passes, broadest first):
 *   1. `contains_word` — strict whole-word case-insensitive. Catches
 *      "alice" inside "where does alice work?" without crossing word
 *      boundaries (so "lisa" doesn't match "alice").
 *   2. `contains_compact` — alphanumeric-only fold on both sides. Catches
 *      compound entity names like "PineNuts" when the user types
 *      "pine nuts", or "FundingRound" against "funding round".
 *
 * The W16 facade-recall benchmark surfaced both modes (Alice→whole-word,
 * PineNuts→compact). */
static int64_t query_anchor(const char *goal, size_t goal_len,
                            const hu_world_model_t *wm) {
    if (!goal || goal_len == 0 || !wm || wm->entities_count == 0) return 0;
    for (size_t i = 0; i < wm->entities_count; i++) {
        const hu_graph_entity_t *e = &wm->entities[i];
        if (!e->name || e->name_len == 0) continue;
        if (contains_word(goal, goal_len, e->name)) return e->id;
    }
    /* Second pass: compact match for multi-word user queries against
     * compound entity names (e.g. "pine nuts" ↔ "PineNuts"). */
    for (size_t i = 0; i < wm->entities_count; i++) {
        const hu_graph_entity_t *e = &wm->entities[i];
        if (!e->name || e->name_len == 0) continue;
        /* Require the entity to be reasonably compound (>=6 chars) before
         * accepting a compact match; otherwise short entity names like
         * "Vim" would match every query containing 'v' 'i' 'm' adjacently
         * (e.g. "vivid memory"). */
        if (e->name_len >= 6 && contains_compact(goal, goal_len, e->name))
            return e->id;
    }
    return 0;
}

static int goal_overlap_score(const char *goal, size_t goal_len,
                              const char *text, size_t text_len);
static hu_retrieval_step_t step_neighbors(const hu_world_model_t *wm,
                                          int64_t anchor, size_t hops,
                                          size_t limit, bool verify);

/* Entity id for `eid` in the snapshot, or NULL. */
static const hu_graph_entity_t *entity_by_id(const hu_world_model_t *wm,
                                             int64_t eid) {
    if (!wm || eid <= 0) return NULL;
    for (size_t i = 0; i < wm->entities_count; i++) {
        if (wm->entities[i].id == eid) return &wm->entities[i];
    }
    return NULL;
}

/* True when `name` appears in goal (whole-word or compact for long names). */
static bool entity_name_in_goal(const char *goal, size_t goal_len,
                                const char *name, size_t name_len) {
    if (!goal || goal_len == 0 || !name || name_len == 0) return false;
    if (contains_word(goal, goal_len, name)) return true;
    return name_len >= 6 && contains_compact(goal, goal_len, name);
}

/* P5.2 — when the goal overlaps `self_model.focused_topics`, prefer the
 * snapshot entity whose name best matches that overlap (W12 cell wiring). */
static int64_t anchor_from_self_model_focus(const char *goal, size_t goal_len,
                                            const hu_world_model_t *wm) {
    if (!goal || goal_len == 0 || !wm || wm->self_model.focused_topics[0] == '\0')
        return 0;

    const char *topics = wm->self_model.focused_topics;
    size_t best_id = 0;
    size_t best_hits = 0;

    for (size_t ei = 0; ei < wm->entities_count; ei++) {
        const hu_graph_entity_t *e = &wm->entities[ei];
        if (!e->name || e->name_len == 0) continue;

        size_t topic_hits = 0;
        const char *p = topics;
        while (*p) {
            while (*p == ';' || *p == ' ') p++;
            if (!*p) break;
            const char *start = p;
            while (*p && *p != ';') p++;
            size_t tlen = (size_t)(p - start);
            while (tlen > 0 && start[tlen - 1] == ' ') tlen--;
            if (tlen >= 3) {
                if (goal_overlap_score(goal, goal_len, start, tlen) > 0 &&
                    entity_name_in_goal(start, tlen, e->name, e->name_len))
                    topic_hits++;
            }
            if (*p == ';') p++;
        }
        if (topic_hits > best_hits) {
            best_hits = topic_hits;
            best_id = (size_t)e->id;
        }
    }
    return best_hits > 0 ? (int64_t)best_id : 0;
}

/* Goal asks for recency / change history (W12 recent_changes cell). */
static bool goal_has_temporal_cue(const char *goal, size_t goal_len) {
    return contains_word(goal, goal_len, "when")
        || contains_word(goal, goal_len, "last")
        || contains_word(goal, goal_len, "recent");
}

/* Derive relation-window bounds from wm->recent_changes when temporal. */
static void relation_window_bounds(const hu_world_model_t *wm, bool temporal,
                                   int64_t *out_from, int64_t *out_to) {
    *out_from = 0;
    *out_to = INT64_MAX;
    if (!temporal || !wm || wm->recent_changes_count == 0) return;

    int64_t oldest = wm->recent_changes[0].at_ms;
    for (size_t i = 1; i < wm->recent_changes_count; i++) {
        int64_t at = wm->recent_changes[i].at_ms;
        if (at < oldest) oldest = at;
    }
    /* Pad before the earliest bitemporal stamp so event_start rows that
     * predate the retraction/supersede still fall inside the window. */
    const int64_t pad_ms = 7LL * 24 * 60 * 60 * 1000;
    *out_from = oldest > pad_ms ? oldest - pad_ms : 0;
}

/* Count hyperedge members whose entity names appear in the goal. */
static size_t hyperedge_named_members(const hu_hyperedge_t *he,
                                      const char *goal, size_t goal_len,
                                      const hu_world_model_t *wm) {
    if (!he || !wm || he->members_count == 0) return 0;
    size_t named = 0;
    for (size_t m = 0; m < he->members_count; m++) {
        const hu_graph_entity_t *e =
            entity_by_id(wm, he->members[m].entity_id);
        if (!e || !e->name) continue;
        if (entity_name_in_goal(goal, goal_len, e->name, e->name_len)) named++;
    }
    return named;
}

/* Append neighbour-expansion steps for hyperedge members (deduped). */
static size_t append_hyperedge_neighbor_steps(hu_retrieval_plan_t *out,
                                            size_t step_idx,
                                            const hu_world_model_t *wm,
                                            const char *goal, size_t goal_len,
                                            size_t neighbor_limit, bool verify) {
    if (!wm || wm->hyperedges_count == 0 || step_idx >= HU_PLANNER_MAX_STEPS)
        return step_idx;

    for (size_t h = 0; h < wm->hyperedges_count && step_idx < HU_PLANNER_MAX_STEPS;
         h++) {
        const hu_hyperedge_t *he = &wm->hyperedges[h];
        if (hyperedge_named_members(he, goal, goal_len, wm) < 2) continue;

        for (size_t m = 0; m < he->members_count && step_idx < HU_PLANNER_MAX_STEPS;
             m++) {
            int64_t eid = he->members[m].entity_id;
            if (eid <= 0) continue;

            bool dup = false;
            for (size_t s = 0; s < step_idx; s++) {
                if (out->steps[s].kind == HU_MEM_ENTITY &&
                    out->steps[s].query.as.neighbors.entity_id == eid) {
                    dup = true;
                    break;
                }
            }
            if (dup) continue;

            out->steps[step_idx++] =
                step_neighbors(wm, eid, 1, neighbor_limit, verify);
        }
    }
    return step_idx;
}

/* Collect PageRank seed entity ids: snapshot entities + hyperedge members
 * that match the goal + focused-topic entities (W12 P4.2 / P5.2 wiring). */
static int64_t *collect_pagerank_seeds(hu_allocator_t *alloc,
                                       const hu_world_model_t *wm,
                                       const char *goal, size_t goal_len,
                                       size_t *out_count) {
    *out_count = 0;
    if (!alloc || !wm || wm->entities_count == 0) return NULL;

    size_t cap = wm->entities_count + 16;
    int64_t *seeds = xalloc(alloc, cap * sizeof(*seeds));
    if (!seeds) return NULL;

    for (size_t i = 0; i < wm->entities_count; i++)
        seeds[(*out_count)++] = wm->entities[i].id;

    if (wm->hyperedges_count > 0 && goal && goal_len > 0) {
        for (size_t h = 0; h < wm->hyperedges_count; h++) {
            const hu_hyperedge_t *he = &wm->hyperedges[h];
            if (hyperedge_named_members(he, goal, goal_len, wm) < 2) continue;
            for (size_t m = 0; m < he->members_count; m++) {
                int64_t eid = he->members[m].entity_id;
                if (eid <= 0) continue;
                bool dup = false;
                for (size_t k = 0; k < *out_count; k++) {
                    if (seeds[k] == eid) { dup = true; break; }
                }
                if (!dup && *out_count < cap) seeds[(*out_count)++] = eid;
            }
        }
    }

    int64_t focused = anchor_from_self_model_focus(goal, goal_len, wm);
    if (focused > 0) {
        bool dup = false;
        for (size_t k = 0; k < *out_count; k++) {
            if (seeds[k] == focused) { dup = true; break; }
        }
        if (!dup && *out_count < cap) seeds[(*out_count)++] = focused;
    }

    return seeds;
}

/* Pick a primary anchor entity from the world model. Priority order:
 *   1. Self-model focused topic overlap with the goal (P5.2).
 *   2. Any world-model entity whose name appears in the goal text.
 *   3. The top-mentioned entity (W9-sorted) as the fallback.
 * Returns 0 if the world model is empty.
 *
 * `goal` may be NULL; that just disables steps 1-2. */
static int64_t primary_anchor_with_goal(const char *goal, size_t goal_len,
                                        const hu_world_model_t *wm) {
    if (!wm || wm->entities_count == 0) return 0;
    int64_t focused = anchor_from_self_model_focus(goal, goal_len, wm);
    if (focused != 0) return focused;
    int64_t named = query_anchor(goal, goal_len, wm);
    if (named != 0) return named;
    return wm->entities[0].id;
}

/* Pick a secondary anchor. Same priority as primary: query-named entity
 * other than `primary_id`, else mention-count fallback. Both strict-word
 * and compact passes mirror `query_anchor` so a query that names two
 * compound entities ("between PineNuts and Genmaicha") still resolves
 * both anchors. */
static int64_t secondary_anchor_with_goal(const char *goal, size_t goal_len,
                                          const hu_world_model_t *wm,
                                          int64_t primary_id) {
    if (!wm || wm->entities_count < 2) return 0;
    if (goal && goal_len > 0) {
        for (size_t i = 0; i < wm->entities_count; i++) {
            const hu_graph_entity_t *e = &wm->entities[i];
            if (e->id == primary_id) continue;
            if (!e->name || e->name_len == 0) continue;
            if (contains_word(goal, goal_len, e->name)) return e->id;
        }
        for (size_t i = 0; i < wm->entities_count; i++) {
            const hu_graph_entity_t *e = &wm->entities[i];
            if (e->id == primary_id) continue;
            if (!e->name || e->name_len == 0) continue;
            if (e->name_len >= 6 && contains_compact(goal, goal_len, e->name))
                return e->id;
        }
    }
    /* Mention-count fallback: first entity that isn't the primary. */
    for (size_t i = 0; i < wm->entities_count; i++) {
        if (wm->entities[i].id != primary_id) return wm->entities[i].id;
    }
    return 0;
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

/* Copy `goal` into `plan->goal` (lowercased, ASCII-truncated). Used by
 * `hu_planner_execute` to re-rank entity records by relation-context
 * overlap against the user query. Idempotent: safe to call from any
 * planner backend. */
static void plan_capture_goal(hu_retrieval_plan_t *plan, const char *goal,
                              size_t goal_len) {
    if (!plan || !goal || goal_len == 0) return;
    size_t copy = goal_len < HU_PLANNER_GOAL_BUF_MAX - 1
                      ? goal_len : HU_PLANNER_GOAL_BUF_MAX - 1;
    for (size_t i = 0; i < copy; i++) {
        unsigned char c = (unsigned char)goal[i];
        plan->goal[i] = (char)tolower(c);
    }
    plan->goal[copy] = '\0';
    plan->goal_len = copy;
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

    plan_capture_goal(out, goal, goal_len);

    bool has_temporal = goal_has_temporal_cue(goal, goal_len);
    int64_t win_from = 0;
    int64_t win_to = INT64_MAX;
    relation_window_bounds(wm, has_temporal, &win_from, &win_to);

    size_t neighbor_limit = 16;
    if (wm && wm->self_model.capabilities_count > 0) {
        for (size_t ci = 0; ci < wm->self_model.capabilities_count; ci++) {
            if (contains_word(goal, goal_len, wm->self_model.capabilities[ci])) {
                neighbor_limit = 64;
                break;
            }
        }
    }

    bool has_where   = contains_word(goal, goal_len, "where");
    bool has_who     = contains_word(goal, goal_len, "who");
    bool has_between = contains_word(goal, goal_len, "between");
    bool has_with    = contains_word(goal, goal_len, "with");

    /* Anchor selection: prefer entities the user named in the goal over
     * world-model mention ordering. See `primary_anchor_with_goal` doc for
     * the rationale; this is the W12 P5 retrieval-quality fix and lifts
     * facade-recall precision_at_1 substantially. */
    int64_t a = primary_anchor_with_goal(goal, goal_len, wm);
    int64_t b = secondary_anchor_with_goal(goal, goal_len, wm, a);
    /* Whether `a` was selected because the goal mentioned its name (vs. the
     * world-model mention-count fallback). When true we know the user has
     * given us a strong signal about which entity to expand; we should not
     * fall through to a generic window query for them. */
    bool a_is_named = (a != 0 && query_anchor(goal, goal_len, wm) == a);
    /* Both anchors named: the user mentioned two specific entities. The
     * relationship-query branch's multi-anchor plan is the right shape
     * even without "between"/"with" prepositions ("when did alice and bob
     * collaborate?" should expand BOTH anchors). */
    bool b_is_named = false;
    if (a_is_named && b != 0) {
        for (size_t i = 0; i < wm->entities_count; i++) {
            const hu_graph_entity_t *e = &wm->entities[i];
            if (e->id != b || !e->name || e->name_len == 0) continue;
            if (contains_word(goal, goal_len, e->name) ||
                (e->name_len >= 6 && contains_compact(goal, goal_len, e->name)))
                b_is_named = true;
            break;
        }
    }

    /* Relationship query (multi-hop): "between X and Y", "with X", or any
     * goal that names two world-model entities. A 3-hop shape: anchor
     * entity -> 1-hop neighbours -> 1-hop intersect -> time-filtered
     * relations. Requires at least one anchor entity; without one,
     * neighbour expansion is meaningless and the backend rejects
     * entity_id=0. Fall through to the temporal/default branches if no
     * anchor is available. */
    if ((has_between || has_with || b_is_named) && a != 0) {
        size_t i = 0;
        out->steps[i++] = step_neighbors(wm, a, 1, neighbor_limit, true);
        if (b != 0)
            out->steps[i++] = step_neighbors(wm, b, 1, neighbor_limit, true);
        i = append_hyperedge_neighbor_steps(out, i, wm, goal, goal_len,
                                            neighbor_limit, true);
        out->steps[i++] =
            step_relations_window(wm, win_from, win_to, 16, true);
        out->steps_count = i;
        out->total_budget_ms = 350;
        return HU_OK;
    }

    /* Temporal query: "when did ...", "last time ...". One step: windowed
     * relations sorted by recency. If the goal also names an entity we
     * prepend a neighbor-expansion step so the planner can answer "when did
     * X do Y" (the answer lives on X's edges, not in the global window).
     *
     * W12 P8 — neighbor limit was 16, raised to 128 for named anchors only.
     * The 12-fact facade-recall corpus rarely hit even 16; the 1542-prompt
     * locomo-facade corpus has hub entities (Caroline, Audrey) with
     * hundreds of relations, and the *matching* one for the current query
     * was being truncated by the cap. 128 keeps the working set bounded
     * (≈ 25 KB of records aggregated per step) while giving the P6 P7
     * re-ranker enough candidates to discriminate. */
    if (has_temporal) {
        size_t i = 0;
        if (a_is_named)
            out->steps[i++] = step_neighbors(wm, a, 1, 1024, true);
        i = append_hyperedge_neighbor_steps(out, i, wm, goal, goal_len,
                                            neighbor_limit, true);
        out->steps[i++] =
            step_relations_window(wm, win_from, win_to, 16, true);
        out->steps_count = i;
        out->total_budget_ms = a_is_named ? 300 : 150;
        return HU_OK;
    }

    /* "where" / "who" / "what" / any other verb when an entity was named:
     * entity-shaped lookup. The W12 P5 fix also routes "what" queries to
     * neighbor expansion when the user named an entity ("what does alice
     * drink?" → expand Alice's neighbors), which was previously falling
     * through to the default window.
     *
     * P6 follow-up: we ALWAYS pair the neighbor expansion with a
     * relations-window step. The relations carry `context` payloads
     * ("drinks every morning", "works at since 2020") which the executor
     * re-ranks against the user goal; high-scoring relation contexts
     * promote their connected entities to the top of the result list.
     * Without the relations step, the executor only sees entity records
     * and falls back to SQL insertion order — which misses the answer
     * ("what does alice drink?" returns Acme/Bob/Genmaicha in insertion
     * order and Genmaicha lands at rank 3). */
    if (((has_where || has_who) && a != 0) || a_is_named) {
        /* W12 P8: same scale-up as the temporal branch above. Hub entities
         * with hundreds of relations need a wider neighbor window so the
         * matching context isn't truncated before the re-ranker sees it. */
        size_t i = 0;
        out->steps[i++] = step_neighbors(wm, a, 1, a_is_named ? 1024 : neighbor_limit, true);
        i = append_hyperedge_neighbor_steps(out, i, wm, goal, goal_len,
                                            neighbor_limit, true);
        out->steps[i++] =
            step_relations_window(wm, win_from, win_to, 32, true);
        out->steps_count = i;
        out->total_budget_ms = 250;
        return HU_OK;
    }

    /* Catch-all: list relations. Cheap and never wrong. */
    out->steps[0] = step_relations_window(wm, win_from, win_to, 16, false);
    out->steps_count = 1;
    out->total_budget_ms = 200;
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

/* Compact aggregator with parallel score / relation-endpoint arrays.
 *
 * `items[]`     — record summaries (kind, id only after strip).
 * `scores[]`    — goal-context overlap. 0 by default; populated by the
 *                 W12 P6 re-ranker (see `hu_planner_execute`). Higher is
 *                 more relevant to the user goal.
 * `is_anchor[]` — W12 P7. True when an ENTITY record's NAME appears in
 *                 the goal text (i.e. the entity is the *subject* of the
 *                 question, not a candidate answer). The propagation +
 *                 sort pass demotes anchors so they never occupy rank 1
 *                 on retrieval queries. Without this, "When did Caroline
 *                 X?" returns Caroline as top-1 — she's the most
 *                 name-overlapping entity and the answer entity she
 *                 connects to ties on propagated score and loses on
 *                 insertion order. False for relations.
 * `rel_src[]`/`rel_tgt[]` — for HU_MEM_RELATION rows, the connecting
 *                 entity ids captured from `payload` *before* agg_push
 *                 strips it. Used by the propagation pass to bump entity
 *                 scores by the best relation that touches them.
 *
 * All arrays are parallel and reallocated together. They are freed
 * by `hu_planner_records_free` (the score / endpoint side-tables stay
 * local to the executor; only `items` flows out). */
typedef struct agg {
    hu_memory_record_t *items;
    float    *scores;
    bool     *is_anchor;
    int64_t  *rel_src;
    int64_t  *rel_tgt;
    size_t count;
    size_t cap;
    hu_allocator_t *alloc;
} agg_t;

/* Stopword cache for the token scorer. Short common English words
 * dominate goal text without adding meaning; folding them out raises
 * the signal-to-noise ratio of overlap scoring. Sorted alphabetically
 * for the binary-search check; tweaking this list is the easiest
 * lever on retrieval quality. */
static const char *const HU_PLANNER_STOPWORDS[] = {
    "a", "all", "am", "an", "and", "any", "are", "as", "at", "be", "been",
    "but", "by", "can", "did", "do", "does", "for", "from", "got", "had",
    "has", "have", "he", "her", "him", "his", "how", "i", "if", "in", "is",
    "it", "me", "my", "no", "not", "of", "on", "or", "our", "out", "she",
    "so", "the", "their", "them", "they", "this", "to", "was", "we", "were",
    "what", "when", "where", "which", "who", "why", "will", "with", "you",
    "your"
};
static const size_t HU_PLANNER_STOPWORDS_N =
    sizeof(HU_PLANNER_STOPWORDS) / sizeof(HU_PLANNER_STOPWORDS[0]);

static bool is_stopword(const char *tok, size_t tlen) {
    for (size_t i = 0; i < HU_PLANNER_STOPWORDS_N; i++) {
        size_t slen = strlen(HU_PLANNER_STOPWORDS[i]);
        if (slen == tlen && memcmp(HU_PLANNER_STOPWORDS[i], tok, tlen) == 0)
            return true;
    }
    return false;
}

/* Count goal tokens (>=3 chars, non-stopword, lowercase) that appear
 * inside `text`. Comparison is case-insensitive substring with an
 * alnum-prefix trim — "drink" matches "drinks every morning" because
 * "drink" is a prefix of "drinks". This is poor man's stemming and is
 * sufficient for the W16 facade-recall corpus; a real implementation
 * would use a tokenizer + Porter stemmer + IDF weights. */
static int goal_overlap_score(const char *goal, size_t goal_len,
                              const char *text, size_t text_len) {
    if (!goal || goal_len == 0 || !text || text_len == 0) return 0;
    int hits = 0;
    size_t i = 0;
    while (i < goal_len) {
        while (i < goal_len && !isalnum((unsigned char)goal[i])) i++;
        size_t start = i;
        while (i < goal_len && isalnum((unsigned char)goal[i])) i++;
        size_t tlen = i - start;
        if (tlen < 3 || tlen > 64) continue;
        if (is_stopword(goal + start, tlen)) continue;
        /* Substring scan over text; alnum-folded comparison. We accept
         * a hit when the goal token is a prefix of any alnum run in
         * text (so "drink" ↔ "drinks"). */
        for (size_t j = 0; j + tlen <= text_len; j++) {
            /* Anchor on word start (j==0 or non-alnum left). */
            if (j > 0 && isalnum((unsigned char)text[j - 1])) continue;
            size_t k = 0;
            for (; k < tlen; k++) {
                unsigned char gc = (unsigned char)goal[start + k];
                unsigned char tc = (unsigned char)text[j + k];
                if (tolower(gc) != tolower(tc)) break;
            }
            if (k == tlen) { hits++; break; }
        }
    }
    return hits;
}

static hu_error_t agg_grow(agg_t *a) {
    if (a->count != a->cap) return HU_OK;
    size_t new_cap = a->cap == 0 ? 16 : a->cap * 2;
    size_t old_b_items   = a->cap * sizeof(*a->items);
    size_t new_b_items   = new_cap * sizeof(*a->items);
    size_t old_b_scores  = a->cap * sizeof(*a->scores);
    size_t new_b_scores  = new_cap * sizeof(*a->scores);
    size_t old_b_rel     = a->cap * sizeof(*a->rel_src);
    size_t new_b_rel     = new_cap * sizeof(*a->rel_src);
    size_t old_b_anchor  = a->cap * sizeof(*a->is_anchor);
    size_t new_b_anchor  = new_cap * sizeof(*a->is_anchor);
    void *p1 = a->alloc->realloc(a->alloc->ctx, a->items,   old_b_items,  new_b_items);
    if (!p1) return HU_ERR_OUT_OF_MEMORY;
    a->items = p1;
    void *p2 = a->alloc->realloc(a->alloc->ctx, a->scores,  old_b_scores, new_b_scores);
    if (!p2) return HU_ERR_OUT_OF_MEMORY;
    a->scores = p2;
    void *p3 = a->alloc->realloc(a->alloc->ctx, a->rel_src, old_b_rel,    new_b_rel);
    if (!p3) return HU_ERR_OUT_OF_MEMORY;
    a->rel_src = p3;
    void *p4 = a->alloc->realloc(a->alloc->ctx, a->rel_tgt, old_b_rel,    new_b_rel);
    if (!p4) return HU_ERR_OUT_OF_MEMORY;
    a->rel_tgt = p4;
    void *p5 = a->alloc->realloc(a->alloc->ctx, a->is_anchor, old_b_anchor, new_b_anchor);
    if (!p5) return HU_ERR_OUT_OF_MEMORY;
    a->is_anchor = p5;
    /* Zero-init the newly grown anchor flags. The realloc tail may carry
     * uninitialised bytes; the score / endpoint arrays don't need this
     * because agg_push writes them before incrementing count, but a
     * future propagation read of a slot pushed without scoring (e.g.
     * relations) must see is_anchor=false rather than garbage. */
    memset((char *)a->is_anchor + old_b_anchor, 0, new_b_anchor - old_b_anchor);
    a->cap = new_cap;
    return HU_OK;
}

static hu_error_t agg_push(agg_t *a, const hu_memory_record_t *src, float score,
                           bool is_anchor) {
    /* Dedupe scan — O(n) but n is bounded by step result caps. On a
     * duplicate we keep the MAX score so the latest, strongest signal
     * wins (mirrors a "max" combine over the plan's multiple steps).
     * Anchor flag is *sticky* — once we've decided an entity is an
     * anchor (its name appeared in the goal in any prior pass), later
     * pushes that fail the name test shouldn't clear it. */
    for (size_t i = 0; i < a->count; i++) {
        if (a->items[i].kind == src->kind && a->items[i].id == src->id) {
            if (score > a->scores[i]) a->scores[i] = score;
            if (is_anchor) a->is_anchor[i] = true;
            return HU_OK;
        }
    }
    hu_error_t err = agg_grow(a);
    if (err != HU_OK) return err;
    /* Capture relation endpoints BEFORE the payload-strip below, so the
     * propagation pass can find the entities a relation touches without
     * keeping the payload alive. Stack copy is fine — the payload's
     * lifetime ends with the caller's records_free a few lines later. */
    int64_t rs = 0, rt = 0;
    if (src->kind == HU_MEM_RELATION && src->payload != NULL) {
        const hu_memory_relation_row_t *r =
            (const hu_memory_relation_row_t *)src->payload;
        rs = r->source_id;
        rt = r->target_id;
    }
    /* Strip non-portable owned fields — the planner output is metadata-only. */
    hu_memory_record_t r = *src;
    r.payload = NULL;
    r.payload_len = 0;
    r.provenance = NULL;
    r.provenance_len = 0;
    a->items[a->count]     = r;
    a->scores[a->count]    = score;
    a->is_anchor[a->count] = is_anchor;
    a->rel_src[a->count]   = rs;
    a->rel_tgt[a->count]   = rt;
    a->count++;
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
            /* W12 P6 scoring: relations carry user-visible context that
             * predicts answer relevance much better than insertion
             * order. Score each record's payload against the plan goal
             * BEFORE agg_push strips the payload. For entity rows we
             * score the entity name (catches "Genmaicha" when goal
             * mentions it); for relations the context string.
             * Entities also get propagated relation scores in the post
             * pass — see below.
             *
             * W12 P7: also mark entities whose name appears in the
             * goal as anchors (= the user's question subject, not a
             * candidate answer). This drives the demote-anchor pass
             * after propagation. */
            float score = 0.0f;
            bool is_anchor = false;
            if (plan->goal_len > 0 && recs[j].payload != NULL) {
                if (recs[j].kind == HU_MEM_ENTITY) {
                    const hu_graph_entity_t *e =
                        (const hu_graph_entity_t *)recs[j].payload;
                    if (e->name && e->name_len > 0) {
                        int hits = goal_overlap_score(plan->goal, plan->goal_len,
                                                      e->name, e->name_len);
                        score = (float)hits;
                        /* Any token of the entity name is in the goal →
                         * the user named this entity. It is the subject
                         * of the question, not the answer. P7 demotion
                         * during the sort will move it below candidate-
                         * answer entities. */
                        if (hits > 0) is_anchor = true;
                    }
                } else if (recs[j].kind == HU_MEM_RELATION) {
                    const hu_memory_relation_row_t *r =
                        (const hu_memory_relation_row_t *)recs[j].payload;
                    if (r->context && r->context_len > 0)
                        score = (float)goal_overlap_score(
                            plan->goal, plan->goal_len, r->context, r->context_len);
                }
            }
            hu_error_t e = agg_push(&a, &recs[j], score, is_anchor);
            if (e != HU_OK) {
                if (keep_mask)
                    alloc->free(alloc->ctx, keep_mask, n * sizeof(*keep_mask));
                hu_memory_facade_records_free(m, alloc, recs, n);
                hu_planner_records_free(alloc, a.items, a.count);
                /* Side tables freed by the failure block below. */
                if (a.scores)    alloc->free(alloc->ctx, a.scores,    a.cap * sizeof(*a.scores));
                if (a.is_anchor) alloc->free(alloc->ctx, a.is_anchor, a.cap * sizeof(*a.is_anchor));
                if (a.rel_src)   alloc->free(alloc->ctx, a.rel_src,   a.cap * sizeof(*a.rel_src));
                if (a.rel_tgt)   alloc->free(alloc->ctx, a.rel_tgt,   a.cap * sizeof(*a.rel_tgt));
                return e;
            }
        }
        (void)kept;
        if (keep_mask)
            alloc->free(alloc->ctx, keep_mask, n * sizeof(*keep_mask));
        hu_memory_facade_records_free(m, alloc, recs, n);

        if (abort_plan) break;
    }

    /* W12 P6 score propagation: for each scored relation, lift the score
     * of any entity it connects (source or target) up to 0.9 * relation
     * score. The 0.9 discount preserves the relation's primacy when
     * relations and entities are present together (matches "answer is
     * about a fact", not "answer is the entity itself"), but is large
     * enough that an entity touching the best-scoring relation beats an
     * entity touching no scoring relation at all. We take MAX rather
     * than SUM to avoid summing duplicate signals — an entity in two
     * relations should rank by its single best connection, not by
     * count. */
    if (plan->goal_len > 0 && a.count > 0) {
        for (size_t i = 0; i < a.count; i++) {
            if (a.items[i].kind != HU_MEM_RELATION) continue;
            float s = a.scores[i];
            if (s <= 0.0f) continue;
            float bumped = s * 0.9f;
            int64_t rs = a.rel_src[i], rt = a.rel_tgt[i];
            for (size_t j = 0; j < a.count; j++) {
                if (a.items[j].kind != HU_MEM_ENTITY) continue;
                if (a.items[j].id != rs && a.items[j].id != rt) continue;
                if (bumped > a.scores[j]) a.scores[j] = bumped;
            }
        }

        /* W12 P7 anchor demotion (conditional). After propagation, we
         * may sort by a tiered key: non-anchor entities AND relations
         * sort above anchor entities. The condition: at least one
         * non-anchor entity must have a positive propagated score.
         *
         * Why this matters. On "When did Caroline X?" Caroline is an
         * anchor (her name is in the goal, so her direct overlap is
         * high). The answer entity (target of the matching relation)
         * only gets propagated 0.9 * relation_score. Without P7,
         * Caroline wins on direct overlap AND on propagation from
         * dozens of her relations. The locomo-facade benchmark
         * exposed this at scale.
         *
         * Why conditional. Facade-recall has queries whose intended
         * answer entity is itself named in the query (`"when did
         * alice and bob collaborate?"` expects `"Bob"`). Those edge
         * cases score 1.0 today and the regression floor is 0.95.
         * Unconditional demotion would push them to 0.75. The right
         * heuristic: only demote anchors when there is a non-anchor
         * candidate that received propagation. If the only entities
         * with score are anchors, the user's question subject IS the
         * intended response (it's the only candidate the relation
         * propagation lifts above 0), so we let it stay on top.
         *
         * Relations stay in the "non-anchor" tier so a high-scoring
         * matching relation still surfaces near the top.
         *
         * Stable selection-sort, bounded by HU_PLANNER_MAX_STEPS *
         * step caps (≲ 256 in worst case), so O(n²) is fine. */
        bool demote_anchors = false;
        for (size_t i = 0; i < a.count; i++) {
            if (a.items[i].kind == HU_MEM_ENTITY && !a.is_anchor[i] &&
                a.scores[i] > 0.0f) {
                demote_anchors = true;
                break;
            }
        }

        for (size_t i = 0; i + 1 < a.count; i++) {
            size_t best = i;
            bool best_anchor = a.is_anchor[i];
            float best_score = a.scores[i];
            for (size_t j = i + 1; j < a.count; j++) {
                bool j_anchor = a.is_anchor[j];
                float j_score = a.scores[j];
                /* Non-anchor strictly beats anchor — only when the
                 * condition above said this query has a real answer
                 * candidate. Otherwise fall through to score-only. */
                if (demote_anchors) {
                    if (best_anchor && !j_anchor) {
                        best = j; best_anchor = j_anchor; best_score = j_score;
                        continue;
                    }
                    if (!best_anchor && j_anchor) continue;
                }
                /* Same tier — break by score desc. */
                if (j_score > best_score) {
                    best = j; best_anchor = j_anchor; best_score = j_score;
                }
            }
            if (best != i) {
                /* Swap items[i] ↔ items[best] across every parallel array. */
                hu_memory_record_t tr = a.items[i];
                a.items[i] = a.items[best]; a.items[best] = tr;
                float ts = a.scores[i];
                a.scores[i] = a.scores[best]; a.scores[best] = ts;
                bool ta = a.is_anchor[i];
                a.is_anchor[i] = a.is_anchor[best]; a.is_anchor[best] = ta;
                int64_t trs = a.rel_src[i], trt = a.rel_tgt[i];
                a.rel_src[i] = a.rel_src[best]; a.rel_tgt[i] = a.rel_tgt[best];
                a.rel_src[best] = trs; a.rel_tgt[best] = trt;
            }
        }
    }

    /* Free side tables (only `items` flows out). */
    if (a.scores)    alloc->free(alloc->ctx, a.scores,    a.cap * sizeof(*a.scores));
    if (a.is_anchor) alloc->free(alloc->ctx, a.is_anchor, a.cap * sizeof(*a.is_anchor));
    if (a.rel_src)   alloc->free(alloc->ctx, a.rel_src,   a.cap * sizeof(*a.rel_src));
    if (a.rel_tgt)   alloc->free(alloc->ctx, a.rel_tgt,   a.cap * sizeof(*a.rel_tgt));

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
    plan_capture_goal(out, goal, goal_len);

    /* No world model or no entities: delegate to heuristic plan. */
    if (!wm || wm->entities_count == 0 || !ctx || !ctx->m) {
        return heuristic_plan(NULL, goal, goal_len, wm, out);
    }

  /* PageRank seeds: entities + hyperedge members + focused-topic anchors. */
    size_t seeds_count = 0;
    int64_t *seeds =
        collect_pagerank_seeds(ctx->alloc, wm, goal, goal_len, &seeds_count);
    if (!seeds) return HU_ERR_OUT_OF_MEMORY;

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
        /* Hop-0: no goal overlap here; use unit weight so dedupe/max still
         * works. is_anchor=false — multi-hop traversal doesn't carry the
         * goal-name overlap check that anchor seeding requires. */
        hu_error_t e = agg_push(&a, &recs[i], 1.0f, false);
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
                float seed_score = (pr_scores && k < pr_count) ? pr_scores[k] : 1.0f;
                for (size_t j = 0; j < hop_n; j++) {
                    /* Hop-N expansion: anchor flag does not propagate to
                     * neighbors of an anchor entity. The flag is sticky on
                     * the original entity (set by goal-name match), but
                     * neighbor records are themselves not anchors. */
                    hu_error_t pe = agg_push(&a, &hop_recs[j], seed_score, false);
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

    /* The aggregator owns FIVE parallel arrays (see agg_t at line 458).
     * We surface `a.items` to the caller — `hu_planner_records_free`
     * will free that one — but the four sibling arrays (`scores`,
     * `is_anchor`, `rel_src`, `rel_tgt`) are working-state only and
     * must be released here. ASan leak summary on PR55 flagged 128 b
     * leaks at agg_grow's rel_src/rel_tgt reallocs because the function
     * exited without releasing them. */
    if (a.scores)    xfree(alloc, a.scores,    a.cap * sizeof(*a.scores));
    if (a.is_anchor) xfree(alloc, a.is_anchor, a.cap * sizeof(*a.is_anchor));
    if (a.rel_src)   xfree(alloc, a.rel_src,   a.cap * sizeof(*a.rel_src));
    if (a.rel_tgt)   xfree(alloc, a.rel_tgt,   a.cap * sizeof(*a.rel_tgt));

    *out = a.items;
    *out_count = a.count;
    return HU_OK;
}
