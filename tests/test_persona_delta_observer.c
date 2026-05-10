/* FIX 9 — Persona delta observer (W5 producer).
 *
 * Validates pattern detection, false-positive resistance, and end-to-end
 * propose -> evolver flow. */

#include "human/core/allocator.h"
#include "human/memory/graph.h"
#include "human/persona/delta_observer.h"
#include "human/persona/persona_deltas.h"
#include "test_framework.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE

static hu_allocator_t g_alloc;
static hu_allocator_t *A(void) {
    g_alloc = hu_system_allocator();
    return &g_alloc;
}

static void open_graph(hu_graph_t **g) { HU_ASSERT_EQ(hu_graph_open(A(), NULL, 0, g), HU_OK); }

/* Helper: count pending deltas of a kind for u1. */
static size_t count_pending(hu_graph_t *g, hu_persona_delta_kind_t kind) {
    hu_persona_delta_t *out = NULL;
    size_t n = 0;
    hu_persona_delta_list(g, A(), "u1", 2, HU_DELTA_STATUS_PENDING, 64, &out, &n);
    size_t c = 0;
    for (size_t i = 0; i < n; i++)
        if (out[i].kind == kind)
            c++;
    hu_persona_delta_free(A(), out, n);
    return c;
}

/* --- "be more X" / "be less X" --- */

static void observer_proposes_be_more_concise_as_length(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);
    size_t observed = 0;
    HU_ASSERT_EQ(
        hu_persona_observe_user_correction(g, "u1", 2, "slack", 5, "Be more concise please.",
                                           strlen("Be more concise please."), 1700000000000LL,
                                           &observed),
        HU_OK);
    HU_ASSERT_EQ(observed, 1);
    HU_ASSERT_EQ(count_pending(g, HU_PERSONA_DELTA_LENGTH), 1);

    hu_persona_delta_t *out = NULL;
    size_t n = 0;
    hu_persona_delta_list(g, A(), "u1", 2, HU_DELTA_STATUS_PENDING, 16, &out, &n);
    HU_ASSERT_EQ(n, 1);
    HU_ASSERT_STR_EQ(out[0].value, "more concise");
    HU_ASSERT_STR_EQ(out[0].key, "slack");
    HU_ASSERT_STR_EQ(out[0].source, "user-explicit-correction");
    hu_persona_delta_free(A(), out, n);
    hu_graph_close(g, A());
}

static void observer_proposes_be_more_formal_as_formality(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);
    size_t observed = 0;
    hu_persona_observe_user_correction(g, "u1", 2, "email", 5, "be more formal", 14,
                                        1700000000000LL, &observed);
    HU_ASSERT_EQ(observed, 1);
    HU_ASSERT_EQ(count_pending(g, HU_PERSONA_DELTA_FORMALITY), 1);
    hu_graph_close(g, A());
}

static void observer_proposes_be_less_wordy_as_length(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);
    size_t observed = 0;
    hu_persona_observe_user_correction(g, "u1", 2, "imessage", 8, "Be less wordy", 13,
                                        1700000000000LL, &observed);
    HU_ASSERT_EQ(observed, 1);
    HU_ASSERT_EQ(count_pending(g, HU_PERSONA_DELTA_LENGTH), 1);
    hu_graph_close(g, A());
}

static void observer_proposes_be_more_warm_as_tone(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);
    size_t observed = 0;
    hu_persona_observe_user_correction(g, "u1", 2, "imessage", 8, "be more warm", 12,
                                        1700000000000LL, &observed);
    HU_ASSERT_EQ(observed, 1);
    HU_ASSERT_EQ(count_pending(g, HU_PERSONA_DELTA_TONE), 1);
    hu_graph_close(g, A());
}

/* --- "stop saying X" / "don't say X" --- */

static void observer_proposes_stop_saying_as_vocab_avoid(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);
    size_t observed = 0;
    hu_persona_observe_user_correction(g, "u1", 2, "slack", 5, "Please stop saying sorry.", 25,
                                        1700000000000LL, &observed);
    HU_ASSERT_EQ(observed, 1);
    HU_ASSERT_EQ(count_pending(g, HU_PERSONA_DELTA_VOCAB_AVOID), 1);

    hu_persona_delta_t *out = NULL;
    size_t n = 0;
    hu_persona_delta_list(g, A(), "u1", 2, HU_DELTA_STATUS_PENDING, 16, &out, &n);
    HU_ASSERT_EQ(n, 1);
    HU_ASSERT_STR_EQ(out[0].value, "sorry");
    hu_persona_delta_free(A(), out, n);
    hu_graph_close(g, A());
}

