/* tests/test_init_proposer.c — covers AC-1 / AC-6 / AC-7 of T1.
 *
 * The init_proposer tick function is a pure predicate over (config, ar_cfg,
 * budget, recency, now). All four T1 scenarios are exercised without any
 * real network or daemon spin-up. Per
 * .claude/rules/security-predicate-extraction.md — the tick is structured
 * as a pure decision so the truth table can be locked here. */

#include "human/agent/governor.h"
#include "human/agent/init_proposer.h"
#include "human/autoresponder.h"
#include "human/config.h"
#include "test_framework.h"
#include <string.h>

/* T1 default config: disabled (AC-7 kill switch is off by default). */
static void make_default_cfg(hu_initiative_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->enabled = false;
    cfg->tick_interval_sec = 1800;
    cfg->confidence_threshold = 0.85;
    cfg->per_contact_min_seconds = 600;
    /* propose_model intentionally left NULL — tick handles that. */
}

static void make_enabled_cfg(hu_initiative_config_t *cfg) {
    make_default_cfg(cfg);
    cfg->enabled = true;
}

static void test_disabled_config_returns_skip_no_state_change(void) {
    hu_init_proposer_reset_warn_guards_for_test();
    hu_initiative_config_t cfg;
    make_default_cfg(&cfg);
    int64_t last_tick = 0;
    uint64_t tick_id = 0;
    hu_init_proposer_result_t result = HU_INIT_RESULT_FIRED; /* sentinel */
    HU_ASSERT_EQ(
        hu_init_proposer_tick(&cfg, NULL, 0, NULL, 0, 1779700000, &last_tick, &tick_id, &result),
        HU_OK);
    HU_ASSERT_EQ((int)result, (int)HU_INIT_RESULT_SKIP);
    /* Disabled path MUST NOT advance the watermark — operators rely on
     * last_tick being stale to detect a flipped-off subsystem. */
    HU_ASSERT_EQ(last_tick, (int64_t)0);
    HU_ASSERT_EQ(tick_id, (uint64_t)0);
}

static void test_enabled_all_clear_returns_skip_advances_state(void) {
    hu_init_proposer_reset_warn_guards_for_test();
    hu_initiative_config_t cfg;
    make_enabled_cfg(&cfg);
    int64_t last_tick = 0;
    uint64_t tick_id = 0;
    hu_init_proposer_result_t result = HU_INIT_RESULT_FIRED;
    int64_t now = 1779700000;
    HU_ASSERT_EQ(hu_init_proposer_tick(&cfg, NULL, 0, NULL, 0, now, &last_tick, &tick_id, &result),
                 HU_OK);
    HU_ASSERT_EQ((int)result, (int)HU_INIT_RESULT_SKIP);
    HU_ASSERT_EQ(last_tick, now);
    HU_ASSERT_EQ(tick_id, (uint64_t)1);
}

static void test_interval_gate_blocks_back_to_back_ticks(void) {
    hu_init_proposer_reset_warn_guards_for_test();
    hu_initiative_config_t cfg;
    make_enabled_cfg(&cfg);
    int64_t last_tick = 1779700000;
    uint64_t tick_id = 5;
    hu_init_proposer_result_t result = HU_INIT_RESULT_FIRED;
    /* 60s later — well inside the 1800s interval. */
    HU_ASSERT_EQ(
        hu_init_proposer_tick(&cfg, NULL, 0, NULL, 0, 1779700060, &last_tick, &tick_id, &result),
        HU_OK);
    HU_ASSERT_EQ((int)result, (int)HU_INIT_RESULT_GATED_INTERVAL);
    /* tick_id MUST NOT advance when interval-gated — otherwise every poll
     * inflates the counter and SKIP-rate metrics become meaningless. */
    HU_ASSERT_EQ(tick_id, (uint64_t)5);
    HU_ASSERT_EQ(last_tick, (int64_t)1779700000);
}

