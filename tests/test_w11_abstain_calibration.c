/* W11 — abstain_threshold calibration on graded weak-evidence pack.
 *
 * The W11 plan tail (docs/plans/2026-05-10-w11-inline-self-rag.md
 * "Remaining scope") asks to "calibrate `abstain_threshold` against the
 * 200-prompt annotated suite so the success metric (≥30 % abstention on
 * weak-evidence prompts) is measurable." The 200-prompt corpus is a
 * future external artifact; this in-tree calibration uses a small
 * graded pack (12 weak-evidence + 4 safe = 16 total) so the floor is
 * pinned in CI today and external corpora can supersede it.
 *
 * What we measure (heuristic backend, empty memory facade):
 *   - true positives  — abstained on a weak-evidence prompt ✓
 *   - false positives — abstained on a safe prompt ✗
 *   - recall          — TP / |weak prompts|
 *   - precision       — TP / (TP + FP)
 *   - global rate     — abstentions / |all prompts|
 *
 * What we pin (at the canonical default threshold 0.5):
 *   - global abstention rate ≥ 30 % (the W11 success-metric floor)
 *   - recall on weak-evidence prompts ≥ 50 %
 *   - precision ≥ 75 % (false positives must stay rare; a noisy
 *     refuser is itself a sycophancy-class regression — it teaches
 *     users the assistant doesn't trust its own answers).
 *
 * Across thresholds we additionally pin monotonicity:
 *   - lowering the threshold can only increase or preserve recall
 *     (more unsupported claims trigger abstain), never decrease it.
 *
 * If a future change moves the heuristic-backend behavior across these
 * gates, this file fails with a diagnostic line that surfaces the
 * actual measured numbers — making the regression unmistakable.
 */

#include "human/agent/self_rag.h"
#include "human/core/allocator.h"
#include "human/memory/graph.h"
#include "human/memory/memory.h"
#include "human/agent/world_model.h"
#include "test_framework.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE

static hu_allocator_t cal_alloc;
static hu_allocator_t *CAL(void) {
    cal_alloc = hu_system_allocator();
    return &cal_alloc;
}

typedef struct cal_prompt {
    const char *text;
    bool expected_abstain;
} cal_prompt_t;

/* Graded pack — 12 weak-evidence (factual claims about entities and
 * relations the empty memory facade has never heard of) + 4 safe
 * (questions, opinions, no concrete claims). Fact-shape was tuned to
 * exercise the heuristic backend's claim extractor: each weak prompt
 * contains at least one explicit subject-verb-object proposition. */
static const cal_prompt_t k_pack[] = {
    /* Weak-evidence (expected to abstain on empty memory). */
    {"Alice works at Initech.", true},
    {"Bob lives in Singapore.", true},
    {"Carlos is the CEO of Globex.", true},
    {"Diana studied at Cambridge.", true},
    {"Evelyn drives a Tesla.", true},
    {"Frank moved to Berlin in 2019.", true},
    {"Greta speaks Mandarin and Spanish.", true},
    {"Henrik owns a sailboat called Aurora.", true},
    {"Iris graduated from MIT in 2014.", true},
    {"Jamal hosts a podcast about jazz.", true},
    {"Kira built a startup in Lisbon.", true},
    {"Luis directs a documentary on bees.", true},

    /* Safe prompts — questions, requests, opinion language. The
     * heuristic claim extractor should find no propositional claims
     * here, so abstention should not fire. */
    {"How are you doing today?", false},
    {"Could you help me think through a tricky problem?", false},
    {"I think the autumn light in Brooklyn is the best.", false},
    {"Tell me a joke about debuggers.", false},
};
static const size_t k_pack_n = sizeof(k_pack) / sizeof(k_pack[0]);

typedef struct cal_metrics {
    size_t weak_total;
    size_t safe_total;
    size_t weak_abstained;   /* TP */
    size_t safe_abstained;   /* FP */
} cal_metrics_t;

static void run_pack_at_threshold(hu_self_rag_t *r, float threshold,
                                  cal_metrics_t *out) {
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; i < k_pack_n; i++) {
        if (k_pack[i].expected_abstain)
            out->weak_total++;
        else
            out->safe_total++;

        hu_self_rag_request_t req;
        memset(&req, 0, sizeof(req));
        req.draft = k_pack[i].text;
        req.draft_len = strlen(k_pack[i].text);
        req.mode = HU_VERIFY_SOFT;
        req.contact_id = "u_w11_cal";
        req.contact_id_len = 9;
        req.abstain_threshold = threshold;
        req.now_ms = 1735690000000LL;

        hu_self_rag_response_t resp;
        memset(&resp, 0, sizeof(resp));
        if (hu_self_rag_verify(r, CAL(), &req, &resp) != HU_OK)
            continue;
        if (resp.outcome == HU_SELF_RAG_ABSTAINED) {
            if (k_pack[i].expected_abstain)
                out->weak_abstained++;
            else
                out->safe_abstained++;
        }
    }
}

static unsigned pct(size_t num, size_t den) {
    if (den == 0)
        return 0;
    return (unsigned)((num * 100ULL) / den);
}

/* Pin: at the canonical default threshold (0.5), the heuristic backend
 * meets all three calibration gates on the graded pack.
 *
 * Note: we deliberately do NOT call `hu_world_model_invalidate(NULL, 0)`
 * here — the process-global world-model cache may hold rows from prior
 * suites whose allocators have already gone out of scope, and a global
 * invalidate would crash on free. The heuristic backend doesn't read
 * the world-model cache anyway; it goes through the W7 facade. */