static void observer_proposes_dont_say_as_vocab_avoid(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);
    size_t observed = 0;
    hu_persona_observe_user_correction(g, "u1", 2, "telegram", 8, "dont say literally", 18,
                                        1700000000000LL, &observed);
    HU_ASSERT_EQ(observed, 1);
    HU_ASSERT_EQ(count_pending(g, HU_PERSONA_DELTA_VOCAB_AVOID), 1);
    hu_graph_close(g, A());
}

/* --- "keep it short" --- */

static void observer_proposes_keep_it_short_as_length(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);
    size_t observed = 0;
    hu_persona_observe_user_correction(g, "u1", 2, "slack", 5, "keep it short", 13,
                                        1700000000000LL, &observed);
    HU_ASSERT_EQ(observed, 1);
    HU_ASSERT_EQ(count_pending(g, HU_PERSONA_DELTA_LENGTH), 1);

    hu_persona_delta_t *out = NULL;
    size_t n = 0;
    hu_persona_delta_list(g, A(), "u1", 2, HU_DELTA_STATUS_PENDING, 16, &out, &n);
    HU_ASSERT_EQ(n, 1);
    HU_ASSERT_STR_EQ(out[0].value, "prefer-short");
    hu_persona_delta_free(A(), out, n);
    hu_graph_close(g, A());
}

/* --- boundary --- */

static void observer_proposes_dont_talk_about_as_boundary(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);
    size_t observed = 0;
    hu_persona_observe_user_correction(g, "u1", 2, "slack", 5,
                                        "don't talk about politics please", 32, 1700000000000LL,
                                        &observed);
    HU_ASSERT_EQ(observed, 1);
    HU_ASSERT_EQ(count_pending(g, HU_PERSONA_DELTA_BOUNDARY), 1);

    hu_persona_delta_t *out = NULL;
    size_t n = 0;
    hu_persona_delta_list(g, A(), "u1", 2, HU_DELTA_STATUS_PENDING, 16, &out, &n);
    HU_ASSERT_EQ(n, 1);
    HU_ASSERT_STR_EQ(out[0].value, "politics");
    hu_persona_delta_free(A(), out, n);
    hu_graph_close(g, A());
}

/* --- ADVERSARIAL: false-positive resistance --- */

static void observer_skips_third_party_narration(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);
    size_t observed = 0;
    /* "she said be more X" should NOT match -- "be" is preceded by 'd' from "said". */
    hu_persona_observe_user_correction(
        g, "u1", 2, "slack", 5,
        "she said be more formal but I disagree", strlen("she said be more formal but I disagree"),
        1700000000000LL, &observed);
    HU_ASSERT_EQ(observed, 0);
    hu_graph_close(g, A());
}

static void observer_skips_messages_without_anchor_words(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);
    size_t observed = 0;
    hu_persona_observe_user_correction(g, "u1", 2, "slack", 5,
                                        "hello, what's the weather like in tokyo?", 40,
                                        1700000000000LL, &observed);
    HU_ASSERT_EQ(observed, 0);
    hu_graph_close(g, A());
}

static void observer_handles_empty_and_null_inputs(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);
    size_t observed = 0;
    HU_ASSERT_EQ(hu_persona_observe_user_correction(g, "u1", 2, "slack", 5, "", 0, 0, &observed),
                 HU_OK);
    HU_ASSERT_EQ(observed, 0);
    HU_ASSERT_EQ(hu_persona_observe_user_correction(g, "u1", 2, "slack", 5, NULL, 0, 0, &observed),
                 HU_OK);
    HU_ASSERT_EQ(observed, 0);
    /* NULL graph: silent no-op (lets callers wire unconditionally). */
    HU_ASSERT_EQ(hu_persona_observe_user_correction(NULL, "u1", 2, "slack", 5, "be more brief",
                                                     13, 0, &observed),
                 HU_OK);
    HU_ASSERT_EQ(observed, 0);
    /* Empty contact_id: no-op (propose layer would reject). */
    HU_ASSERT_EQ(hu_persona_observe_user_correction(g, "", 0, "slack", 5, "be more brief", 13, 0,
                                                     &observed),
                 HU_OK);
    HU_ASSERT_EQ(observed, 0);
    hu_graph_close(g, A());
}

