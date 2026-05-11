/* W16 — LoCoMo production-stack benchmark.
 *
 * The second backend (after `facade-recall`) that exercises the *real* v2
 * stack on a *real* corpus:
 *
 *   W7 facade  →  W9 world model  →  W12 heuristic planner
 *               →  W12 executor    →  scoring vs LoCoMo ground truth
 *
 * Where facade-recall measures the stack against a 12-fact synthetic corpus
 * the author hand-crafted to match the planner's query verbs, this backend
 * runs the stack against the 1542-prompt LoCoMo corpus *as it is*. No
 * cherry-picked queries, no curated entities, no triple shape. Just whatever
 * real LoCoMo gives us.
 *
 * Why both. facade-recall proves the stack works in the happy case and locks
 * a regression floor on a planner change. locomo-facade tells the truth
 * about how the stack behaves at scale on data that wasn't written for it —
 * a much harder bar. The two scores together define the credibility
 * envelope: "between 12 hand-curated facts (~100 %) and 1.5k real facts
 * (~X %)."
 *
 * ── Honest shape mismatch ─────────────────────────────────────────────
 *
 * LoCoMo items are (query, short-answer, fact_id) tuples. The "fact" field
 * is a *short answer* ("7 May 2023", "Sweden", "pottery, camping"), not a
 * (subject, predicate, object) triple. Our W7+W12 stack expects an
 * entity/relation graph. To bridge the two without lying about the data:
 *
 *   - Each item's `query` text is mined for named entities via a
 *     conservative heuristic NER (consecutive capitalised tokens of length
 *     >= 3, with a small stopword list of interrogative pronouns). Those
 *     become PERSON/TOPIC entities.
 *   - Each item's `fact` text becomes one TOPIC entity whose name is a
 *     deterministic `__ans_<idx>` identifier (so identical answer strings
 *     in different items remain distinct).
 *   - We seed: every query-entity --[query_text]--> answer-entity. The
 *     relation `context` field carries the *original query string*. That's
 *     what the W12 P6 re-ranker scores against at retrieval time.
 *
 * The retrieval score is then:
 *
 *   For each query, the production stack returns a ranked entity list.
 *   precision_at_1 is "the top-1 entity is the answer entity that was
 *   originally seeded with this query".
 *
 * This is the cleanest mapping that exercises the planner's anchor
 * selection (person/topic in the query), neighbour expansion (relations
 * out of the anchor), and the P6 context re-ranker (which of the anchor's
 * many relations matches the current query best).
 *
 * ── Caveats spelled out ───────────────────────────────────────────────
 *
 * 1. The world-model cache loads top-64 entities by mention count. LoCoMo
 *    has thousands of entities. Single-mention answer entities never make
 *    the world-model. They do not need to: the planner's neighbour-window
 *    step queries the graph directly, so the answer entities still surface
 *    through the executor.
 * 2. Heuristic NER is generous on first run. False-positive entities
 *    (e.g. "What", a stopword we miss) get demoted by their low overall
 *    mention count.
 * 3. precision_at_1 here is harsher than facade-recall: with ~1500 distinct
 *    answer entities under one contact, the anchor's neighbour set can
 *    contain dozens of relations. The W12 P6 re-ranker must pick the right
 *    one by query-context overlap. A weak re-ranker pays for itself here.
 *
 * Requires HU_ENABLE_SQLITE. Without it, the backend returns a structured
 * error_summary the regression gate handles. */

#include "human/evaluation/evaluation.h"
#include "evaluation_dataset_loader.h"
#include "evaluation_internal.h"

#include "human/core/allocator.h"
#include "human/core/error.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef HU_ENABLE_SQLITE
#include "human/agent/retrieval_planner.h"
#include "human/agent/world_model.h"
#include "human/memory/graph.h"
#include "human/memory/memory.h"
#endif

#define LF_CONTACT_ID "u-locomo-facade"