static void test_per_contact_recency_gates_when_seth_texted_recently(void) {
    hu_init_proposer_reset_warn_guards_for_test();
    hu_initiative_config_t cfg;
    make_enabled_cfg(&cfg);
    int64_t now = 1779700000;
    int64_t last_inbound = now - 60; /* Seth texted 60s ago, floor is 600s */
    int64_t last_tick = 0;
    uint64_t tick_id = 0;
    hu_init_proposer_result_t result = HU_INIT_RESULT_FIRED;
    HU_ASSERT_EQ(hu_init_proposer_tick(&cfg, NULL, 0, NULL, last_inbound, now, &last_tick, &tick_id,
                                       &result),
                 HU_OK);
    HU_ASSERT_EQ((int)result, (int)HU_INIT_RESULT_GATED_RECENCY);
    /* Recency-gated ticks DO advance the watermark — otherwise a
     * fast-talking Seth would starve the proposer indefinitely. */
    HU_ASSERT_EQ(last_tick, now);
    HU_ASSERT_EQ(tick_id, (uint64_t)1);
}

static void test_null_args_return_invalid(void) {
    hu_initiative_config_t cfg;
    make_enabled_cfg(&cfg);
    int64_t last_tick = 0;
    uint64_t tick_id = 0;
    hu_init_proposer_result_t result = HU_INIT_RESULT_FIRED;
    /* cfg NULL */
    HU_ASSERT_EQ(hu_init_proposer_tick(NULL, NULL, 0, NULL, 0, 0, &last_tick, &tick_id, &result),
                 HU_ERR_INVALID_ARGUMENT);
    /* watermark NULL */
    HU_ASSERT_EQ(hu_init_proposer_tick(&cfg, NULL, 0, NULL, 0, 0, NULL, &tick_id, &result),
                 HU_ERR_INVALID_ARGUMENT);
    /* tick_id NULL */
    HU_ASSERT_EQ(hu_init_proposer_tick(&cfg, NULL, 0, NULL, 0, 0, &last_tick, NULL, &result),
                 HU_ERR_INVALID_ARGUMENT);
    /* out_result NULL */
    HU_ASSERT_EQ(hu_init_proposer_tick(&cfg, NULL, 0, NULL, 0, 0, &last_tick, &tick_id, NULL),
                 HU_ERR_INVALID_ARGUMENT);
}

/* ──────────────────────────────────────────────────────────────────────────
 * T2 — context bundle + summary formatting tests.
 *
 * The format function is a pure predicate, so we exercise it directly with
 * synthetic bundles. Avoids spinning a stub hu_agent_t. */

static void test_format_summary_empty_bundle_writes_zero_counts(void) {
    hu_init_context_bundle_t bundle;
    memset(&bundle, 0, sizeof(bundle));
    char buf[256] = {0};
    size_t n = hu_init_proposer_format_context_summary(&bundle, buf, sizeof(buf));
    HU_ASSERT(n > 0);
    /* All fields zero; leader should report fields=0 total=0. */
    HU_ASSERT(strstr(buf, "fields=0") != NULL);
    HU_ASSERT(strstr(buf, "total=0") != NULL);
    /* Every field must appear with =0 so operators can see the slot exists
     * (telemetric value: an unpopulated field is a known unwired source,
     * not a mystery). */
    HU_ASSERT(strstr(buf, "persona=0") != NULL);
    HU_ASSERT(strstr(buf, "conversation=0") != NULL);
    HU_ASSERT(strstr(buf, "memory=0") != NULL);
}

static void test_format_summary_populated_bundle_counts_correctly(void) {
    hu_init_context_bundle_t bundle;
    memset(&bundle, 0, sizeof(bundle));
    bundle.content[HU_INIT_FIELD_CONTACT] = "alice@example.com";
    bundle.bytes[HU_INIT_FIELD_CONTACT] = 17;
    bundle.content[HU_INIT_FIELD_CONVERSATION] = "hello there";
    bundle.bytes[HU_INIT_FIELD_CONVERSATION] = 11;
    bundle.total_bytes = 28;
    char buf[256] = {0};
    size_t n = hu_init_proposer_format_context_summary(&bundle, buf, sizeof(buf));
    HU_ASSERT(n > 0);
    HU_ASSERT(strstr(buf, "fields=2") != NULL);
    HU_ASSERT(strstr(buf, "total=28") != NULL);
    HU_ASSERT(strstr(buf, "contact=17") != NULL);
    HU_ASSERT(strstr(buf, "conversation=11") != NULL);
}

