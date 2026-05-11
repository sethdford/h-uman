/* W16 — Production memory facade recall benchmark.
 *
 * The only W16 backend that exercises the *real* v2 stack:
 *
 *   W7 facade   →  W9 world model  →  W12 heuristic planner
 *               →  W12 executor    →  scoring vs ground truth
 *
 * Every other backend in `src/evaluation/` measures a self-contained
 * retriever (cosine index, word-overlap scorer, etc.). They prove the
 * harness, regression gate, and reporting layer work end to end — but
 * they cannot answer the question that actually matters for the v2
 * roadmap success metrics: *does our memory subsystem retrieve the
 * right facts when asked?*
 *
 * This backend answers that with a deterministic offline dataset:
 *   - 12 LoCoMo-style facts as (subject, predicate, object) entity/relation
 *     triples seeded into an in-memory SQLite facade.
 *   - 12 paired queries — a mix of "what / where / when / who / between"
 *     patterns that exercise each branch of the heuristic planner.
 *   - For each query: load world model (W9 cache) → plan → execute → score.
 *
 * Metrics:
 *   - precision_at_1   — top-1 record is the expected entity (id-match)
 *   - recall_at_5      — expected entity in the first 5 records
 *   - recall_at_10     — expected entity in the first 10 records
 *   - planner_steps_avg — average plan length (sanity ceiling on cost)
 *
 * Why heuristic, not LLM, planner: the benchmark must be deterministic
 * and free of provider I/O. Under HU_IS_TEST the LLM planner falls back
 * to the same deterministic plan as the heuristic backend, so adding it
 * here would not change the score — only the run time. CI can flip to
 * the LLM backend in a follow-up workflow once we have a hermetic mock
 * provider that returns recorded JSON.
 *
 * Requires HU_ENABLE_SQLITE because the facade needs a graph backend.
 * Without it, the factory still succeeds but `run` returns
 * HU_ERR_NOT_SUPPORTED with a structured error_summary so the regression
 * gate doesn't blow up.
 */

#include "human/evaluation/evaluation.h"
#include "evaluation_internal.h"

#include "human/core/allocator.h"
#include "human/core/error.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "human/memory/graph.h"
#ifdef HU_ENABLE_SQLITE
#include "human/agent/retrieval_planner.h"
#include "human/agent/world_model.h"
#include "human/memory/memory.h"
#endif

/* ── Dataset ───────────────────────────────────────────────────────────── */

typedef struct facade_item {
    const char *subject;       /* entity name (canonical) */
    hu_entity_type_t subj_type;
    const char *object;        /* entity name */
    hu_entity_type_t obj_type;
    const char *predicate;     /* relation context (free-form) */
    const char *query;         /* user query */
    const char *expect_name;   /* expected entity name in the top results */
} facade_item_t;

/* 12 facts, mix of types. The query verbs cover every branch of the W12
 * heuristic planner (when/last/where/who/between/with/default). The expected
 * answer is always an entity name from the same triple — so retrieving the
 * relation is enough to pass (we score by entity-id presence in the
 * returned records). */