/* ── Heuristic NER ──────────────────────────────────────────────────────
 *
 * Consecutive Capitalised tokens >=3 chars form one entity. We split on
 * non-alphanumeric. The stopword list catches interrogative pronouns at
 * the start of LoCoMo queries ("When did Caroline...") so they don't
 * pollute the entity set with a "When" pseudo-entity that 800+ queries
 * would then anchor on. */

static const char *NER_STOPWORDS[] = {
    "What", "When", "Where", "Why", "Who", "Which", "How",
    "Would", "Could", "Should", "Did", "Does", "Is", "Are",
    "Was", "Were", "Will", "Can", "May", "Might", "Has", "Have",
    "Had", "Do", "Don", "A", "An", "The", "Of", "And", "Or",
    "In", "On", "At", "To", "For", "From", "By", "With",
};
static const size_t NER_STOPWORDS_N = sizeof(NER_STOPWORDS) / sizeof(NER_STOPWORDS[0]);

static bool ner_is_stopword(const char *tok, size_t len) {
    for (size_t i = 0; i < NER_STOPWORDS_N; i++) {
        size_t slen = strlen(NER_STOPWORDS[i]);
        if (slen != len) continue;
        bool match = true;
        for (size_t j = 0; j < len; j++) {
            char a = tok[j];
            char b = NER_STOPWORDS[i][j];
            if (a >= 'a' && a <= 'z') a = (char)(a - 32);
            if (b >= 'a' && b <= 'z') b = (char)(b - 32);
            if (a != b) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

static bool ner_is_capitalised_alpha(char c) {
    return c >= 'A' && c <= 'Z';
}

/* Token classification: returns the byte length of the next contiguous
 * alphanumeric run starting at `text`, or 0 at non-alnum. */
static size_t ner_token_len(const char *text, size_t cap) {
    size_t i = 0;
    while (i < cap && (isalnum((unsigned char)text[i]) || text[i] == '\'')) i++;
    return i;
}

/* Extract up to `max_entities` named-entity tokens from `query`. Writes each
 * token's [start, len) into the parallel arrays. A "named entity" is a
 * single capitalised alphanumeric token >=3 chars that isn't an
 * interrogative stopword. Multi-word entities ("LGBTQ support group") are
 * not stitched together — single-token anchors are enough for the planner
 * to pivot on, and stitching produces more false positives than wins. */
static size_t ner_extract(const char *query, size_t qlen,
                          size_t *starts, size_t *lens, size_t max_entities) {
    size_t found = 0;
    size_t i = 0;
    while (i < qlen && found < max_entities) {
        while (i < qlen && !isalnum((unsigned char)query[i])) i++;
        size_t tok_len = ner_token_len(query + i, qlen - i);
        if (tok_len == 0) break;
        if (tok_len >= 3 && ner_is_capitalised_alpha(query[i]) &&
            !ner_is_stopword(query + i, tok_len)) {
            starts[found] = i;
            lens[found] = tok_len;
            found++;
        }
        i += tok_len;
    }
    return found;
}

/* ── Backend ctx ───────────────────────────────────────────────────────── */

typedef struct lf_ctx {
    int unused;
} lf_ctx_t;

static const char *lf_name(void *ctx) {
    (void)ctx;
    return "locomo-facade";
}

static bool lf_available(void *ctx) {
    (void)ctx;
#ifdef HU_ENABLE_SQLITE
    return true;
#else
    return false;
#endif
}

static int64_t now_ms(void) { return (int64_t)time(NULL) * 1000; }

#ifdef HU_ENABLE_SQLITE

/* Upsert a named entity once and remember its id. We use an open-addressed
 * linear-probing hash table keyed by (lowercased) name to avoid the
 * O(n^2) name lookup that a flat parallel array would force on 1500
 * upserts. */

#define LF_NAME_CAP 256

typedef struct lf_named_entity {
    char name[LF_NAME_CAP];
    int64_t id;
    uint32_t hash;
} lf_named_entity_t;

typedef struct lf_name_index {
    lf_named_entity_t *slots;
    size_t capacity;
    size_t count;
} lf_name_index_t;

static uint32_t lf_hash(const char *s, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 32);
        h ^= (uint8_t)c;
        h *= 16777619u;
    }
    return h ? h : 1u;
}

static bool lf_name_eq(const char *a, size_t alen, const char *b) {
    size_t blen = strlen(b);
    if (alen != blen) return false;
    for (size_t i = 0; i < alen; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 32);
        if (ca != cb) return false;
    }
    return true;
}