static void test_format_summary_null_args_safe(void) {
    char buf[64] = {'x', 0};
    /* NULL bundle returns 0 and writes empty string (defensively). */
    HU_ASSERT_EQ(hu_init_proposer_format_context_summary(NULL, buf, sizeof(buf)), (size_t)0);
    HU_ASSERT_EQ(buf[0], '\0');
    /* NULL out returns 0. */
    hu_init_context_bundle_t bundle;
    memset(&bundle, 0, sizeof(bundle));
    HU_ASSERT_EQ(hu_init_proposer_format_context_summary(&bundle, NULL, 16), (size_t)0);
    /* Zero cap returns 0. */
    HU_ASSERT_EQ(hu_init_proposer_format_context_summary(&bundle, buf, 0), (size_t)0);
}

static void test_format_summary_tiny_buffer_truncates_safely(void) {
    hu_init_context_bundle_t bundle;
    memset(&bundle, 0, sizeof(bundle));
    bundle.bytes[HU_INIT_FIELD_CONTACT] = 17;
    bundle.total_bytes = 17;
    char tiny[8] = {0};
    size_t n = hu_init_proposer_format_context_summary(&bundle, tiny, sizeof(tiny));
    /* Must NUL-terminate even when truncated; must not write past cap. */
    HU_ASSERT(n < sizeof(tiny));
    HU_ASSERT_EQ(tiny[sizeof(tiny) - 1], '\0');
}

static void test_assemble_context_null_out_returns_invalid(void) {
    HU_ASSERT_EQ(hu_init_proposer_assemble_context(NULL, 0, 0, NULL), HU_ERR_INVALID_ARGUMENT);
}

static void test_assemble_context_null_agent_returns_empty_bundle(void) {
    hu_init_context_bundle_t bundle;
    memset(&bundle, 0x55, sizeof(bundle)); /* sentinel garbage */
    HU_ASSERT_EQ(hu_init_proposer_assemble_context(NULL, 1779700000, 1779699000, &bundle), HU_OK);
    /* All field bytes zero, but metadata preserved. */
    HU_ASSERT_EQ(bundle.total_bytes, (size_t)0);
    HU_ASSERT_EQ(bundle.now_unix, (int64_t)1779700000);
    HU_ASSERT_EQ(bundle.last_inbound_unix, (int64_t)1779699000);
    for (size_t i = 0; i < (size_t)HU_INIT_FIELD_COUNT; i++) {
        HU_ASSERT_EQ(bundle.bytes[i], (size_t)0);
        HU_ASSERT(bundle.content[i] == NULL);
    }
}

/* ──────────────────────────────────────────────────────────────────────────
 * T3 — Prompt building, response parsing, decision evaluation.
 *
 * All three predicates are pure — testable without HTTP, providers, or
 * agents. Integration test for hu_init_proposer_tick_with_provider is
 * limited to the NULL-provider passthrough case (HU_IS_TEST forbids
 * real network calls per .claude/rules/testing.md). */

static void test_build_prompt_includes_role_and_json_contract(void) {
    hu_init_context_bundle_t bundle;
    memset(&bundle, 0, sizeof(bundle));
    char sys[1536] = {0};
    char usr[4096] = {0};
    size_t n = hu_init_proposer_build_propose_prompt(&bundle, sys, sizeof(sys), usr, sizeof(usr));
    HU_ASSERT(n > 0);
    /* System prompt must establish role + JSON contract. */
    HU_ASSERT(strstr(sys, "Initiative Layer") != NULL);
    HU_ASSERT(strstr(sys, "should_propose") != NULL);
    HU_ASSERT(strstr(sys, "confidence") != NULL);
    HU_ASSERT(strstr(sys, "JSON") != NULL);
    /* User message must end with the question line so the LLM knows to decide. */
    HU_ASSERT(strstr(usr, "Should h-uman send Seth a message right now?") != NULL);
}