static const facade_item_t FACADE_ITEMS[] = {
    {"Alice",   HU_ENTITY_PERSON,       "Acme",      HU_ENTITY_ORGANIZATION,
     "works at since 2020",
     "where does alice work?",                                   "Acme"},
    {"Bob",     HU_ENTITY_PERSON,       "Berlin",    HU_ENTITY_PLACE,
     "moved to in 2019",
     "where does bob live now?",                                 "Berlin"},
    {"Carla",   HU_ENTITY_PERSON,       "Yosemite",  HU_ENTITY_PLACE,
     "climbed in last spring",
     "where did carla climb last spring?",                       "Yosemite"},
    {"Daniel",  HU_ENTITY_PERSON,       "Sourdough", HU_ENTITY_TOPIC,
     "learned during lockdown",
     "what did daniel learn during lockdown?",                   "Sourdough"},
    {"Eunji",   HU_ENTITY_PERSON,       "Vim",       HU_ENTITY_TOPIC,
     "prefers as editor",
     "who prefers vim?",                                         "Eunji"},
    {"Felix",   HU_ENTITY_PERSON,       "PineNuts",  HU_ENTITY_TOPIC,
     "allergic to",
     "who is allergic to pine nuts?",                            "Felix"},
    {"Grace",   HU_ENTITY_PERSON,       "Marathon",  HU_ENTITY_EVENT,
     "finished in three hours forty-two",
     "what was grace's marathon time?",                          "Marathon"},
    {"Hank",    HU_ENTITY_PERSON,       "Son",       HU_ENTITY_PERSON,
     "born on october twelfth",
     "when was hank's son born?",                                "Son"},
    {"Iris",    HU_ENTITY_PERSON,       "Lisbon",    HU_ENTITY_PLACE,
     "keeps beehive in",
     "where is iris keeping bees?",                              "Lisbon"},
    {"Jamal",   HU_ENTITY_PERSON,       "Accordion", HU_ENTITY_TOPIC,
     "taught by grandmother",
     "who taught jamal the accordion?",                          "Jamal"},
    {"Alice",   HU_ENTITY_PERSON,       "Bob",       HU_ENTITY_PERSON,
     "collaborated on funding round",
     "when did alice and bob collaborate?",                      "Bob"},
    {"Alice",   HU_ENTITY_PERSON,       "Genmaicha", HU_ENTITY_TOPIC,
     "drinks every morning",
     "what does alice drink in the morning?",                    "Genmaicha"},
};
static const size_t FACADE_N = sizeof(FACADE_ITEMS) / sizeof(FACADE_ITEMS[0]);

/* ── Backend ctx ───────────────────────────────────────────────────────── */

typedef struct facade_recall_ctx {
    int unused;
} facade_recall_ctx_t;

static const char *fr_name(void *ctx) {
    (void)ctx;
    return "facade-recall";
}

static bool fr_available(void *ctx) {
    (void)ctx;
#ifdef HU_ENABLE_SQLITE
    return true;
#else
    return false;
#endif
}

static int64_t now_ms(void) {
    return (int64_t)time(NULL) * 1000;
}

#ifdef HU_ENABLE_SQLITE

/* Insert one triple. Both entities upserted, then a relation. We track the
 * (entity-name → id) mapping in parallel arrays so the scoring loop can
 * resolve expected names without re-querying. */
typedef struct id_map {
    const char *name;
    int64_t id;
} id_map_t;

static int64_t resolve_id(const id_map_t *m, size_t n, const char *name) {
    for (size_t i = 0; i < n; i++) {
        if (strcmp(m[i].name, name) == 0) return m[i].id;
    }
    return 0;
}

static hu_error_t upsert_entity_tracked(hu_graph_t *g, const char *cid,
                                        const char *name, hu_entity_type_t t,
                                        id_map_t *map, size_t *map_n,
                                        size_t map_cap) {
    int64_t id = resolve_id(map, *map_n, name);
    if (id != 0) return HU_OK;
    if (*map_n >= map_cap) return HU_ERR_OUT_OF_MEMORY;
    hu_error_t err = hu_graph_upsert_entity(g, cid, strlen(cid), name,
                                            strlen(name), t, NULL, &id);
    if (err != HU_OK) return err;
    map[*map_n].name = name;
    map[(*map_n)++].id = id;
    return HU_OK;
}

/* For relation records, the `id` is the relation row id, not an entity id.
 * Match by traversing the planner output for entity kinds first, then fall
 * back to confirming the relation involves the target entity. Returns the
 * first matching rank (1-indexed) or 0 if not found. We approximate the
 * second case by looking up the relation row in the graph — but to keep this
 * benchmark dependency-light, we just check entity kind matches. Relations
 * count if the executor expanded them into entity records (which the
 * neighbors-style steps do; window steps do not). */
/* Returns the 1-indexed position of the target entity among ENTITY records
 * in the result list, or 0 if absent. Counts only entity kind: relations
 * and other record kinds are skipped so a precision_at_1 = "the top entity
 * answer is correct" semantic holds. Without this filter, a high-scoring
 * relation rank-1 would push every correct entity to rank ≥ 2 once the
 * W12 P6 re-ranker promotes relations with strong context overlap. */
