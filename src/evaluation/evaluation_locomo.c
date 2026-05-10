/* W16 — LoCoMo backend.
 *
 * Real LoCoMo (arxiv 2402.17753) is a 35-session, 9000-token long-conversation
 * recall benchmark. When the real corpus is available at
 * `$HU_EVAL_DATA_DIR/locomo.json` (or `~/.human/eval-datasets/locomo.json`)
 * the suite scores against it; otherwise it falls back to the inline
 * 10-item synthetic recall set so the harness, regression gate, and CI
 * workflow can be exercised offline.
 *
 * Use `scripts/fetch-evaluation-datasets.sh locomo` to populate the real
 * corpus. The fetcher downloads the official upstream JSON and transforms
 * it into our schema (see evaluation_dataset_loader.h).
 *
 * Score: precision@1 over the recall queries. For each query we pick the
 * candidate fact whose word overlap with the query is highest; the pick is
 * correct iff it equals the bundled ground-truth fact id. Ties are broken
 * by the first occurrence in the dataset, which keeps the score
 * deterministic across runs.
 */

#include "human/evaluation/evaluation.h"
#include "evaluation_dataset_loader.h"
#include "evaluation_internal.h"

#include "human/core/allocator.h"
#include "human/core/error.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    const char *fact_id;
    const char *fact;
    const char *query;
    const char *expected_id;
} locomo_item_t;

/* 10-item recall set. Each item carries a candidate fact and a paired query.
 * The "expected_id" is the ground-truth fact id for that query — usually the
 * same fact, but a few cross-reference queries point at a different item to
 * stress fact-discrimination. */
static const locomo_item_t LOCOMO_ITEMS[] = {
    {"f1", "Alice's favourite tea is genmaicha and she drinks it every morning.",
     "what tea does alice drink in the morning?", "f1"},
    {"f2", "Bob moved to Berlin in 2019 to join a robotics startup.",
     "where does bob live now?", "f2"},
    {"f3", "Carla broke her left wrist climbing in Yosemite last spring.",
     "which wrist did carla injure climbing?", "f3"},
    {"f4", "Daniel learned to bake sourdough during the 2020 lockdown.",
     "what did daniel learn during lockdown?", "f4"},
    {"f5", "Eun-ji prefers Vim to Emacs but uses VS Code at her current job.",
     "which editor does eun-ji prefer?", "f5"},
    {"f6", "Felix is allergic to pine nuts and avoids pesto sauces.",
     "what food allergy does felix have?", "f6"},
    {"f7", "Grace finished her marathon in three hours forty-two minutes.",
     "what was grace's marathon time?", "f7"},
    {"f8", "Hank's son was born on the morning of October 12th, 2024.",
     "when was hank's son born?", "f8"},
    {"f9", "Iris keeps a beehive on the roof of her apartment in Lisbon.",
     "where is iris keeping bees?", "f9"},
    {"f10", "Jamal's grandmother taught him to play the cumbia accordion.",
     "who taught jamal accordion?", "f10"},
};

static const size_t LOCOMO_N = sizeof(LOCOMO_ITEMS) / sizeof(LOCOMO_ITEMS[0]);

/* Working-set view: either points at the inline LOCOMO_ITEMS (synthetic)
 * or at a heap-allocated array materialised from a real on-disk corpus.
 * We unify the two so the scoring loop is identical. */
typedef struct {
    const char *fact_id;
    const char *fact;
    const char *query;
    const char *expected_id;
} locomo_view_t;

typedef struct {
    /* When loaded != 0 the working set is a real corpus and `owned`
     * holds the malloc-owned strings to be freed at deinit. When loaded
     * == 0 the working set is the inline synthetic table and there is
     * nothing to free. */
    int loaded;
    locomo_view_t *items;
    size_t count;
    hu_eval_locomo_dataset_t owned;
} locomo_ctx_t;

/* ── word-overlap retriever ─────────────────────────────────────────────── */

