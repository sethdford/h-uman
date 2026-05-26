/* tests/test_init_proposer_compose.c — T1 of the M3 dispatch unification
 * spec (docs/plans/2026-05-26-m3-dispatch-unification/). Pins the pure
 * compose-inputs USER-message builder so daemon_proactive's rich-context
 * shape renders cleanly through init_proposer's propose-or-skip prompt.
 *
 * The builder is a pure function over (inputs, now_unix, last_inbound,
 * out, out_cap). No agent dependency, no I/O. */

#include "human/agent/init_proposer.h"
#include "test_framework.h"
#include <stdbool.h>
#include <string.h>

/* ── 1. Empty inputs → only the header + final question ──────────────── */

static void compose_empty_inputs_emits_header_and_question(void) {
    hu_proactive_compose_inputs_t inputs;
    memset(&inputs, 0, sizeof(inputs));
    char buf[1024] = {0};
    size_t n = hu_init_proposer_build_propose_user_message_ex(&inputs, /*now=*/1779840000,
                                                              /*last_inbound=*/0, buf, sizeof(buf));
    HU_ASSERT(n > 0);
    /* Header always present. */
    HU_ASSERT(strstr(buf, "Context as of unix=1779840000") != NULL);
    HU_ASSERT(strstr(buf, "last_inbound=0") != NULL);
    /* Final question line. */
    HU_ASSERT(strstr(buf, "Should h-uman send Seth a message right now?") != NULL);
    /* NO content labels — all fields empty. */
    HU_ASSERT_NULL(strstr(buf, "--- memory ---"));
    HU_ASSERT_NULL(strstr(buf, "--- weather ---"));
    HU_ASSERT_NULL(strstr(buf, "--- calendar ---"));
    HU_ASSERT_NULL(strstr(buf, "--- feeds ---"));
    HU_ASSERT_NULL(strstr(buf, "--- contact ---"));
    HU_ASSERT_NULL(strstr(buf, "--- channel ---"));
}

/* ── 2. All sources populated → all labels + bodies appear ──────────── */

static void compose_all_fields_present_renders_all_labels(void) {
    hu_proactive_compose_inputs_t inputs;
    memset(&inputs, 0, sizeof(inputs));
    inputs.contact_id = "alice@example.com";
    inputs.contact_id_len = strlen(inputs.contact_id);
    inputs.channel_name = "imessage";
    inputs.channel_name_len = strlen(inputs.channel_name);
    inputs.memory_context = "alice mentioned she's stressed about work";
    inputs.memory_context_len = strlen(inputs.memory_context);
    inputs.weather_context = "60F drizzling rain";
    inputs.weather_context_len = strlen(inputs.weather_context);
    inputs.calendar_context = "team standup at 10am";
    inputs.calendar_context_len = strlen(inputs.calendar_context);
    inputs.feeds_context = "industry layoffs trending";
    inputs.feeds_context_len = strlen(inputs.feeds_context);

    char buf[2048] = {0};
    size_t n = hu_init_proposer_build_propose_user_message_ex(&inputs, /*now=*/1779840000,
                                                              /*last_inbound=*/1779830000, buf,
                                                              sizeof(buf));
    HU_ASSERT(n > 0);
    /* Identity block. */
    HU_ASSERT(strstr(buf, "--- channel ---\nimessage") != NULL);
    HU_ASSERT(strstr(buf, "--- contact ---\nalice@example.com") != NULL);
    /* All four content fragments. */
    HU_ASSERT(strstr(buf, "--- memory ---\nalice mentioned she's stressed about work") != NULL);
    HU_ASSERT(strstr(buf, "--- weather ---\n60F drizzling rain") != NULL);
    HU_ASSERT(strstr(buf, "--- calendar ---\nteam standup at 10am") != NULL);
    HU_ASSERT(strstr(buf, "--- feeds ---\nindustry layoffs trending") != NULL);
    /* Header + question always. */
    HU_ASSERT(strstr(buf, "last_inbound=1779830000") != NULL);
    HU_ASSERT(strstr(buf, "Should h-uman send Seth a message right now?") != NULL);
}

/* ── 3. Partial population → only present sources rendered ─────────── */

