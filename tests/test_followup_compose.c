/* Tests for persona-composed follow-up nudges (SOTA roadmap #3).
 *
 * The contract these pin, in one line: at LIVE a compose failure sends
 * NOTHING — it must never substitute the hardcoded template this module
 * exists to remove. That is the assertion most likely to be "simplified"
 * back into a fallback by a future reader who finds a NULL return
 * surprising, so it is tested from several directions. */
#include "human/agent/followup_compose.h"
#include "human/follow_up.h"
#include "test_framework.h"

#include <string.h>

/* ── Gate ──────────────────────────────────────────────────────────────── */

static void followup_compose_defaults_to_off(void) {
    hu_followup_compose_set_mode_for_test(-1); /* read env */
    /* Default OFF is the activation contract
     * (.claude/rules/feature-gate-requires-measurement.md). A test env with
     * HU_FOLLOWUP_COMPOSE set would legitimately differ, so assert the
     * fail-closed property that holds either way: never LIVE by accident. */
    HU_ASSERT_TRUE(hu_followup_compose_mode() != HU_GATE_LIVE);
}

static void followup_compose_mode_override_round_trips(void) {
    hu_followup_compose_set_mode_for_test((int)HU_GATE_SHADOW);
    HU_ASSERT_EQ((int)hu_followup_compose_mode(), (int)HU_GATE_SHADOW);
    hu_followup_compose_set_mode_for_test((int)HU_GATE_LIVE);
    HU_ASSERT_EQ((int)hu_followup_compose_mode(), (int)HU_GATE_LIVE);
    hu_followup_compose_set_mode_for_test(-1);
}

/* ── pick(): the activation policy ─────────────────────────────────────── */

static void pick_off_returns_template(void) {
    const char *r = hu_followup_compose_pick(HU_GATE_OFF, HU_OK, "composed line", "tmpl");
    HU_ASSERT_NOT_NULL(r);
    HU_ASSERT_STR_EQ(r, "tmpl");
}

static void pick_shadow_returns_template_even_when_compose_succeeded(void) {
    /* SHADOW must not change what is sent — that is what makes it a safe
     * measurement mode. A shadow that emitted the composed text would be
     * LIVE wearing a different name. */
    const char *r = hu_followup_compose_pick(HU_GATE_SHADOW, HU_OK, "composed line", "tmpl");
    HU_ASSERT_NOT_NULL(r);
    HU_ASSERT_STR_EQ(r, "tmpl");
}

static void pick_live_returns_composed(void) {
    const char *r = hu_followup_compose_pick(HU_GATE_LIVE, HU_OK, "composed line", "tmpl");
    HU_ASSERT_NOT_NULL(r);
    HU_ASSERT_STR_EQ(r, "composed line");
}

/* THE contract. If this ever returns the template, the module's whole reason
 * for existing is gone: the hardcoded string reaches a real contact exactly
 * when the composer is unhealthy. */
static void pick_live_compose_failed_sends_nothing_not_template(void) {
    const char *r = hu_followup_compose_pick(HU_GATE_LIVE, HU_ERR_PROVIDER_RESPONSE, "", "tmpl");
    HU_ASSERT_TRUE(r == NULL);
}

static void pick_live_empty_composed_sends_nothing(void) {
    /* HU_OK with empty text is the shape a lenient composer could produce;
     * treat it as failure, not as "send an empty message". */
    HU_ASSERT_TRUE(hu_followup_compose_pick(HU_GATE_LIVE, HU_OK, "", "tmpl") == NULL);
    HU_ASSERT_TRUE(hu_followup_compose_pick(HU_GATE_LIVE, HU_OK, NULL, "tmpl") == NULL);
}

/* ── directive(): pure builder ─────────────────────────────────────────── */

static void directive_none_warmth_refuses(void) {
    char buf[384];
    HU_ASSERT_EQ(hu_followup_compose_directive("+15555550123", HU_FOLLOWUP_WARMTH_NONE, 3,
                                               "imessage", buf, sizeof(buf)),
                 (size_t)0);
    HU_ASSERT_EQ(buf[0], '\0');
}

static void directive_includes_situation_and_output_rule(void) {
    char buf[384];
    size_t n = hu_followup_compose_directive("+15555550123", HU_FOLLOWUP_WARMTH_CLOSE, 5,
                                             "imessage", buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_EQ(strlen(buf), n);
    /* The situational facts the model needs. */
    HU_ASSERT_NOT_NULL(strstr(buf, "+15555550123"));
    HU_ASSERT_NOT_NULL(strstr(buf, "imessage"));
    HU_ASSERT_NOT_NULL(strstr(buf, "5 hours"));
    /* The output-format rule. Without it the model narrates instead of
     * emitting a bare line, and finalize rejects every reply. */
    HU_ASSERT_NOT_NULL(strstr(buf, "Output only the message text"));
    /* The anti-template clause — the reason this beats a hardcoded string. */
    HU_ASSERT_NOT_NULL(strstr(buf, "stock check-in phrasing"));
}

static void directive_singular_hour_reads_naturally(void) {
    char buf[384];
    HU_ASSERT_TRUE(hu_followup_compose_directive("+15555550123", HU_FOLLOWUP_WARMTH_FRIEND, 1,
                                                 "imessage", buf, sizeof(buf)) > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "1 hour ago"));
    HU_ASSERT_TRUE(strstr(buf, "1 hours") == NULL);
}