static void w11_cal_default_threshold_meets_floor(void) {
    hu_graph_t *g = NULL;
    HU_ASSERT_EQ(hu_graph_open(CAL(), NULL, 0, &g), HU_OK);
    hu_memory_facade_t *m = NULL;
    HU_ASSERT_EQ(hu_memory_facade_open(CAL(), g, &m), HU_OK);

    hu_self_rag_t r = {0};
    HU_ASSERT_EQ(hu_self_rag_heuristic(m, &r), HU_OK);

    cal_metrics_t mt;
    run_pack_at_threshold(&r, 0.5f, &mt);

    size_t total_abstained = mt.weak_abstained + mt.safe_abstained;
    unsigned global_rate = pct(total_abstained, k_pack_n);
    unsigned recall = pct(mt.weak_abstained, mt.weak_total);
    unsigned precision = total_abstained > 0
                             ? pct(mt.weak_abstained, total_abstained)
                             : 100u;

    /* Three gates with diagnostic messages so a regression names the
     * actual measured number rather than just "an assert failed." */
    if (global_rate < 30u) {
        HU_FAIL("W11 abstain calibration regressed: global abstention rate "
                "%u%% < 30%% floor (weak %zu/%zu, safe %zu/%zu).",
                global_rate, mt.weak_abstained, mt.weak_total,
                mt.safe_abstained, mt.safe_total);
    }
    if (recall < 50u) {
        HU_FAIL("W11 abstain calibration regressed: recall on weak-evidence "
                "%u%% < 50%% floor (%zu/%zu).",
                recall, mt.weak_abstained, mt.weak_total);
    }
    if (precision < 75u) {
        HU_FAIL("W11 abstain calibration regressed: precision %u%% < 75%% "
                "floor (TP=%zu, FP=%zu, total abstained=%zu). Noisy refusals "
                "are a sycophancy-class regression.",
                precision, mt.weak_abstained, mt.safe_abstained,
                total_abstained);
    }

    hu_self_rag_close(&r);
    hu_memory_facade_close(m, CAL());
    hu_graph_close(g, CAL());
}

/* Pin: lowering the threshold can only increase or preserve recall on
 * weak-evidence prompts. A change that violates this means the backend
 * is no longer monotone in `abstain_threshold` and the calibration
 * surface itself is broken (further calibration would be meaningless). */
static void w11_cal_recall_monotone_in_threshold(void) {
    hu_graph_t *g = NULL;
    HU_ASSERT_EQ(hu_graph_open(CAL(), NULL, 0, &g), HU_OK);
    hu_memory_facade_t *m = NULL;
    HU_ASSERT_EQ(hu_memory_facade_open(CAL(), g, &m), HU_OK);

    hu_self_rag_t r = {0};
    HU_ASSERT_EQ(hu_self_rag_heuristic(m, &r), HU_OK);

    cal_metrics_t lo, mid, hi;
    run_pack_at_threshold(&r, 0.3f, &lo);
    run_pack_at_threshold(&r, 0.5f, &mid);
    run_pack_at_threshold(&r, 0.7f, &hi);

    /* lower threshold ≥ higher threshold (in recall, equality OK). */
    HU_ASSERT_GE(lo.weak_abstained, mid.weak_abstained);
    HU_ASSERT_GE(mid.weak_abstained, hi.weak_abstained);

    hu_self_rag_close(&r);
    hu_memory_facade_close(m, CAL());
    hu_graph_close(g, CAL());
}

/* Pin: zero `abstain_threshold` resolves to the default (0.5), per
 * the request struct's documented behavior. Catches a regression
 * where the default fall-through is silently dropped (which would
 * make every caller's verifier silently agreeable when they leave
 * the field zero-init). */
static void w11_cal_zero_threshold_uses_default_05(void) {
    hu_graph_t *g = NULL;
    HU_ASSERT_EQ(hu_graph_open(CAL(), NULL, 0, &g), HU_OK);
    hu_memory_facade_t *m = NULL;
    HU_ASSERT_EQ(hu_memory_facade_open(CAL(), g, &m), HU_OK);

    hu_self_rag_t r = {0};
    HU_ASSERT_EQ(hu_self_rag_heuristic(m, &r), HU_OK);

    cal_metrics_t mt_zero, mt_05;
    run_pack_at_threshold(&r, 0.0f, &mt_zero);
    run_pack_at_threshold(&r, 0.5f, &mt_05);

    HU_ASSERT_EQ(mt_zero.weak_abstained, mt_05.weak_abstained);
    HU_ASSERT_EQ(mt_zero.safe_abstained, mt_05.safe_abstained);

    hu_self_rag_close(&r);
    hu_memory_facade_close(m, CAL());
    hu_graph_close(g, CAL());
}

#endif /* HU_ENABLE_SQLITE */

void run_w11_abstain_calibration_tests(void);

void run_w11_abstain_calibration_tests(void) {
    HU_TEST_SUITE("W11 abstain calibration");
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(w11_cal_default_threshold_meets_floor);
    HU_RUN_TEST(w11_cal_recall_monotone_in_threshold);
    HU_RUN_TEST(w11_cal_zero_threshold_uses_default_05);
#endif
}