static void compose_partial_fields_renders_only_present_labels(void) {
    /* Memory + calendar populated; weather + feeds absent. */
    hu_proactive_compose_inputs_t inputs;
    memset(&inputs, 0, sizeof(inputs));
    inputs.memory_context = "MEM";
    inputs.memory_context_len = 3;
    inputs.calendar_context = "CAL";
    inputs.calendar_context_len = 3;

    char buf[1024] = {0};
    (void)hu_init_proposer_build_propose_user_message_ex(&inputs, 0, 0, buf, sizeof(buf));
    HU_ASSERT(strstr(buf, "--- memory ---") != NULL);
    HU_ASSERT(strstr(buf, "--- calendar ---") != NULL);
    HU_ASSERT_NULL(strstr(buf, "--- weather ---"));
    HU_ASSERT_NULL(strstr(buf, "--- feeds ---"));
}

/* ── 4. Safety predicate (Risk 3 mitigation) — rejection path ───────── */

static bool always_unsafe(const char *content, size_t len) {
    (void)content;
    (void)len;
    return false;
}

static void compose_unsafe_memory_filtered_when_predicate_rejects(void) {
    /* Caller passes a predicate that returns false → memory is silently
     * dropped from the rendered prompt. Weather/calendar/feeds DO NOT
     * route through the predicate (per design.md: only memory). */
    hu_proactive_compose_inputs_t inputs;
    memset(&inputs, 0, sizeof(inputs));
    inputs.memory_context = "I am lonely tonight";
    inputs.memory_context_len = strlen(inputs.memory_context);
    inputs.weather_context = "sunny";
    inputs.weather_context_len = 5;
    inputs.content_is_safe = always_unsafe;

    char buf[1024] = {0};
    (void)hu_init_proposer_build_propose_user_message_ex(&inputs, 0, 0, buf, sizeof(buf));
    HU_ASSERT_NULL(strstr(buf, "I am lonely tonight"));
    HU_ASSERT_NULL(strstr(buf, "--- memory ---"));
    /* Weather still rendered — predicate only gates memory. */
    HU_ASSERT(strstr(buf, "--- weather ---\nsunny") != NULL);
}

/* ── 5. Safety predicate — accept path ──────────────────────────────── */

static bool always_safe(const char *content, size_t len) {
    (void)content;
    (void)len;
    return true;
}

static void compose_safe_memory_passes_through_when_predicate_accepts(void) {
    hu_proactive_compose_inputs_t inputs;
    memset(&inputs, 0, sizeof(inputs));
    inputs.memory_context = "alice's birthday is friday";
    inputs.memory_context_len = strlen(inputs.memory_context);
    inputs.content_is_safe = always_safe;

    char buf[1024] = {0};
    (void)hu_init_proposer_build_propose_user_message_ex(&inputs, 0, 0, buf, sizeof(buf));
    HU_ASSERT(strstr(buf, "--- memory ---\nalice's birthday is friday") != NULL);
}

/* ── 6. NULL safety + zero-cap ──────────────────────────────────────── */

static void compose_returns_zero_for_null_or_zero_cap(void) {
    char buf[64];
    /* NULL inputs. */
    HU_ASSERT_EQ(hu_init_proposer_build_propose_user_message_ex(NULL, 0, 0, buf, sizeof(buf)),
                 (size_t)0);
    /* NULL out. */
    hu_proactive_compose_inputs_t inputs;
    memset(&inputs, 0, sizeof(inputs));
    HU_ASSERT_EQ(hu_init_proposer_build_propose_user_message_ex(&inputs, 0, 0, NULL, 16),
                 (size_t)0);
    /* Zero cap. */
    HU_ASSERT_EQ(hu_init_proposer_build_propose_user_message_ex(&inputs, 0, 0, buf, 0), (size_t)0);
}

/* ── 7. Tiny buffer truncates safely ────────────────────────────────── */