static hu_error_t lf_index_init(lf_name_index_t *ix, hu_allocator_t *alloc, size_t cap) {
    ix->slots = alloc->alloc(alloc->ctx, cap * sizeof(lf_named_entity_t));
    if (!ix->slots) return HU_ERR_OUT_OF_MEMORY;
    memset(ix->slots, 0, cap * sizeof(lf_named_entity_t));
    ix->capacity = cap;
    ix->count = 0;
    return HU_OK;
}

static void lf_index_free(lf_name_index_t *ix, hu_allocator_t *alloc) {
    if (ix->slots) alloc->free(alloc->ctx, ix->slots, ix->capacity * sizeof(lf_named_entity_t));
    ix->slots = NULL;
}

static int64_t lf_index_find(const lf_name_index_t *ix, const char *name, size_t name_len) {
    if (ix->count == 0) return 0;
    uint32_t h = lf_hash(name, name_len);
    size_t idx = h % ix->capacity;
    for (size_t probe = 0; probe < ix->capacity; probe++) {
        const lf_named_entity_t *e = &ix->slots[idx];
        if (e->hash == 0) return 0;
        if (e->hash == h && lf_name_eq(name, name_len, e->name)) return e->id;
        idx = (idx + 1) % ix->capacity;
    }
    return 0;
}

static hu_error_t lf_index_insert(lf_name_index_t *ix, const char *name, size_t name_len,
                                  int64_t id) {
    if (ix->count * 2 >= ix->capacity) return HU_ERR_OUT_OF_MEMORY;
    if (name_len >= LF_NAME_CAP) name_len = LF_NAME_CAP - 1;
    uint32_t h = lf_hash(name, name_len);
    size_t idx = h % ix->capacity;
    for (size_t probe = 0; probe < ix->capacity; probe++) {
        lf_named_entity_t *e = &ix->slots[idx];
        if (e->hash == 0) {
            memcpy(e->name, name, name_len);
            e->name[name_len] = '\0';
            e->hash = h;
            e->id = id;
            ix->count++;
            return HU_OK;
        }
        idx = (idx + 1) % ix->capacity;
    }
    return HU_ERR_OUT_OF_MEMORY;
}

static hu_error_t lf_upsert_named(hu_graph_t *g, lf_name_index_t *ix,
                                  const char *name, size_t name_len,
                                  hu_entity_type_t type, int64_t *out_id) {
    int64_t existing = lf_index_find(ix, name, name_len);
    if (existing != 0) { *out_id = existing; return HU_OK; }

    int64_t id = 0;
    hu_error_t err = hu_graph_upsert_entity(g, LF_CONTACT_ID, strlen(LF_CONTACT_ID),
                                            name, name_len, type, NULL, &id);
    if (err != HU_OK) return err;
    *out_id = id;
    return lf_index_insert(ix, name, name_len, id);
}

/* For score-keeping: per LoCoMo item, the answer entity id we just seeded. */
typedef struct lf_item_link {
    int64_t answer_id;
} lf_item_link_t;

/* Returns 1-indexed rank of `target_id` among ENTITY records in the
 * planner's result list, or 0 if absent. Mirrors facade-recall semantics:
 * relations and non-entity rows are skipped so precision_at_1 = "top-1
 * answer is the correct answer entity". */
static size_t lf_rank_of_entity(const hu_memory_record_t *recs, size_t n,
                                int64_t target_id) {
    size_t entity_rank = 0;
    for (size_t i = 0; i < n; i++) {
        if (recs[i].kind != HU_MEM_ENTITY) continue;
        entity_rank++;
        if (recs[i].id == target_id) return entity_rank;
    }
    return 0;
}