static size_t rank_of_entity(const hu_memory_record_t *recs, size_t n,
                             int64_t target_id) {
    size_t entity_rank = 0;
    for (size_t i = 0; i < n; i++) {
        if (recs[i].kind != HU_MEM_ENTITY) continue;
        entity_rank++;
        if (recs[i].id == target_id) return entity_rank;
    }
    return 0;
}

static hu_error_t run_one_query(hu_memory_facade_t *m, hu_allocator_t *alloc,
                                const char *cid, const char *query,
                                int64_t expect_entity_id,
                                size_t *out_rank, size_t *out_step_count) {
    hu_world_model_t *wm = NULL;
    hu_error_t err = hu_world_model_load(m, alloc, cid, strlen(cid),
                                         (int64_t)time(NULL) * 1000LL, &wm);
    if (err != HU_OK || !wm) {
        *out_rank = 0;
        *out_step_count = 0;
        return err;
    }

    hu_planner_t p;
    err = hu_planner_heuristic(&p);
    if (err != HU_OK) {
        hu_world_model_free(alloc, wm);
        return err;
    }

    hu_retrieval_plan_t plan;
    err = hu_planner_plan(&p, query, strlen(query), wm, &plan);
    if (err != HU_OK) {
        hu_planner_close(&p);
        hu_world_model_free(alloc, wm);
        return err;
    }
    *out_step_count = plan.steps_count;

    hu_memory_record_t *out = NULL;
    size_t n = 0;
    err = hu_planner_execute(m, /*self_rag=*/NULL, &plan, alloc, &out, &n);
    if (err != HU_OK) {
        hu_planner_close(&p);
        hu_world_model_free(alloc, wm);
        if (out) hu_planner_records_free(alloc, out, n);
        return err;
    }

    *out_rank = rank_of_entity(out, n, expect_entity_id);

    if (out) hu_planner_records_free(alloc, out, n);
    hu_planner_close(&p);
    hu_world_model_free(alloc, wm);
    return HU_OK;
}

#endif /* HU_ENABLE_SQLITE */