static void compose_truncates_safely_on_small_buffer(void) {
    /* 24-byte buffer fits the header start but nothing else. Builder
     * must NUL-terminate and not write past the buffer. Use a guard
     * arena to catch overrun. */
    char wrap[64];
    memset(wrap, 0xAA, sizeof(wrap));
    char *buf = wrap + 16;
    const size_t cap = 24;
    hu_proactive_compose_inputs_t inputs;
    memset(&inputs, 0, sizeof(inputs));
    inputs.memory_context = "long enough to not fit";
    inputs.memory_context_len = 22;
    size_t n = hu_init_proposer_build_propose_user_message_ex(&inputs, 1779840000, 0, buf, cap);
    HU_ASSERT(n < cap);
    HU_ASSERT_EQ(buf[n], '\0');
    /* Guard bytes around the buffer must be untouched. */
    for (size_t i = 0; i < 16; i++) {
        HU_ASSERT_EQ((unsigned char)wrap[i], (unsigned char)0xAA);
        HU_ASSERT_EQ((unsigned char)wrap[16 + cap + i], (unsigned char)0xAA);
    }
}

/* ── 8. Headers ordered before content ──────────────────────────────── */

static void compose_identity_block_renders_before_content(void) {
    /* Pin the order so the LLM sees WHO first, then context. Saves the
     * model from getting confused by content that looks like it could
     * apply to any contact. */
    hu_proactive_compose_inputs_t inputs;
    memset(&inputs, 0, sizeof(inputs));
    inputs.channel_name = "imessage";
    inputs.channel_name_len = 8;
    inputs.contact_id = "alice";
    inputs.contact_id_len = 5;
    inputs.memory_context = "BODY";
    inputs.memory_context_len = 4;

    char buf[1024] = {0};
    (void)hu_init_proposer_build_propose_user_message_ex(&inputs, 0, 0, buf, sizeof(buf));
    char *channel_pos = strstr(buf, "--- channel ---");
    char *contact_pos = strstr(buf, "--- contact ---");
    char *memory_pos = strstr(buf, "--- memory ---");
    HU_ASSERT_NOT_NULL(channel_pos);
    HU_ASSERT_NOT_NULL(contact_pos);
    HU_ASSERT_NOT_NULL(memory_pos);
    HU_ASSERT(channel_pos < contact_pos);
    HU_ASSERT(contact_pos < memory_pos);
}

/* ── M3 Dispatch T2 — pure verdict-mapping helper tests ─────────────── */

#include "human/agent/response_guard.h"

static void guard_outcome_ok_keeps_fired(void) {
    HU_ASSERT_EQ((int)hu_init_proposer_evaluate_guard_outcome((int)HU_GUARD_OK),
                 (int)HU_INIT_RESULT_FIRED);
}

static void guard_outcome_rewrote_keeps_fired(void) {
    /* REWROTE is "caller must swap to rewrite" — still a send. */
    HU_ASSERT_EQ((int)hu_init_proposer_evaluate_guard_outcome((int)HU_GUARD_REWROTE),
                 (int)HU_INIT_RESULT_FIRED);
}

static void guard_outcome_reject_downgrades_to_guard_reject(void) {
    HU_ASSERT_EQ((int)hu_init_proposer_evaluate_guard_outcome((int)HU_GUARD_REJECT),
                 (int)HU_INIT_RESULT_GUARD_REJECT);
}

static void guard_outcome_unknown_defaults_to_guard_reject(void) {
    /* Defensive: any future outcome we don't recognize fails CLOSED — a
     * draft never slips past on an enum value we don't yet know how to
     * interpret. Bug-pinning test for the default branch. */
    HU_ASSERT_EQ((int)hu_init_proposer_evaluate_guard_outcome(99),
                 (int)HU_INIT_RESULT_GUARD_REJECT);
    HU_ASSERT_EQ((int)hu_init_proposer_evaluate_guard_outcome(-1),
                 (int)HU_INIT_RESULT_GUARD_REJECT);
}

/* End-to-end pure-helper proof: the Jordan production failure
 * ("tbh morning. you awake yet?") would be caught by the verdict
 * helper if passed through response_guard. Pins the integration
 * shape without requiring a mock provider. */
static void guard_outcome_jordan_draft_would_be_rejected(void) {
    const char *jordan = "tbh morning. you awake yet?";
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t outcome = HU_GUARD_OK;
    hu_guard_report_t report;
    memset(&report, 0, sizeof(report));
    HU_ASSERT_EQ(
        hu_response_guard_check(&alloc, jordan, strlen(jordan), &out, &out_len, &outcome, &report),
        HU_OK);
    HU_ASSERT_EQ((int)outcome, (int)HU_GUARD_REJECT);
    HU_ASSERT_TRUE(report.detected_naked_discourse_opener);
    /* The verdict helper maps that REJECT to GUARD_REJECT — confirming
     * the wire from response_guard back to the tick result enum. */
    HU_ASSERT_EQ((int)hu_init_proposer_evaluate_guard_outcome((int)outcome),
                 (int)HU_INIT_RESULT_GUARD_REJECT);
}

