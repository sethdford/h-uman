/* W16 — LongMemEval backend.
 *
 * Real LongMemEval — see paper arxiv:2410.10813 — splits long-conversation
 * memory into five task categories: temporal, multi-hop, single-hop,
 * abstention, and knowledge-update. Real fetch is gated; this file ships a
 * 10-prompt synthetic split (2 per category) so the harness reports five
 * category metrics and the regression gate has something to compare each
 * commit.
 *
 * Score per category: pass-rate over the 2 prompts in that category. Each
 * prompt's "expected" answer is a small set of keywords; the prompt passes
 * iff every keyword appears (case-insensitive, whole-word) in the candidate
 * answer string. Candidate answers are bundled with the prompts so this stays
 * deterministic without calling a provider.
 */

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

typedef struct {
    const char *category;
    const char *prompt;
    const char *candidate_answer;
    /* up to 4 keywords, NULL-terminated */
    const char *keywords[4];
} lme_item_t;

/* 10 items, 2 per category. The candidate strings simulate what a strong
 * memory pipeline would emit. Keep them stable — bumping wording shifts the
 * baseline. */
static const lme_item_t LME_ITEMS[] = {
    /* Temporal: the system must reason about time intervals or order. */
    {"temporal",
     "How long ago did Alice start her current job?",
     "She started in March 2024, so about eighteen months ago.",
     {"march", "2024", NULL, NULL}},
    {"temporal",
     "Which holiday did Bob travel to before his first child was born?",
     "Bob travelled to Mexico in summer 2022, before his daughter arrived in 2023.",
     {"mexico", "2022", NULL, NULL}},

    /* Multi-hop: reasoning across two or more memory entries. */
    {"multi_hop",
     "Where does Carla's manager live?",
     "Carla's manager is Diego, and Diego moved to Lisbon last year.",
     {"diego", "lisbon", NULL, NULL}},
    {"multi_hop",
     "Which language does Eun-ji's brother speak at work?",
     "Her brother Min-ho works at a Tokyo trading firm and speaks Japanese.",
     {"min-ho", "japanese", NULL, NULL}},

    /* Single-hop: direct lookup of one stored fact. */
    {"single_hop",
     "What is Felix's favourite restaurant?",
     "Felix's favourite restaurant is Pizzeria Bianco.",
     {"pizzeria", "bianco", NULL, NULL}},
    {"single_hop",
     "What instrument does Grace play in her band?",
     "Grace plays the standup bass in her jazz quartet.",
     {"standup", "bass", NULL, NULL}},

    /* Abstention: the system must say it does not know rather than fabricate. */
    {"abstention",
     "What is Hank's mother's maiden name?",
     "I do not have that information about Hank's family in memory.",
     {"do not", "information", NULL, NULL}},
    {"abstention",
     "Which colour did Iris paint her childhood bedroom?",
     "I cannot recall that detail; it was never shared with me.",
     {"cannot", "recall", NULL, NULL}},

    /* Knowledge-update: a stored fact has been overridden by a newer one. */
    {"knowledge_update",
     "What is Jamal's current address?",
     "Jamal moved last month; his current address is 14 Maple Street.",
     {"maple", "street", NULL, NULL}},
    {"knowledge_update",
     "Which phone number should I use for Kira?",
     "Kira changed numbers in April; use the new one ending in 4421.",
     {"april", "4421", NULL, NULL}},
};

static const size_t LME_N = sizeof(LME_ITEMS) / sizeof(LME_ITEMS[0]);

static const char *const LME_CATEGORIES[] = {
    "temporal", "multi_hop", "single_hop", "abstention", "knowledge_update",
};
static const size_t LME_CAT_N = sizeof(LME_CATEGORIES) / sizeof(LME_CATEGORIES[0]);

/* Working-set view: same lifetime model as LoCoMo — either a borrowed
 * pointer into the inline synthetic table OR a malloc-owned array
 * materialised from a real on-disk corpus. */
typedef struct {
    const char *category;
    const char *candidate_answer;
    const char *keywords[HU_EVAL_LME_MAX_KEYWORDS];
    size_t keyword_count;
} lme_view_t;

typedef struct {
    int loaded;
    lme_view_t *items;
    size_t count;
    hu_eval_lme_dataset_t owned;
} lme_ctx_t;

/* ── helpers ────────────────────────────────────────────────────────────── */