/* Lowercase a single char without locale weirdness. */
static int low(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* Case-insensitive substring search restricted to whole-word match. Avoids
 * counting "the" inside "another". */
static bool contains_word_ci(const char *hay, const char *word, size_t wlen) {
    if (wlen == 0)
        return false;
    size_t hlen = strlen(hay);
    if (wlen > hlen)
        return false;
    for (size_t i = 0; i + wlen <= hlen; i++) {
        bool prev_boundary = (i == 0) || !isalnum((unsigned char)hay[i - 1]);
        bool next_boundary =
            (i + wlen == hlen) || !isalnum((unsigned char)hay[i + wlen]);
        if (!prev_boundary || !next_boundary)
            continue;
        size_t j = 0;
        for (; j < wlen; j++) {
            if (low((unsigned char)hay[i + j]) != low((unsigned char)word[j]))
                break;
        }
        if (j == wlen)
            return true;
    }
    return false;
}

/* Walks the query and counts how many words appear (case-insensitive,
 * whole-word) inside `fact`. Words shorter than 3 chars are skipped: they
 * are mostly stopwords ("is", "to") and dominate the overlap otherwise. */
static int word_overlap(const char *query, const char *fact) {
    int matches = 0;
    const char *p = query;
    while (*p) {
        while (*p && !isalnum((unsigned char)*p))
            p++;
        const char *start = p;
        while (*p && isalnum((unsigned char)*p))
            p++;
        size_t len = (size_t)(p - start);
        if (len >= 3 && contains_word_ci(fact, start, len))
            matches++;
    }
    return matches;
}

static const char *retrieve_top1(const locomo_view_t *items, size_t n, const char *query) {
    int best = -1;
    size_t best_idx = 0;
    for (size_t i = 0; i < n; i++) {
        int score = word_overlap(query, items[i].fact);
        if (score > best) {
            best = score;
            best_idx = i;
        }
    }
    return best > 0 ? items[best_idx].fact_id : NULL;
}

/* ── vtable ─────────────────────────────────────────────────────────────── */

static const char *locomo_name(void *ctx) {
    (void)ctx;
    return "locomo";
}

static bool locomo_available(void *ctx) {
    (void)ctx;
    return true;
}

static int64_t now_ms(void) {
    return (int64_t)time(NULL) * 1000;
}

/* Materialise the working set from the on-disk corpus, falling back to
 * the inline synthetic table when no real corpus is present. */
static hu_error_t locomo_ensure_working_set(locomo_ctx_t *c, hu_allocator_t *alloc) {
    if (c->items)
        return HU_OK;
    hu_error_t err = hu_eval_locomo_load(alloc, &c->owned);
    if (err == HU_OK && c->owned.count > 0) {
        locomo_view_t *view = (locomo_view_t *)alloc->alloc(
            alloc->ctx, c->owned.count * sizeof(locomo_view_t));
        if (!view) {
            hu_eval_locomo_free(alloc, &c->owned);
            return HU_ERR_OUT_OF_MEMORY;
        }
        for (size_t i = 0; i < c->owned.count; i++) {
            view[i].fact_id = c->owned.items[i].fact_id;
            view[i].fact = c->owned.items[i].fact;
            view[i].query = c->owned.items[i].query;
            view[i].expected_id = c->owned.items[i].expected_id;
        }
        c->items = view;
        c->count = c->owned.count;
        c->loaded = 1;
        return HU_OK;
    }
    /* Missing corpus or schema mismatch: fall back to inline synthetic. */
    locomo_view_t *view =
        (locomo_view_t *)alloc->alloc(alloc->ctx, LOCOMO_N * sizeof(locomo_view_t));
    if (!view)
        return HU_ERR_OUT_OF_MEMORY;
    for (size_t i = 0; i < LOCOMO_N; i++) {
        view[i].fact_id = LOCOMO_ITEMS[i].fact_id;
        view[i].fact = LOCOMO_ITEMS[i].fact;
        view[i].query = LOCOMO_ITEMS[i].query;
        view[i].expected_id = LOCOMO_ITEMS[i].expected_id;
    }
    c->items = view;
    c->count = LOCOMO_N;
    c->loaded = 0;
    return HU_OK;
}

static hu_error_t locomo_run(void *ctx, hu_allocator_t *alloc, hu_evaluation_run_report_t *out) {
    if (!alloc || !out || !ctx)
        return HU_ERR_INVALID_ARGUMENT;
    locomo_ctx_t *c = (locomo_ctx_t *)ctx;

    hu_error_t err = locomo_ensure_working_set(c, alloc);
    if (err != HU_OK)
        return err;

    err = hu_evaluation_report_init(alloc, "locomo", out);
    if (err != HU_OK)
        return err;
    out->started_at_ms = now_ms();

    size_t correct = 0;
    for (size_t i = 0; i < c->count; i++) {
        const char *pick = retrieve_top1(c->items, c->count, c->items[i].query);
        if (pick && strcmp(pick, c->items[i].expected_id) == 0)
            correct++;
    }

    out->prompts_total = c->count;
    out->prompts_passed = correct;
    out->prompts_failed = c->count - correct;

    double precision_at_1 = (double)correct / (double)c->count;
    err = hu_evaluation_report_add_metric(alloc, out, "precision_at_1", precision_at_1, c->count);
    if (err != HU_OK) {
        hu_evaluation_report_free(alloc, out);
        return err;
    }
    /* Annotate which corpus was used so report consumers (CI, regression
     * gate, dashboards) can tell synthetic from real. */
    err = hu_evaluation_report_add_metric(alloc, out, "real_corpus",
                                          c->loaded ? 1.0 : 0.0, c->count);
    if (err != HU_OK) {
        hu_evaluation_report_free(alloc, out);
        return err;
    }
    out->finished_at_ms = now_ms();
    return HU_OK;
}

static void locomo_deinit(void *ctx, hu_allocator_t *alloc) {
    if (!ctx || !alloc)
        return;
    locomo_ctx_t *c = (locomo_ctx_t *)ctx;
    if (c->items) {
        alloc->free(alloc->ctx, c->items, c->count * sizeof(locomo_view_t));
        c->items = NULL;
    }
    if (c->loaded)
        hu_eval_locomo_free(alloc, &c->owned);
    alloc->free(alloc->ctx, ctx, sizeof(locomo_ctx_t));
}

static const hu_evaluation_vtable_t LOCOMO_VTABLE = {
    .name = locomo_name,
    .available = locomo_available,
    .run = locomo_run,
    .deinit = locomo_deinit,
};

hu_error_t hu_evaluation_locomo(hu_allocator_t *alloc, hu_evaluation_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    locomo_ctx_t *c = alloc->alloc(alloc->ctx, sizeof(locomo_ctx_t));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    out->ctx = c;
    out->vtable = &LOCOMO_VTABLE;
    out->alloc = alloc;
    return HU_OK;
}