/* ── M3 Dispatch T3 — use_unified_dispatch config flag ──────────────── */

#include "human/config.h"

/* T3 unit tests: structural assertions on the config struct + the
 * struct's place in hu_config_t. The full parse-from-JSON path is
 * tested at integration level (hu_config_load) — see
 * tests/test_config_extended.c family; calling hu_config_parse_json
 * on a memset'd config segfaults because it expects allocator setup
 * from hu_config_load's set_defaults path.
 *
 * What these tests pin:
 *   - The struct has a use_unified_dispatch field of type bool.
 *   - The field can be read + written through hu_config_t.
 *   - Setting it to true/false sticks (no silent overrides). */

static void config_use_unified_dispatch_field_round_trips(void) {
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    /* Default zero-init must be false (matches set_defaults intent). */
    HU_ASSERT_FALSE(cfg.proactive_throttle.use_unified_dispatch);
    cfg.proactive_throttle.use_unified_dispatch = true;
    HU_ASSERT_TRUE(cfg.proactive_throttle.use_unified_dispatch);
    cfg.proactive_throttle.use_unified_dispatch = false;
    HU_ASSERT_FALSE(cfg.proactive_throttle.use_unified_dispatch);
}

static void config_use_unified_dispatch_independent_of_other_throttle_fields(void) {
    /* Pin that setting the flag does not perturb the other throttle
     * fields. Operators flipping just use_unified_dispatch should not
     * disturb their per_contact_daily_max or enabled state. */
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.proactive_throttle.enabled = true;
    cfg.proactive_throttle.per_contact_daily_max = 5;
    cfg.proactive_throttle.use_unified_dispatch = true;
    HU_ASSERT_TRUE(cfg.proactive_throttle.enabled);
    HU_ASSERT_EQ(cfg.proactive_throttle.per_contact_daily_max, 5);
    HU_ASSERT_TRUE(cfg.proactive_throttle.use_unified_dispatch);
}

/* ── M3 Dispatch T8 — legacy path deletion audit ─────────────────────── */

#include <stdio.h>

/* T8 audit: grep src/daemon.c's proactive-send block for hu_agent_turn
 * and assert ZERO matches. Pins that the legacy composition path is
 * structurally gone, not just bypassed via the (now-removed) else
 * branch. Future refactors that accidentally re-add hu_agent_turn to
 * the proactive block trip this test. */
static void t8_daemon_proactive_block_has_zero_hu_agent_turn_calls(void) {
    FILE *f = fopen("src/daemon.c", "r");
    if (!f) {
        /* Working directory may not be repo root in CI test harness;
         * try the abs path as a fallback. */
        f = fopen("/Users/sethford/Projects/h-uman/src/daemon.c", "r");
    }
    HU_ASSERT_NOT_NULL(f);

    /* Scan lines, tracking whether we're inside the proactive-send
     * block. The block is bounded by the comment "M3 Dispatch T8" on
     * entry (planted in T8's edit) and the line `agent->proactive_turn
     * = false;` on exit. Any hu_agent_turn( seen between those markers
     * is a regression. */
    char line[2048];
    bool in_block = false;
    size_t hits = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!in_block && strstr(line, "M3 Dispatch T8") != NULL)
            in_block = true;
        else if (in_block && strstr(line, "agent->proactive_turn = false") != NULL)
            in_block = false;
        else if (in_block && strstr(line, "hu_agent_turn(") != NULL)
            hits++;
    }
    fclose(f);
    HU_ASSERT_EQ(hits, (size_t)0);
}

/* ── M3 Dispatch T4 — daemon-side compose-inputs wire smoke ─────────── */

/* T4 wires daemon.c's proactive-send block to construct a
 * hu_proactive_compose_inputs_t and call hu_init_proposer_tick_with_provider_ex
 * when cfg->proactive_throttle.use_unified_dispatch is true. The full
 * daemon integration test (mock provider + mock channel + assert send
 * was called with the unified draft) is gated on a daemon test
 * harness that doesn't exist yet; for now we pin the API contract the
 * daemon relies on: the _ex function accepts the same inputs shape
 * daemon.c constructs, and returns SKIP under HU_IS_TEST (no real
 * provider call). */