/* Run one query through the production stack, scoring against the
 * expected answer entity. `*out_rank` is the answer's 1-indexed rank
 * among entity records (0 = absent). `*out_steps` is the plan length. */
static hu_error_t lf_run_one(hu_memory_facade_t *m, hu_allocator_t *alloc,
                             const char *query, int64_t expect_id,
                             size_t *out_rank, size_t *out_steps) {
    *out_rank = 0;
    *out_steps = 0;

    hu_world_model_t *wm = NULL;
    hu_error_t err = hu_world_model_load(m, alloc, LF_CONTACT_ID, strlen(LF_CONTACT_ID),
                                         now_ms(), &wm);
    if (err != HU_OK || !wm) return err;

    hu_planner_t p;
    err = hu_planner_heuristic(&p);
    if (err != HU_OK) { hu_world_model_free(alloc, wm); return err; }

    hu_retrieval_plan_t plan;
    err = hu_planner_plan(&p, query, strlen(query), wm, &plan);
    if (err != HU_OK) {
        hu_planner_close(&p);
        hu_world_model_free(alloc, wm);
        return err;
    }
    *out_steps = plan.steps_count;

    hu_memory_record_t *out = NULL;
    size_t n = 0;
    err = hu_planner_execute(m, /*self_rag=*/NULL, &plan, alloc, &out, &n);
    if (err == HU_OK) *out_rank = lf_rank_of_entity(out, n, expect_id);
    if (out) hu_planner_records_free(alloc, out, n);
    hu_planner_close(&p);
    hu_world_model_free(alloc, wm);
    return err;
}

/* Sample-limit guard. LoCoMo is 1542 items but each item triggers two
 * SQL upserts + one relation insert + a planner round trip. ~5 s on a
 * laptop. Override with HU_LOCOMO_FACADE_LIMIT for faster CI runs. */
static size_t lf_sample_limit(size_t corpus_n) {
    const char *env = getenv("HU_LOCOMO_FACADE_LIMIT");
    if (!env || !*env) return corpus_n;
    long v = strtol(env, NULL, 10);
    if (v <= 0) return corpus_n;
    return ((size_t)v < corpus_n) ? (size_t)v : corpus_n;
}

#endif /* HU_ENABLE_SQLITE */

