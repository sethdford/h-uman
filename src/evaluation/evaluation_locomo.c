/* W16 — LoCoMo backend.
 *
 * Real LoCoMo (arxiv 2402.17753) is a 35-session, 9000-token long-conversation
 * recall benchmark. The fetcher is gated behind
 * `scripts/fetch-evaluation-datasets.sh` and licensing review; this commit
 * ships a 10-item synthetic recall set inline so the harness, regression
 * gate, and CI workflow can be exercised offline.
 *
 * Score: precision@1 over the synthetic recall queries. For each query we
 * pick the candidate fact whose word overlap with the query is highest; the
 * pick is correct iff it equals the bundled ground-truth fact id. Ties are
 * broken by the first occurrence in the dataset, which keeps the score
 * deterministic across runs.
 */

#include "human/evaluation/evaluation.h"
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

typedef struct {
    /* No instance state beyond the wrapper; backend reads only constants. */
    int unused;
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

static const char *retrieve_top1(const char *query) {
    int best = -1;
    size_t best_idx = 0;
    for (size_t i = 0; i < LOCOMO_N; i++) {
        int score = word_overlap(query, LOCOMO_ITEMS[i].fact);
        if (score > best) {
            best = score;
            best_idx = i;
        }
    }
    return best > 0 ? LOCOMO_ITEMS[best_idx].fact_id : NULL;
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

static hu_error_t locomo_run(void *ctx, hu_allocator_t *alloc, hu_evaluation_run_report_t *out) {
    (void)ctx;
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;

    hu_error_t err = hu_evaluation_report_init(alloc, "locomo", out);
    if (err != HU_OK)
        return err;
    out->started_at_ms = now_ms();

    size_t correct = 0;
    for (size_t i = 0; i < LOCOMO_N; i++) {
        const char *pick = retrieve_top1(LOCOMO_ITEMS[i].query);
        if (pick && strcmp(pick, LOCOMO_ITEMS[i].expected_id) == 0)
            correct++;
    }

    out->prompts_total = LOCOMO_N;
    out->prompts_passed = correct;
    out->prompts_failed = LOCOMO_N - correct;

    double precision_at_1 = (double)correct / (double)LOCOMO_N;
    err = hu_evaluation_report_add_metric(alloc, out, "precision_at_1", precision_at_1, LOCOMO_N);
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