static void test_build_prompt_skips_empty_fields_includes_populated_ones(void) {
    hu_init_context_bundle_t bundle;
    memset(&bundle, 0, sizeof(bundle));
    bundle.now_unix = 1779700000;
    bundle.last_inbound_unix = 1779699000;
    bundle.content[HU_INIT_FIELD_CONTACT] = "alice@example.com";
    bundle.bytes[HU_INIT_FIELD_CONTACT] = 17;
    bundle.content[HU_INIT_FIELD_CONVERSATION] = "hello world from Alice";
    bundle.bytes[HU_INIT_FIELD_CONVERSATION] = 22;
    char sys[1536] = {0};
    char usr[4096] = {0};
    hu_init_proposer_build_propose_prompt(&bundle, sys, sizeof(sys), usr, sizeof(usr));
    /* Populated fields appear with their label headers + content. */
    HU_ASSERT(strstr(usr, "--- contact ---") != NULL);
    HU_ASSERT(strstr(usr, "alice@example.com") != NULL);
    HU_ASSERT(strstr(usr, "--- conversation ---") != NULL);
    HU_ASSERT(strstr(usr, "hello world from Alice") != NULL);
    /* Empty fields are silently skipped (no header for them). */
    HU_ASSERT(strstr(usr, "--- memory ---") == NULL);
    HU_ASSERT(strstr(usr, "--- persona ---") == NULL);
    /* Metadata line carries the timestamps. */
    HU_ASSERT(strstr(usr, "1779700000") != NULL);
}

static void test_build_prompt_tiny_user_cap_truncates_safely(void) {
    hu_init_context_bundle_t bundle;
    memset(&bundle, 0, sizeof(bundle));
    bundle.content[HU_INIT_FIELD_CONVERSATION] = "this is a long conversation that won't fit";
    bundle.bytes[HU_INIT_FIELD_CONVERSATION] = 42;
    char sys[256] = {0};
    char tiny_usr[32] = {0};
    size_t n = hu_init_proposer_build_propose_prompt(&bundle, sys, sizeof(sys), tiny_usr,
                                                     sizeof(tiny_usr));
    HU_ASSERT(n < sizeof(tiny_usr));
    HU_ASSERT_EQ(tiny_usr[sizeof(tiny_usr) - 1], '\0');
}

static void test_parse_response_valid_propose_with_high_confidence(void) {
    const char *json =
        "{\"should_propose\": true, \"confidence\": 0.92, \"draft\": \"Hey, got a sec?\"}";
    hu_init_decision_t d;
    HU_ASSERT_EQ(hu_init_proposer_parse_response(json, strlen(json), &d), HU_OK);
    HU_ASSERT(d.should_propose);
    HU_ASSERT(d.confidence > 0.91 && d.confidence < 0.93);
    HU_ASSERT_STR_EQ(d.draft, "Hey, got a sec?");
    HU_ASSERT(d.draft_len == 15);
    HU_ASSERT_EQ(d.skip_reason_len, (size_t)0);
}

static void test_parse_response_extracts_json_from_surrounding_prose(void) {
    /* LLMs frequently violate "return ONLY JSON" — defensive parser must cope. */
    const char *response =
        "Here is my decision:\n"
        "{\"should_propose\": false, \"confidence\": 0.3, \"reason\": \"thin context\"}\n"
        "Hope this helps!";
    hu_init_decision_t d;
    HU_ASSERT_EQ(hu_init_proposer_parse_response(response, strlen(response), &d), HU_OK);
    HU_ASSERT(!d.should_propose);
    HU_ASSERT_STR_EQ(d.skip_reason, "thin context");
}

static void test_parse_response_clamps_out_of_range_confidence(void) {
    const char *json = "{\"should_propose\": true, \"confidence\": 1.5, \"draft\": \"hi\"}";
    hu_init_decision_t d;
    HU_ASSERT_EQ(hu_init_proposer_parse_response(json, strlen(json), &d), HU_OK);
    HU_ASSERT(d.confidence <= 1.0);

    const char *neg = "{\"should_propose\": true, \"confidence\": -0.5, \"draft\": \"hi\"}";
    HU_ASSERT_EQ(hu_init_proposer_parse_response(neg, strlen(neg), &d), HU_OK);
    HU_ASSERT(d.confidence >= 0.0);
}

static void test_parse_response_missing_fields_default_to_skip(void) {
    /* Empty object — both fields default to safe values. */
    const char *json = "{}";
    hu_init_decision_t d;
    HU_ASSERT_EQ(hu_init_proposer_parse_response(json, strlen(json), &d), HU_OK);
    HU_ASSERT(!d.should_propose);
    HU_ASSERT_EQ(d.confidence, 0.0);
}