static void directive_distinguishes_warmth_tiers(void) {
    char close_buf[384], friend_buf[384];
    HU_ASSERT_TRUE(hu_followup_compose_directive("+1", HU_FOLLOWUP_WARMTH_CLOSE, 2, "imessage",
                                                 close_buf, sizeof(close_buf)) > 0);
    HU_ASSERT_TRUE(hu_followup_compose_directive("+1", HU_FOLLOWUP_WARMTH_FRIEND, 2, "imessage",
                                                 friend_buf, sizeof(friend_buf)) > 0);
    /* Same situation, different relationship => different prompt. If these
     * ever collapse, warmth has stopped shaping the nudge. */
    HU_ASSERT_TRUE(strcmp(close_buf, friend_buf) != 0);
}

static void directive_truncation_refuses_rather_than_clipping(void) {
    /* A clipped directive silently loses its trailing output-format rule, so
     * a too-small buffer must produce nothing at all. */
    char tiny[24];
    HU_ASSERT_EQ(hu_followup_compose_directive("+15555550123", HU_FOLLOWUP_WARMTH_CLOSE, 3,
                                               "imessage", tiny, sizeof(tiny)),
                 (size_t)0);
    HU_ASSERT_EQ(tiny[0], '\0');
}

static void directive_rejects_bad_input(void) {
    char buf[384];
    HU_ASSERT_EQ(hu_followup_compose_directive(NULL, HU_FOLLOWUP_WARMTH_CLOSE, 1, "imessage", buf,
                                               sizeof(buf)),
                 (size_t)0);
    HU_ASSERT_EQ(hu_followup_compose_directive("", HU_FOLLOWUP_WARMTH_CLOSE, 1, "imessage", buf,
                                               sizeof(buf)),
                 (size_t)0);
    HU_ASSERT_EQ(
        hu_followup_compose_directive("+1", HU_FOLLOWUP_WARMTH_CLOSE, 1, NULL, buf, sizeof(buf)),
        (size_t)0);
    HU_ASSERT_EQ(hu_followup_compose_directive("+1", HU_FOLLOWUP_WARMTH_CLOSE, 1, "imessage", NULL,
                                               sizeof(buf)),
                 (size_t)0);
}

/* ── compose_text(): finalize via the injected seam ────────────────────── */

static const char *s_canned = NULL;
static hu_error_t s_canned_err = HU_OK;

static hu_error_t canned_llm(void *ctx, const char *system, size_t system_len,
                             const char *directive, size_t directive_len, char *out, size_t cap) {
    (void)ctx;
    (void)system;
    (void)system_len;
    (void)directive;
    (void)directive_len;
    if (s_canned_err != HU_OK)
        return s_canned_err;
    if (!s_canned)
        return HU_ERR_PROVIDER_RESPONSE;
    size_t n = strlen(s_canned);
    if (n + 1 > cap)
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(out, s_canned, n + 1);
    return HU_OK;
}

static hu_error_t compose_canned(const char *reply, char *out, size_t cap) {
    s_canned = reply;
    s_canned_err = HU_OK;
    hu_followup_compose_set_llm_for_test(canned_llm, NULL);
    hu_error_t err = hu_followup_compose_text(NULL, NULL, NULL, "imessage", "d", out, cap);
    hu_followup_compose_set_llm_for_test(NULL, NULL);
    return err;
}

static void compose_accepts_a_clean_one_liner(void) {
    char out[HU_FOLLOWUP_COMPOSE_MAX];
    HU_ASSERT_EQ((int)compose_canned("still thinking on this one?", out, sizeof(out)), (int)HU_OK);
    HU_ASSERT_STR_EQ(out, "still thinking on this one?");
}

static void compose_trims_whitespace_and_one_quote_pair(void) {
    char out[HU_FOLLOWUP_COMPOSE_MAX];
    HU_ASSERT_EQ((int)compose_canned("  \"any word on this?\"  ", out, sizeof(out)), (int)HU_OK);
    HU_ASSERT_STR_EQ(out, "any word on this?");
}

static void compose_rejects_multiline_deliberation(void) {
    /* The candidate-list shape: a model that deliberates instead of answering.
     * Sending line one of a thinking-out-loud reply is the 2026-07-12
     * deliberation-leak incident. */
    char out[HU_FOLLOWUP_COMPOSE_MAX];
    HU_ASSERT_TRUE(compose_canned("Option 1: hey\nOption 2: yo", out, sizeof(out)) != HU_OK);
    HU_ASSERT_EQ(out[0], '\0');
}