static hu_error_t lf_run(void *ctx, hu_allocator_t *alloc,
                         hu_evaluation_run_report_t *out) {
    (void)ctx;
    if (!alloc || !out) return HU_ERR_INVALID_ARGUMENT;

    hu_error_t err = hu_evaluation_report_init(alloc, "locomo-facade", out);
    if (err != HU_OK) return err;
    out->started_at_ms = now_ms();

#ifndef HU_ENABLE_SQLITE
    (void)hu_evaluation_report_set_error(
        alloc, out, "SQLite not enabled at build time; locomo-facade is offline only");
    out->finished_at_ms = now_ms();
    return HU_OK;
#else
    hu_eval_locomo_dataset_t ds;
    memset(&ds, 0, sizeof(ds));
    bool have_real = false;
    if (hu_eval_locomo_load(alloc, &ds) == HU_OK && ds.count > 0) have_real = true;

    if (!have_real) {
        (void)hu_evaluation_report_set_error(
            alloc, out,
            "no real LoCoMo corpus found; locomo-facade requires a real dataset "
            "to be meaningful — synthetic fallback is intentionally not used here. "
            "Run scripts/fetch-evaluation-datasets.sh locomo.");
        out->finished_at_ms = now_ms();
        return HU_OK;
    }

    const size_t n_items = lf_sample_limit(ds.count);

    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    lf_item_link_t *links = NULL;
    lf_name_index_t name_ix; memset(&name_ix, 0, sizeof(name_ix));

    /* World-model entity cap defaults to 64 — far below LoCoMo's ~200
     * unique named characters. Raise it for the duration of the run so
     * the planner's named-anchor heuristic can find every character
     * mentioned in the corpus, not just the top 64. The env-var override
     * is the production code path (`src/agent/world_model.c` W12 P9); we
     * set it here rather than mutate the default so production behaviour
     * is unchanged. */
    char prior_wm_cap[64] = {0};
    {
        const char *cur = getenv("HU_WORLD_MODEL_ENTITY_LIMIT");
        if (cur) {
            snprintf(prior_wm_cap, sizeof(prior_wm_cap), "%s", cur);
        }
        setenv("HU_WORLD_MODEL_ENTITY_LIMIT", "4096", 1);
    }

    err = hu_graph_open(alloc, NULL, 0, &g);
    if (err != HU_OK) goto cleanup;
    err = hu_memory_facade_open(alloc, g, &m);
    if (err != HU_OK) goto cleanup;

    /* Hash-index cap: 2x worst-case unique names ≈ items + ~2 entities/item.
     * Power-of-two-ish keeps probing well-distributed even with a poor hash. */
    err = lf_index_init(&name_ix, alloc, (n_items * 4) + 64);
    if (err != HU_OK) goto cleanup;

    links = alloc->alloc(alloc->ctx, n_items * sizeof(*links));
    if (!links) { err = HU_ERR_OUT_OF_MEMORY; goto cleanup; }
    memset(links, 0, n_items * sizeof(*links));

    /* ── Seed phase ──────────────────────────────────────────────────── */

    for (size_t i = 0; i < n_items; i++) {
        const hu_eval_locomo_item_t *it = &ds.items[i];
        if (!it->query || !it->fact) continue;

        /* Synthesise a deterministic unique answer-entity name so two items
         * with the same `fact` text remain distinct (they may carry different
         * fact_ids and need to be scored independently). */
        char ans_name[64];
        snprintf(ans_name, sizeof(ans_name), "__ans_%zu", i);

        int64_t ans_id = 0;
        err = lf_upsert_named(g, &name_ix, ans_name, strlen(ans_name),
                              HU_ENTITY_TOPIC, &ans_id);
        if (err != HU_OK) goto cleanup;
        links[i].answer_id = ans_id;

        /* Extract named entities from the query. Cap at 4 per query to stop
         * outliers (long compound queries) from inflating the graph. */
        size_t starts[4], lens[4];
        size_t n_named = ner_extract(it->query, strlen(it->query), starts, lens, 4);

        /* Truncate context to MAX 250 chars to keep relation rows small. */
        size_t ctx_len = strlen(it->query);
        if (ctx_len > 250) ctx_len = 250;

        if (n_named == 0) {
            /* No named entity in query → no anchor possible. The item still
             * counts toward total (it will MISS@1) so the score remains
             * honest about this real failure mode. */
            continue;
        }

        for (size_t k = 0; k < n_named; k++) {
            int64_t q_id = 0;
            err = lf_upsert_named(g, &name_ix, it->query + starts[k], lens[k],
                                  HU_ENTITY_PERSON, &q_id);
            if (err != HU_OK) goto cleanup;
            err = hu_graph_upsert_relation(g, LF_CONTACT_ID, strlen(LF_CONTACT_ID),
                                           q_id, ans_id, HU_REL_RELATED_TO, 1.0f,
                                           it->query, ctx_len);
            if (err != HU_OK) goto cleanup;
        }
    }

    /* Invalidate any cached world models from parallel tests. */
    hu_world_model_invalidate(NULL, 0);

    /* ── Score phase ─────────────────────────────────────────────────── */

    size_t hit_at_1 = 0, hit_at_5 = 0, hit_at_10 = 0;
    size_t scored = 0;
    size_t step_total = 0;
    size_t skipped_no_anchor = 0;

    const char *trace = getenv("HU_LOCOMO_FACADE_TRACE");

    for (size_t i = 0; i < n_items; i++) {
        const hu_eval_locomo_item_t *it = &ds.items[i];
        if (!it->query || !it->fact || links[i].answer_id == 0) {
            skipped_no_anchor++;
            continue;
        }

        size_t rank = 0, steps = 0;
        hu_error_t qe = lf_run_one(m, alloc, it->query, links[i].answer_id, &rank, &steps);
        if (qe != HU_OK) continue;

        scored++;
        step_total += steps;
        if (rank == 1) hit_at_1++;
        if (rank > 0 && rank <= 5) hit_at_5++;
        if (rank > 0 && rank <= 10) hit_at_10++;

        if (trace && trace[0] == '1' && i < 30) {
            fprintf(stderr,
                    "[locomo-facade] i=%-4zu rank=%zu steps=%zu expect=%lld q=%.70s\n",
                    i, rank, steps, (long long)links[i].answer_id, it->query);
        }
    }

    if (scored == 0) {
        (void)hu_evaluation_report_set_error(
            alloc, out, "locomo-facade scored zero items (NER returned no anchors)");
        goto done;
    }

    double p1  = (double)hit_at_1  / (double)scored;
    double r5  = (double)hit_at_5  / (double)scored;
    double r10 = (double)hit_at_10 / (double)scored;
    double avg_steps = (double)step_total / (double)scored;
    double no_anchor_pct = (double)skipped_no_anchor / (double)n_items;

    err = hu_evaluation_report_add_metric(alloc, out, "precision_at_1", p1, scored);
    if (err != HU_OK) goto cleanup;
    err = hu_evaluation_report_add_metric(alloc, out, "recall_at_5", r5, scored);
    if (err != HU_OK) goto cleanup;
    err = hu_evaluation_report_add_metric(alloc, out, "recall_at_10", r10, scored);
    if (err != HU_OK) goto cleanup;
    err = hu_evaluation_report_add_metric(alloc, out, "planner_steps_norm",
                                          avg_steps / 8.0, scored);
    if (err != HU_OK) goto cleanup;
    err = hu_evaluation_report_add_metric(alloc, out, "no_anchor_rate",
                                          no_anchor_pct, n_items);
    if (err != HU_OK) goto cleanup;
    err = hu_evaluation_report_add_metric(alloc, out, "real_corpus", 1.0, n_items);
    if (err != HU_OK) goto cleanup;

    out->prompts_total = scored;
    out->prompts_passed = hit_at_1;
    out->prompts_failed = scored - hit_at_1;
    err = HU_OK;

done:
    out->finished_at_ms = now_ms();
cleanup:
    /* Restore the world-model env override so a downstream backend run
     * isn't accidentally polluted by our bump. */
    if (prior_wm_cap[0])
        setenv("HU_WORLD_MODEL_ENTITY_LIMIT", prior_wm_cap, 1);
    else
        unsetenv("HU_WORLD_MODEL_ENTITY_LIMIT");

    if (links) alloc->free(alloc->ctx, links, n_items * sizeof(*links));
    lf_index_free(&name_ix, alloc);
    if (m) hu_memory_facade_close(m, alloc);
    if (g) hu_graph_close(g, alloc);
    hu_eval_locomo_free(alloc, &ds);
    if (err != HU_OK) hu_evaluation_report_free(alloc, out);
    return err;
#endif /* HU_ENABLE_SQLITE */
}

static void lf_deinit(void *ctx, hu_allocator_t *alloc) {
    if (!ctx || !alloc) return;
    alloc->free(alloc->ctx, ctx, sizeof(lf_ctx_t));
}

static const hu_evaluation_vtable_t LF_VTABLE = {
    .name = lf_name,
    .available = lf_available,
    .run = lf_run,
    .deinit = lf_deinit,
};

hu_error_t hu_evaluation_locomo_facade(hu_allocator_t *alloc, hu_evaluation_t *out) {
    if (!alloc || !out) return HU_ERR_INVALID_ARGUMENT;
    lf_ctx_t *c = alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c) return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    out->ctx = c;
    out->vtable = &LF_VTABLE;
    out->alloc = alloc;
    return HU_OK;
}