static void test_parse_response_malformed_returns_parse_error(void) {
    hu_init_decision_t d;
    /* No JSON object at all. */
    HU_ASSERT_EQ(hu_init_proposer_parse_response("not json at all", 15, &d), HU_ERR_JSON_PARSE);
    /* Empty input. */
    HU_ASSERT_EQ(hu_init_proposer_parse_response("", 0, &d), HU_ERR_JSON_PARSE);
}

static void test_parse_response_null_args_return_invalid(void) {
    hu_init_decision_t d;
    HU_ASSERT_EQ(hu_init_proposer_parse_response(NULL, 0, &d), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_init_proposer_parse_response("{}", 2, NULL), HU_ERR_INVALID_ARGUMENT);
}

static void test_parse_response_strips_markdown_code_fence(void) {
    /* gemini-3.1-flash-lite (and many other small models) often wraps the
     * JSON in a markdown code fence despite the system prompt asking for
     * raw output. The parser must strip ```json fences before brace-
     * matching. Reason: 2026-05-25 service-loop-sprint58 logs showed
     * `err=42 preview=```json {...` failures every tick. */
    const char *fenced = "```json\n{\"should_propose\":false,\"reason\":\"too quiet\"}\n```";
    hu_init_decision_t d;
    HU_ASSERT_EQ(hu_init_proposer_parse_response(fenced, strlen(fenced), &d), HU_OK);
    HU_ASSERT_EQ((int)d.should_propose, 0);
}

static void test_parse_response_strips_bare_triple_backtick_fence(void) {
    /* Some models use a bare ``` (no language tag). Strip that too. */
    const char *fenced = "```\n{\"should_propose\":true,\"confidence\":0.9,\"draft\":\"hi\"}\n```";
    hu_init_decision_t d;
    HU_ASSERT_EQ(hu_init_proposer_parse_response(fenced, strlen(fenced), &d), HU_OK);
    HU_ASSERT_EQ((int)d.should_propose, 1);
    HU_ASSERT(d.confidence > 0.85);
}

static void test_parse_response_partial_skip_fallback_recovers_truncated_false(void) {
    /* 2026-05-26 issue-sweep — when gemini-3.5-flash truncates mid-response
     * but the model clearly said "should_propose:false", the partial-skip
     * fallback should extract the SKIP intent rather than fail-parse. This
     * reduces operator log noise from `err=42` lines that have no actionable
     * meaning (the safe default IS skip). */
    const char *truncated_fenced = "```json\n{\n  \"should_propose\": false,\n  \"confidence\":";
    hu_init_decision_t d;
    HU_ASSERT_EQ(hu_init_proposer_parse_response(truncated_fenced, strlen(truncated_fenced), &d),
                 HU_OK);
    HU_ASSERT_EQ((int)d.should_propose, 0);
    HU_ASSERT_EQ((int)(d.confidence * 100), 0); /* truncated → 0 confidence */
    /* Reason populated so a debug log line can show the partial-parse path. */
    HU_ASSERT(d.skip_reason_len > 0);
}

static void test_parse_response_partial_true_still_fails_parse(void) {
    /* Partial "should_propose":true should NOT trigger the fallback —
     * without confidence + draft, a FIRED decision would be malformed,
     * and the safe default is to surface the parse error so the operator
     * sees a real failure (vs a silent "model intended yes but couldn't
     * follow through"). Per the fallback's comment. */
    const char *partial_true = "```json\n{\n  \"should_propose\": true,\n  \"confidence\":";
    hu_init_decision_t d;
    HU_ASSERT_EQ(hu_init_proposer_parse_response(partial_true, strlen(partial_true), &d),
                 HU_ERR_JSON_PARSE);
}

static void test_parse_response_truncates_oversize_draft_to_buffer(void) {
    /* Draft longer than HU_INIT_DRAFT_MAX must be truncated, not overflow. */
    char json[HU_INIT_DRAFT_MAX + 128];
    memcpy(json, "{\"should_propose\":true,\"confidence\":0.9,\"draft\":\"", 49);
    size_t pos = 49;
    for (size_t i = 0; i < HU_INIT_DRAFT_MAX + 20; i++)
        json[pos++] = 'x';
    memcpy(json + pos, "\"}", 2);
    pos += 2;
    hu_init_decision_t d;
    HU_ASSERT_EQ(hu_init_proposer_parse_response(json, pos, &d), HU_OK);
    /* draft_len capped at buffer cap (excluding NUL). */
    HU_ASSERT(d.draft_len < HU_INIT_DRAFT_MAX);
    HU_ASSERT_EQ(d.draft[d.draft_len], '\0');
}