static int low(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static bool contains_word_ci(const char *hay, const char *word) {
    size_t wlen = strlen(word);
    size_t hlen = strlen(hay);
    if (wlen == 0 || wlen > hlen)
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

/* Pass iff every keyword appears as a whole word in the candidate. */
static bool view_passes(const lme_view_t *item) {
    for (size_t k = 0; k < item->keyword_count; k++) {
        if (!item->keywords[k])
            return false;
        if (!contains_word_ci(item->candidate_answer, item->keywords[k]))
            return false;
    }
    return item->keyword_count > 0;
}

/* ── vtable ─────────────────────────────────────────────────────────────── */

static const char *lme_name(void *ctx) {
    (void)ctx;
    return "longmemeval";
}

static bool lme_available(void *ctx) {
    (void)ctx;
    return true;
}

static int64_t now_ms(void) {
    return (int64_t)time(NULL) * 1000;
}

/* Materialise the working set from disk, falling back to the inline
 * synthetic split when the on-disk corpus is missing or malformed. */
static hu_error_t lme_ensure_working_set(lme_ctx_t *c, hu_allocator_t *alloc) {
    if (c->items)
        return HU_OK;
    hu_error_t err = hu_eval_lme_load(alloc, &c->owned);
    if (err == HU_OK && c->owned.count > 0) {
        lme_view_t *view = (lme_view_t *)alloc->alloc(
            alloc->ctx, c->owned.count * sizeof(lme_view_t));
        if (!view) {
            hu_eval_lme_free(alloc, &c->owned);
            return HU_ERR_OUT_OF_MEMORY;
        }
        for (size_t i = 0; i < c->owned.count; i++) {
            view[i].category = c->owned.items[i].category;
            view[i].candidate_answer = c->owned.items[i].candidate_answer;
            view[i].keyword_count = c->owned.items[i].keyword_count;
            for (size_t k = 0; k < HU_EVAL_LME_MAX_KEYWORDS; k++)
                view[i].keywords[k] = c->owned.items[i].keywords[k];
        }
        c->items = view;
        c->count = c->owned.count;
        c->loaded = 1;
        return HU_OK;
    }
    /* Fall back to inline synthetic. */
    lme_view_t *view = (lme_view_t *)alloc->alloc(
        alloc->ctx, LME_N * sizeof(lme_view_t));
    if (!view)
        return HU_ERR_OUT_OF_MEMORY;
    for (size_t i = 0; i < LME_N; i++) {
        view[i].category = LME_ITEMS[i].category;
        view[i].candidate_answer = LME_ITEMS[i].candidate_answer;
        size_t kw = 0;
        for (; kw < HU_EVAL_LME_MAX_KEYWORDS; kw++) {
            if (!LME_ITEMS[i].keywords[kw])
                break;
            view[i].keywords[kw] = LME_ITEMS[i].keywords[kw];
        }
        view[i].keyword_count = kw;
        /* zero unused keyword slots */
        for (; kw < HU_EVAL_LME_MAX_KEYWORDS; kw++)
            view[i].keywords[kw] = NULL;
    }
    c->items = view;
    c->count = LME_N;
    c->loaded = 0;
    return HU_OK;
}

static hu_error_t lme_run(void *ctx, hu_allocator_t *alloc, hu_evaluation_run_report_t *out) {
    if (!alloc || !out || !ctx)
        return HU_ERR_INVALID_ARGUMENT;
    lme_ctx_t *c = (lme_ctx_t *)ctx;

    hu_error_t err = lme_ensure_working_set(c, alloc);
    if (err != HU_OK)
        return err;

    err = hu_evaluation_report_init(alloc, "longmemeval", out);
    if (err != HU_OK)
        return err;
    out->started_at_ms = now_ms();

    /* Aggregate per category. */
    size_t total_passed = 0;
    for (size_t cat_idx = 0; cat_idx < LME_CAT_N; cat_idx++) {
        size_t passed = 0;
        size_t in_cat = 0;
        for (size_t i = 0; i < c->count; i++) {
            if (strcmp(c->items[i].category, LME_CATEGORIES[cat_idx]) != 0)
                continue;
            in_cat++;
            if (view_passes(&c->items[i]))
                passed++;
        }
        double score = in_cat == 0 ? 0.0 : (double)passed / (double)in_cat;
        char metric_name[64];
        int n = snprintf(metric_name, sizeof(metric_name), "category_%s",
                         LME_CATEGORIES[cat_idx]);
        if (n < 0 || (size_t)n >= sizeof(metric_name)) {
            hu_evaluation_report_free(alloc, out);
            return HU_ERR_INTERNAL;
        }
        err = hu_evaluation_report_add_metric(alloc, out, metric_name, score, in_cat);
        if (err != HU_OK) {
            hu_evaluation_report_free(alloc, out);
            return err;
        }
        total_passed += passed;
    }

    out->prompts_total = c->count;
    out->prompts_passed = total_passed;
    out->prompts_failed = c->count - total_passed;

    err = hu_evaluation_report_add_metric(alloc, out, "real_corpus",
                                          c->loaded ? 1.0 : 0.0, c->count);
    if (err != HU_OK) {
        hu_evaluation_report_free(alloc, out);
        return err;
    }
    out->finished_at_ms = now_ms();
    return HU_OK;
}

static void lme_deinit(void *ctx, hu_allocator_t *alloc) {
    if (!ctx || !alloc)
        return;
    lme_ctx_t *c = (lme_ctx_t *)ctx;
    if (c->items) {
        alloc->free(alloc->ctx, c->items, c->count * sizeof(lme_view_t));
        c->items = NULL;
    }
    if (c->loaded)
        hu_eval_lme_free(alloc, &c->owned);
    alloc->free(alloc->ctx, c, sizeof(lme_ctx_t));
}

static const hu_evaluation_vtable_t LME_VTABLE = {
    .name = lme_name,
    .available = lme_available,
    .run = lme_run,
    .deinit = lme_deinit,
};

hu_error_t hu_evaluation_longmemeval(hu_allocator_t *alloc, hu_evaluation_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    lme_ctx_t *c = alloc->alloc(alloc->ctx, sizeof(lme_ctx_t));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    out->ctx = c;
    out->vtable = &LME_VTABLE;
    out->alloc = alloc;
    return HU_OK;
}
