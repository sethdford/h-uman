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

typedef struct {
    int unused;
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

/* Pass iff every non-NULL keyword appears as a whole word in the candidate. */
static bool item_passes(const lme_item_t *item) {
    for (size_t k = 0; k < sizeof(item->keywords) / sizeof(item->keywords[0]); k++) {
        if (!item->keywords[k])
            break;
        if (!contains_word_ci(item->candidate_answer, item->keywords[k]))
            return false;
    }
    return true;
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

static hu_error_t lme_run(void *ctx, hu_allocator_t *alloc, hu_evaluation_run_report_t *out) {
    (void)ctx;
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;

    hu_error_t err = hu_evaluation_report_init(alloc, "longmemeval", out);
    if (err != HU_OK)
        return err;
    out->started_at_ms = now_ms();

    /* Aggregate per category. */
    size_t total_passed = 0;
    for (size_t c = 0; c < LME_CAT_N; c++) {
        size_t passed = 0;
        size_t in_cat = 0;
        for (size_t i = 0; i < LME_N; i++) {
            if (strcmp(LME_ITEMS[i].category, LME_CATEGORIES[c]) != 0)
                continue;
            in_cat++;
            if (item_passes(&LME_ITEMS[i]))
                passed++;
        }
        double score = in_cat == 0 ? 0.0 : (double)passed / (double)in_cat;
        char metric_name[64];
        int n = snprintf(metric_name, sizeof(metric_name), "category_%s", LME_CATEGORIES[c]);
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

    out->prompts_total = LME_N;
    out->prompts_passed = total_passed;
    out->prompts_failed = LME_N - total_passed;
    out->finished_at_ms = now_ms();
    return HU_OK;
}

static void lme_deinit(void *ctx, hu_allocator_t *alloc) {
    if (!ctx || !alloc)
        return;
    alloc->free(alloc->ctx, ctx, sizeof(lme_ctx_t));
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