static void observer_detects_multiple_patterns_in_one_message(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);
    size_t observed = 0;
    hu_persona_observe_user_correction(
        g, "u1", 2, "slack", 5,
        "be more concise. stop saying actually. don't talk about religion.",
        strlen("be more concise. stop saying actually. don't talk about religion."),
        1700000000000LL, &observed);
    HU_ASSERT_EQ(observed, 3);
    HU_ASSERT_EQ(count_pending(g, HU_PERSONA_DELTA_LENGTH), 1);
    HU_ASSERT_EQ(count_pending(g, HU_PERSONA_DELTA_VOCAB_AVOID), 1);
    HU_ASSERT_EQ(count_pending(g, HU_PERSONA_DELTA_BOUNDARY), 1);
    hu_graph_close(g, A());
}

/* --- E2E: producer -> evolver loop --- */

static void observer_e2e_three_corroborations_apply_via_evolver(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);

    /* Three identical user corrections within an hour -> evolver should apply. */
    size_t observed = 0;
    for (int i = 0; i < 3; i++) {
        hu_persona_observe_user_correction(g, "u1", 2, "slack", 5, "be more brief", 13,
                                            1700000000000LL + i * 60000, &observed);
    }
    HU_ASSERT_EQ(observed, 1); /* last call's count -- tracks per-call only */

    hu_persona_evolver_config_t cfg = hu_persona_evolver_default_config();
    cfg.now_ms = 1700000000000LL + 5 * 60000;
    /* Default rate_limit_per_hour = 10; 3 proposals from one source is fine. */
    hu_persona_evolver_report_t r;
    HU_ASSERT_EQ(hu_persona_evolver_run(g, "u1", 2, &cfg, &r), HU_OK);
    /* 0.7 confidence + 3 corroborations => apply per the W5 thresholds. */
    HU_ASSERT(r.applied >= 1);
    hu_graph_close(g, A());
}

/* --- ADVERSARIAL: rate-limit kicks in if a malicious channel floods --- */

static void observer_flood_from_one_channel_is_rate_limited(void) {
    hu_graph_t *g = NULL;
    open_graph(&g);

    /* Same source ("user-explicit-correction"), 30 proposals in an hour. The
     * evolver's rate_limit_per_hour=10 should quarantine the excess. */
    size_t observed = 0;
    for (int i = 0; i < 30; i++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "stop saying word%d", i);
        hu_persona_observe_user_correction(g, "u1", 2, "slack", 5, msg, strlen(msg),
                                            1700000000000LL + i * 1000LL, &observed);
    }

    hu_persona_evolver_config_t cfg = hu_persona_evolver_default_config();
    cfg.now_ms = 1700000000000LL + 60000;
    hu_persona_evolver_report_t r;
    HU_ASSERT_EQ(hu_persona_evolver_run(g, "u1", 2, &cfg, &r), HU_OK);
    HU_ASSERT(r.quarantined >= 1);
    hu_graph_close(g, A());
}

#endif /* HU_ENABLE_SQLITE */

void run_persona_delta_observer_tests(void) {
    HU_TEST_SUITE("Persona delta observer (FIX 9)");
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(observer_proposes_be_more_concise_as_length);
    HU_RUN_TEST(observer_proposes_be_more_formal_as_formality);
    HU_RUN_TEST(observer_proposes_be_less_wordy_as_length);
    HU_RUN_TEST(observer_proposes_be_more_warm_as_tone);
    HU_RUN_TEST(observer_proposes_stop_saying_as_vocab_avoid);
    HU_RUN_TEST(observer_proposes_dont_say_as_vocab_avoid);
    HU_RUN_TEST(observer_proposes_keep_it_short_as_length);
    HU_RUN_TEST(observer_proposes_dont_talk_about_as_boundary);
    HU_RUN_TEST(observer_skips_third_party_narration);
    HU_RUN_TEST(observer_skips_messages_without_anchor_words);
    HU_RUN_TEST(observer_handles_empty_and_null_inputs);
    HU_RUN_TEST(observer_detects_multiple_patterns_in_one_message);
    HU_RUN_TEST(observer_e2e_three_corroborations_apply_via_evolver);
    HU_RUN_TEST(observer_flood_from_one_channel_is_rate_limited);
#endif
}