static void test_evaluate_high_confidence_propose_returns_fired(void) {
    hu_init_decision_t d = {0};
    d.should_propose = true;
    d.confidence = 0.9;
    strcpy(d.draft, "hello");
    d.draft_len = 5;
    HU_ASSERT_EQ((int)hu_init_proposer_evaluate_decision(&d, 0.85), (int)HU_INIT_RESULT_FIRED);
}

static void test_evaluate_low_confidence_propose_returns_low_confidence(void) {
    hu_init_decision_t d = {0};
    d.should_propose = true;
    d.confidence = 0.7;
    strcpy(d.draft, "hello");
    d.draft_len = 5;
    HU_ASSERT_EQ((int)hu_init_proposer_evaluate_decision(&d, 0.85),
                 (int)HU_INIT_RESULT_LOW_CONFIDENCE);
}

static void test_evaluate_should_not_propose_returns_negative(void) {
    hu_init_decision_t d = {0};
    d.should_propose = false;
    d.confidence = 0.99;
    HU_ASSERT_EQ((int)hu_init_proposer_evaluate_decision(&d, 0.85), (int)HU_INIT_RESULT_NEGATIVE);
}

static void test_evaluate_propose_with_empty_draft_treated_as_low_confidence(void) {
    /* Malformed "propose but no draft" — safer to SKIP than send empty text. */
    hu_init_decision_t d = {0};
    d.should_propose = true;
    d.confidence = 0.95;
    d.draft_len = 0;
    HU_ASSERT_EQ((int)hu_init_proposer_evaluate_decision(&d, 0.85),
                 (int)HU_INIT_RESULT_LOW_CONFIDENCE);
}

static void test_evaluate_null_decision_returns_llm_error(void) {
    HU_ASSERT_EQ((int)hu_init_proposer_evaluate_decision(NULL, 0.85),
                 (int)HU_INIT_RESULT_LLM_ERROR);
}

static void test_tick_with_provider_null_provider_behaves_like_t1_tick(void) {
    hu_init_proposer_reset_warn_guards_for_test();
    hu_initiative_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.enabled = true;
    cfg.tick_interval_sec = 1800;
    cfg.confidence_threshold = 0.85;
    cfg.per_contact_min_seconds = 600;

    int64_t last_tick = 0;
    uint64_t tick_id = 0;
    hu_init_proposer_result_t result = HU_INIT_RESULT_FIRED;
    hu_init_decision_t decision;
    memset(&decision, 0, sizeof(decision));

    /* NULL provider + alloc → T3 falls through to T1 governor path; SKIP. */
    hu_error_t err = hu_init_proposer_tick_with_provider(
        &cfg, /*ar_cfg=*/NULL, /*tz=*/0, /*budget=*/NULL, /*agent=*/NULL, /*provider=*/NULL,
        /*alloc=*/NULL, /*last_inbound=*/0, /*now=*/1779700000, &last_tick, &tick_id, &result,
        &decision);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ((int)result, (int)HU_INIT_RESULT_SKIP);
    HU_ASSERT_EQ(tick_id, (uint64_t)1);
    HU_ASSERT_EQ(last_tick, (int64_t)1779700000);
}

/* ── Sprint 41 follow-up #2 — single-source-of-truth arbiter ────────── */

static void arbiter_skip_returns_skip_for_all_null_clear_args(void) {
    /* NULL cfg + NULL ar_cfg + NULL budget + last_inbound=0 → all gates
     * are operator-disabled or N/A. Must return SKIP (caller proceeds). */
    hu_init_proposer_result_t r = hu_init_proposer_governor_check_only(
        /*cfg=*/NULL, /*ar_cfg=*/NULL, /*tz=*/0, /*budget=*/NULL, /*last_inbound=*/0,
        /*now=*/1779700000);
    HU_ASSERT_EQ((int)r, (int)HU_INIT_RESULT_SKIP);
}