static void t4_tick_with_provider_ex_accepts_daemon_shape_inputs(void) {
    hu_proactive_compose_inputs_t inputs;
    memset(&inputs, 0, sizeof(inputs));
    inputs.contact_id = "alice@example.com";
    inputs.contact_id_len = strlen(inputs.contact_id);
    inputs.channel_name = "imessage";
    inputs.channel_name_len = strlen(inputs.channel_name);
    /* daemon.c populates content_is_safe = hu_daemon_callback_content_is_safe;
     * test uses NULL since the predicate isn't visible here without
     * the daemon-side include. The _ex path must accept NULL cleanly. */
    inputs.content_is_safe = NULL;

    int64_t last_tick = 0;
    uint64_t tick_id = 0;
    hu_init_proposer_result_t result = HU_INIT_RESULT_FIRED; /* sentinel */
    hu_init_decision_t decision;
    memset(&decision, 0, sizeof(decision));

    /* Match daemon.c's call shape with a stub cfg (matches what
     * &config->initiative would look like with defaults). NULL provider
     * + NULL alloc mean no LLM call — _ex returns SKIP after governor
     * gates pass. Pins that the daemon's argument shape doesn't crash
     * the unified path. */
    hu_initiative_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.enabled = true;
    cfg.tick_interval_sec = 1800;
    cfg.confidence_threshold = 0.85;
    cfg.per_contact_min_seconds = 600;

    hu_error_t err = hu_init_proposer_tick_with_provider_ex(
        &cfg, /*ar_cfg=*/NULL, /*tz_offset_seconds=*/0, /*budget=*/NULL,
        /*agent=*/NULL, /*provider=*/NULL, /*alloc=*/NULL, &inputs, /*last_inbound_unix=*/0,
        /*now_unix=*/1779840000, &last_tick, &tick_id, &result, &decision);
    HU_ASSERT_EQ(err, HU_OK);
    /* No provider → SKIP after governor gates pass. */
    HU_ASSERT_EQ((int)result, (int)HU_INIT_RESULT_SKIP);
}

void run_init_proposer_compose_tests(void);
void run_init_proposer_compose_tests(void) {
    HU_TEST_SUITE("init_proposer_compose");
    HU_RUN_TEST(compose_empty_inputs_emits_header_and_question);
    HU_RUN_TEST(compose_all_fields_present_renders_all_labels);
    HU_RUN_TEST(compose_partial_fields_renders_only_present_labels);
    HU_RUN_TEST(compose_unsafe_memory_filtered_when_predicate_rejects);
    HU_RUN_TEST(compose_safe_memory_passes_through_when_predicate_accepts);
    HU_RUN_TEST(compose_returns_zero_for_null_or_zero_cap);
    HU_RUN_TEST(compose_truncates_safely_on_small_buffer);
    HU_RUN_TEST(compose_identity_block_renders_before_content);
    /* T2 — pure verdict-mapping helper. */
    HU_RUN_TEST(guard_outcome_ok_keeps_fired);
    HU_RUN_TEST(guard_outcome_rewrote_keeps_fired);
    HU_RUN_TEST(guard_outcome_reject_downgrades_to_guard_reject);
    HU_RUN_TEST(guard_outcome_unknown_defaults_to_guard_reject);
    HU_RUN_TEST(guard_outcome_jordan_draft_would_be_rejected);
    /* T3 — use_unified_dispatch config flag. */
    HU_RUN_TEST(config_use_unified_dispatch_field_round_trips);
    HU_RUN_TEST(config_use_unified_dispatch_independent_of_other_throttle_fields);
    /* T4 — daemon-side wire smoke (compose-inputs + tick_with_provider_ex
     * accept the rich-context shape daemon now passes). */
    HU_RUN_TEST(t4_tick_with_provider_ex_accepts_daemon_shape_inputs);
    /* T8 — audit that legacy hu_agent_turn is structurally removed from
     * the proactive-send block. */
    HU_RUN_TEST(t8_daemon_proactive_block_has_zero_hu_agent_turn_calls);
}