static void compose_rejects_empty_and_whitespace_only(void) {
    char out[HU_FOLLOWUP_COMPOSE_MAX];
    HU_ASSERT_TRUE(compose_canned("", out, sizeof(out)) != HU_OK);
    HU_ASSERT_EQ(out[0], '\0');
    HU_ASSERT_TRUE(compose_canned("   \n  ", out, sizeof(out)) != HU_OK);
    HU_ASSERT_EQ(out[0], '\0');
}

static void compose_rejects_overlong_rather_than_truncating(void) {
    /* Truncating would ship half a sentence to a real person. */
    char big[HU_FOLLOWUP_COMPOSE_MAX + 64];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    char out[HU_FOLLOWUP_COMPOSE_MAX];
    HU_ASSERT_TRUE(compose_canned(big, out, sizeof(out)) != HU_OK);
    HU_ASSERT_EQ(out[0], '\0');
}

static void compose_propagates_provider_error(void) {
    char out[HU_FOLLOWUP_COMPOSE_MAX];
    s_canned = "unused";
    s_canned_err = HU_ERR_PROVIDER_RESPONSE;
    hu_followup_compose_set_llm_for_test(canned_llm, NULL);
    hu_error_t err = hu_followup_compose_text(NULL, NULL, NULL, "imessage", "d", out, sizeof(out));
    hu_followup_compose_set_llm_for_test(NULL, NULL);
    s_canned_err = HU_OK;
    HU_ASSERT_EQ((int)err, (int)HU_ERR_PROVIDER_RESPONSE);
    HU_ASSERT_EQ(out[0], '\0');
}

static void compose_rejects_empty_directive(void) {
    char out[HU_FOLLOWUP_COMPOSE_MAX];
    HU_ASSERT_TRUE(hu_followup_compose_text(NULL, NULL, NULL, "imessage", "", out, sizeof(out)) !=
                   HU_OK);
    HU_ASSERT_TRUE(hu_followup_compose_text(NULL, NULL, NULL, "imessage", NULL, out, sizeof(out)) !=
                   HU_OK);
}

/* ── End-to-end policy: a rejected compose must not send the template ──── */

static void rejected_compose_at_live_yields_no_send(void) {
    /* Compose a reply that finalize rejects, then run the real pick() on the
     * real error — the two halves the daemon wires together. Proves the
     * skip decision end-to-end without a provider or a daemon. */
    char out[HU_FOLLOWUP_COMPOSE_MAX];
    hu_error_t err = compose_canned("line one\nline two", out, sizeof(out));
    HU_ASSERT_TRUE(err != HU_OK);
    const char *tmpl = hu_followup_template_for_warmth(HU_FOLLOWUP_WARMTH_CLOSE);
    HU_ASSERT_NOT_NULL(tmpl); /* the hardcoded string still exists on this path */
    HU_ASSERT_TRUE(hu_followup_compose_pick(HU_GATE_LIVE, err, out, tmpl) == NULL);
}

void run_followup_compose_tests(void) {
    HU_TEST_SUITE("followup_compose");
    HU_RUN_TEST(followup_compose_defaults_to_off);
    HU_RUN_TEST(followup_compose_mode_override_round_trips);
    HU_RUN_TEST(pick_off_returns_template);
    HU_RUN_TEST(pick_shadow_returns_template_even_when_compose_succeeded);
    HU_RUN_TEST(pick_live_returns_composed);
    HU_RUN_TEST(pick_live_compose_failed_sends_nothing_not_template);
    HU_RUN_TEST(pick_live_empty_composed_sends_nothing);
    HU_RUN_TEST(directive_none_warmth_refuses);
    HU_RUN_TEST(directive_includes_situation_and_output_rule);
    HU_RUN_TEST(directive_singular_hour_reads_naturally);
    HU_RUN_TEST(directive_distinguishes_warmth_tiers);
    HU_RUN_TEST(directive_truncation_refuses_rather_than_clipping);
    HU_RUN_TEST(directive_rejects_bad_input);
    HU_RUN_TEST(compose_accepts_a_clean_one_liner);
    HU_RUN_TEST(compose_trims_whitespace_and_one_quote_pair);
    HU_RUN_TEST(compose_rejects_multiline_deliberation);
    HU_RUN_TEST(compose_rejects_empty_and_whitespace_only);
    HU_RUN_TEST(compose_rejects_overlong_rather_than_truncating);
    HU_RUN_TEST(compose_propagates_provider_error);
    HU_RUN_TEST(compose_rejects_empty_directive);
    HU_RUN_TEST(rejected_compose_at_live_yields_no_send);
}