static void arbiter_skip_returns_gated_recency_when_user_texted_recently(void) {
    /* Default recency floor 600s. last_inbound 100s ago → GATED. */
    hu_init_proposer_result_t r = hu_init_proposer_governor_check_only(
        /*cfg=*/NULL, /*ar_cfg=*/NULL, /*tz=*/0, /*budget=*/NULL,
        /*last_inbound=*/1779700000 - 100, /*now=*/1779700000);
    HU_ASSERT_EQ((int)r, (int)HU_INIT_RESULT_GATED_RECENCY);
}

static void arbiter_skip_returns_skip_when_user_texted_long_ago(void) {
    /* last_inbound 700s ago → past the 600s floor → SKIP. */
    hu_init_proposer_result_t r = hu_init_proposer_governor_check_only(
        /*cfg=*/NULL, /*ar_cfg=*/NULL, /*tz=*/0, /*budget=*/NULL,
        /*last_inbound=*/1779700000 - 700, /*now=*/1779700000);
    HU_ASSERT_EQ((int)r, (int)HU_INIT_RESULT_SKIP);
}

void run_init_proposer_tests(void);
void run_init_proposer_tests(void) {
    HU_TEST_SUITE("init_proposer");
    HU_RUN_TEST(arbiter_skip_returns_skip_for_all_null_clear_args);
    HU_RUN_TEST(arbiter_skip_returns_gated_recency_when_user_texted_recently);
    HU_RUN_TEST(arbiter_skip_returns_skip_when_user_texted_long_ago);
    HU_RUN_TEST(test_disabled_config_returns_skip_no_state_change);
    HU_RUN_TEST(test_enabled_all_clear_returns_skip_advances_state);
    HU_RUN_TEST(test_interval_gate_blocks_back_to_back_ticks);
    HU_RUN_TEST(test_per_contact_recency_gates_when_seth_texted_recently);
    HU_RUN_TEST(test_null_args_return_invalid);
    HU_RUN_TEST(test_format_summary_empty_bundle_writes_zero_counts);
    HU_RUN_TEST(test_format_summary_populated_bundle_counts_correctly);
    HU_RUN_TEST(test_format_summary_null_args_safe);
    HU_RUN_TEST(test_format_summary_tiny_buffer_truncates_safely);
    HU_RUN_TEST(test_assemble_context_null_out_returns_invalid);
    HU_RUN_TEST(test_assemble_context_null_agent_returns_empty_bundle);
    /* T3 — prompt build / parse / evaluate */
    HU_RUN_TEST(test_build_prompt_includes_role_and_json_contract);
    HU_RUN_TEST(test_build_prompt_skips_empty_fields_includes_populated_ones);
    HU_RUN_TEST(test_build_prompt_tiny_user_cap_truncates_safely);
    HU_RUN_TEST(test_parse_response_valid_propose_with_high_confidence);
    HU_RUN_TEST(test_parse_response_extracts_json_from_surrounding_prose);
    HU_RUN_TEST(test_parse_response_clamps_out_of_range_confidence);
    HU_RUN_TEST(test_parse_response_missing_fields_default_to_skip);
    HU_RUN_TEST(test_parse_response_malformed_returns_parse_error);
    HU_RUN_TEST(test_parse_response_null_args_return_invalid);
    HU_RUN_TEST(test_parse_response_strips_markdown_code_fence);
    HU_RUN_TEST(test_parse_response_strips_bare_triple_backtick_fence);
    HU_RUN_TEST(test_parse_response_partial_skip_fallback_recovers_truncated_false);
    HU_RUN_TEST(test_parse_response_partial_true_still_fails_parse);
    HU_RUN_TEST(test_parse_response_truncates_oversize_draft_to_buffer);
    HU_RUN_TEST(test_evaluate_high_confidence_propose_returns_fired);
    HU_RUN_TEST(test_evaluate_low_confidence_propose_returns_low_confidence);
    HU_RUN_TEST(test_evaluate_should_not_propose_returns_negative);
    HU_RUN_TEST(test_evaluate_propose_with_empty_draft_treated_as_low_confidence);
    HU_RUN_TEST(test_evaluate_null_decision_returns_llm_error);
    HU_RUN_TEST(test_tick_with_provider_null_provider_behaves_like_t1_tick);
}