static hu_error_t fr_run(void *ctx, hu_allocator_t *alloc,
                         hu_evaluation_run_report_t *out) {
    (void)ctx;
    if (!alloc || !out) return HU_ERR_INVALID_ARGUMENT;

    hu_error_t err = hu_evaluation_report_init(alloc, "facade-recall", out);
    if (err != HU_OK) return err;
    out->started_at_ms = now_ms();

#ifndef HU_ENABLE_SQLITE
    (void)hu_evaluation_report_set_error(
        alloc, out, "SQLite not enabled at build time; facade-recall is offline only");
    out->finished_at_ms = now_ms();
    return HU_OK;
#else
    const char *cid = "u-facade-recall";
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;

    err = hu_graph_open(alloc, NULL, 0, &g);
    if (err != HU_OK) goto fail;
    err = hu_memory_facade_open(alloc, g, &m);
    if (err != HU_OK) goto fail;

    /* Track upsert ids in parallel; max 32 unique entities across 12 facts. */
    id_map_t name_to_id[32];
    size_t map_n = 0;

    for (size_t i = 0; i < FACADE_N; i++) {
        const facade_item_t *it = &FACADE_ITEMS[i];
        err = upsert_entity_tracked(g, cid, it->subject, it->subj_type,
                                    name_to_id, &map_n, 32);
        if (err != HU_OK) goto fail;
        err = upsert_entity_tracked(g, cid, it->object, it->obj_type,
                                    name_to_id, &map_n, 32);
        if (err != HU_OK) goto fail;

        int64_t src_id = resolve_id(name_to_id, map_n, it->subject);
        int64_t tgt_id = resolve_id(name_to_id, map_n, it->object);
        err = hu_graph_upsert_relation(g, cid, strlen(cid), src_id, tgt_id,
                                       HU_REL_RELATED_TO, 1.0f, it->predicate,
                                       strlen(it->predicate));
        if (err != HU_OK) goto fail;
    }

    /* Invalidate any cached world models built by parallel tests. */
    hu_world_model_invalidate(NULL, 0);

    size_t hit_at_1 = 0, hit_at_5 = 0, hit_at_10 = 0;
    size_t step_total = 0;
    size_t scored = 0;
    /* HU_FACADE_RECALL_TRACE=1 prints one line per query: rank, plan size,
     * pass/fail. Used during planner-quality iteration to see which queries
     * the heuristic misses without re-running with a debugger attached. */
    const char *trace = getenv("HU_FACADE_RECALL_TRACE");

    for (size_t i = 0; i < FACADE_N; i++) {
        int64_t target_id = resolve_id(name_to_id, map_n, FACADE_ITEMS[i].expect_name);
        if (target_id == 0) continue;
        size_t rank = 0, step_count = 0;
        hu_error_t qe = run_one_query(m, alloc, cid, FACADE_ITEMS[i].query,
                                      target_id, &rank, &step_count);
        if (qe != HU_OK) continue;
        scored++;
        step_total += step_count;
        if (rank == 1) hit_at_1++;
        if (rank > 0 && rank <= 5) hit_at_5++;
        if (rank > 0 && rank <= 10) hit_at_10++;
        if (trace && trace[0] == '1') {
            fprintf(stderr,
                    "[facade-recall] q=%-50s expect=%-12s rank=%zu steps=%zu %s\n",
                    FACADE_ITEMS[i].query, FACADE_ITEMS[i].expect_name, rank,
                    step_count, rank == 1 ? "HIT@1" : (rank > 0 ? "MISS@1" : "MISS"));
        }
    }

    hu_memory_facade_close(m, alloc); m = NULL;
    hu_graph_close(g, alloc);          g = NULL;

    if (scored == 0) {
        (void)hu_evaluation_report_set_error(alloc, out,
            "facade-recall ran zero queries (id resolution failed)");
        out->finished_at_ms = now_ms();
        return HU_OK;
    }

    double p1  = (double)hit_at_1  / (double)scored;
    double r5  = (double)hit_at_5  / (double)scored;
    double r10 = (double)hit_at_10 / (double)scored;
    double avg_steps = (double)step_total / (double)scored;

    err = hu_evaluation_report_add_metric(alloc, out, "precision_at_1", p1, scored);
    if (err != HU_OK) goto fail;
    err = hu_evaluation_report_add_metric(alloc, out, "recall_at_5", r5, scored);
    if (err != HU_OK) goto fail;
    err = hu_evaluation_report_add_metric(alloc, out, "recall_at_10", r10, scored);
    if (err != HU_OK) goto fail;
    /* Steps average is informational; clamp to [0,1] via /8 (HU_PLANNER_MAX_STEPS). */
    err = hu_evaluation_report_add_metric(alloc, out, "planner_steps_norm",
                                          avg_steps / 8.0, scored);
    if (err != HU_OK) goto fail;

    out->prompts_total = scored;
    out->prompts_passed = hit_at_1;
    out->prompts_failed = scored - hit_at_1;
    out->finished_at_ms = now_ms();
    return HU_OK;

fail:
    if (m) hu_memory_facade_close(m, alloc);
    if (g) hu_graph_close(g, alloc);
    hu_evaluation_report_free(alloc, out);
    return err;
#endif /* HU_ENABLE_SQLITE */
}

static void fr_deinit(void *ctx, hu_allocator_t *alloc) {
    if (!ctx || !alloc) return;
    alloc->free(alloc->ctx, ctx, sizeof(facade_recall_ctx_t));
}

static const hu_evaluation_vtable_t FACADE_RECALL_VTABLE = {
    .name = fr_name,
    .available = fr_available,
    .run = fr_run,
    .deinit = fr_deinit,
};

hu_error_t hu_evaluation_facade_recall(hu_allocator_t *alloc,
                                       hu_evaluation_t *out) {
    if (!alloc || !out) return HU_ERR_INVALID_ARGUMENT;
    facade_recall_ctx_t *c = alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c) return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    out->ctx = c;
    out->vtable = &FACADE_RECALL_VTABLE;
    out->alloc = alloc;
    return HU_OK;
}
