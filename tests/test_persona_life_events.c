/* test_persona_life_events.c — pins the life-event lifecycle contract
 * (src/persona/life_events.c).
 *
 * THE BUG THIS PINS (human blind-A/B cycle 4, 2026-07-27, n=40): 2 of 9
 * detections were the model asserting a WRONG STATE for an in-progress life
 * event. Asked "or are you still moving" it said "done moving all settled in
 * now" while the real Seth was mid-move. The persona stated the transition as
 * completed steady-state and carried no as-of or in-progress representation,
 * so the model completed it plausibly and confidently.
 *
 * The contract these tests pin: an event whose expected date has passed with
 * no confirmed resolution renders as UNKNOWN with explicit do-not-assert
 * guidance, and NEVER as completed.
 *
 * Non-vacuity: every "guidance is present" assertion is paired with a case
 * where the SAME builder must omit it. A test that only ever saw the stale
 * case would pass against a builder that stamped the guidance unconditionally,
 * and would therefore pin nothing (see .claude/rules/tests-that-pin-bugs.md).
 */
#include "human/persona.h"
#include "human/persona/life_events.h"
#include "test_framework.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Fixture dates for the cycle-4 regression scenario. */
#define AS_OF_STR    "2026-07-20" /* when "pending" was last confirmed */
#define EXPECTED_STR "2026-07-23" /* "Moving the 23rd" */
#define NOW_STR      "2026-07-27" /* the day of the blind-A/B read */

/* The exact status token the renderer emits; asserting on ABSENCE of the
 * completed form is the load-bearing check. A keyword search for "done" or
 * "completed" could not do this — the guidance sentence contains both. */
#define STATUS_COMPLETED "[status: completed]"
#define STATUS_UNKNOWN   "[status: unknown]"
#define GUIDANCE_MARKER  "Do not say it is done"

static hu_life_event_t make_move_event(hu_life_event_state_t state, const char *as_of,
                                       const char *expected) {
    hu_life_event_t ev;
    memset(&ev, 0, sizeof(ev));
    snprintf(ev.description, sizeof(ev.description), "moving to the waterfront place in st pete");
    ev.state = state;
    ev.as_of = as_of ? hu_life_event_parse_date(as_of, strlen(as_of)) : 0;
    ev.expected_date = expected ? hu_life_event_parse_date(expected, strlen(expected)) : 0;
    return ev;
}

/* ── State parsing ────────────────────────────────────────────────────── */

/* Exact-token matching. "not_completed" must NOT resolve to COMPLETED — under
 * substring matching it would, and under the project's usual word-boundary fix
 * it ALSO would, because `_` is a word boundary. Only exact matching is safe
 * for this closed vocabulary. */
static void life_event_state_parse_is_exact_not_substring(void) {
    HU_ASSERT_EQ((int)hu_life_event_state_from_string("completed", 9),
                 (int)HU_LIFE_EVENT_STATE_COMPLETED);
    HU_ASSERT_EQ((int)hu_life_event_state_from_string("not_completed", 13),
                 (int)HU_LIFE_EVENT_STATE_UNKNOWN);
    HU_ASSERT_EQ((int)hu_life_event_state_from_string("uncompleted", 11),
                 (int)HU_LIFE_EVENT_STATE_UNKNOWN);
    HU_ASSERT_EQ((int)hu_life_event_state_from_string("in_progress", 11),
                 (int)HU_LIFE_EVENT_STATE_IN_PROGRESS);
    HU_ASSERT_EQ((int)hu_life_event_state_from_string("PENDING", 7),
                 (int)HU_LIFE_EVENT_STATE_PENDING);
    /* Unrecognized input fails toward hedging, never toward a completion. */
    HU_ASSERT_EQ((int)hu_life_event_state_from_string("finishd", 7),
                 (int)HU_LIFE_EVENT_STATE_UNKNOWN);
    HU_ASSERT_EQ((int)hu_life_event_state_from_string(NULL, 0), (int)HU_LIFE_EVENT_STATE_UNKNOWN);
}

/* Date parsing is pure integer arithmetic, so these hold on any host timezone.
 * Asserted as relationships plus a fixed anchor rather than a magic epoch. */
static void life_event_parse_date_is_deterministic(void) {
    HU_ASSERT_EQ(hu_life_event_parse_date("1970-01-01", 10), (int64_t)0);
    int64_t d20 = hu_life_event_parse_date(AS_OF_STR, 10);
    int64_t d23 = hu_life_event_parse_date(EXPECTED_STR, 10);
    int64_t d27 = hu_life_event_parse_date(NOW_STR, 10);
    HU_ASSERT_TRUE(d20 > 0);
    HU_ASSERT_EQ(d23 - d20, (int64_t)(3 * 86400));
    HU_ASSERT_EQ(d27 - d23, (int64_t)(4 * 86400));
    /* Malformed input is "unset" (0), never a guessed date — a misparse would
     * silently shift a staleness verdict. */
    HU_ASSERT_EQ(hu_life_event_parse_date("2026-7-3", 8), (int64_t)0);
    HU_ASSERT_EQ(hu_life_event_parse_date("not-a-date", 10), (int64_t)0);
    HU_ASSERT_EQ(hu_life_event_parse_date("2026-13-01", 10), (int64_t)0);
}

/* ── The pure predicate ───────────────────────────────────────────────── */

/* THE REGRESSION CASE. Pending, due the 23rd, confirmed only on the 20th, read
 * on the 27th. The date passed but nobody confirmed what happened, so the
 * effective state is UNKNOWN — explicitly NOT completed. */
static void life_event_passed_date_unconfirmed_is_unknown_not_completed(void) {
    hu_life_event_t ev = make_move_event(HU_LIFE_EVENT_STATE_PENDING, AS_OF_STR, EXPECTED_STR);
    int64_t now = hu_life_event_parse_date(NOW_STR, 10);

    HU_ASSERT_EQ((int)hu_life_event_effective_state(&ev, now), (int)HU_LIFE_EVENT_STATE_UNKNOWN);
    HU_ASSERT_TRUE(hu_life_event_must_not_assert_completion(&ev, now));
}

/* Pre/post contract on the SAME event: before the expected date the declared
 * state stands; after it, unconfirmed, it demotes. If the predicate ignored
 * time entirely, this fails. */
static void life_event_before_expected_date_keeps_declared_state(void) {
    hu_life_event_t ev = make_move_event(HU_LIFE_EVENT_STATE_PENDING, AS_OF_STR, EXPECTED_STR);
    int64_t before = hu_life_event_parse_date("2026-07-21", 10);
    int64_t after = hu_life_event_parse_date(NOW_STR, 10);

    HU_ASSERT_EQ((int)hu_life_event_effective_state(&ev, before), (int)HU_LIFE_EVENT_STATE_PENDING);
    HU_ASSERT_EQ((int)hu_life_event_effective_state(&ev, after), (int)HU_LIFE_EVENT_STATE_UNKNOWN);
}

/* A state confirmed AFTER the expected date is a real observation, not a stale
 * prediction — "still in progress, checked yesterday" must survive. */
static void life_event_confirmed_after_expected_date_keeps_declared_state(void) {
    hu_life_event_t ev =
        make_move_event(HU_LIFE_EVENT_STATE_IN_PROGRESS, "2026-07-26", EXPECTED_STR);
    int64_t now = hu_life_event_parse_date(NOW_STR, 10);

    HU_ASSERT_EQ((int)hu_life_event_effective_state(&ev, now),
                 (int)HU_LIFE_EVENT_STATE_IN_PROGRESS);
    /* Still no confirmed resolution — in-progress may not be asserted done. */
    HU_ASSERT_TRUE(hu_life_event_must_not_assert_completion(&ev, now));
}

/* Terminal states are observations; time does not un-complete a move. */
static void life_event_completed_stays_completed_and_is_assertable(void) {
    hu_life_event_t ev = make_move_event(HU_LIFE_EVENT_STATE_COMPLETED, AS_OF_STR, EXPECTED_STR);
    int64_t now = hu_life_event_parse_date(NOW_STR, 10);

    HU_ASSERT_EQ((int)hu_life_event_effective_state(&ev, now), (int)HU_LIFE_EVENT_STATE_COMPLETED);
    HU_ASSERT_TRUE(!hu_life_event_must_not_assert_completion(&ev, now));
}

/* An open-ended event has no resolution point to be past. */
static void life_event_open_ended_keeps_declared_state(void) {
    hu_life_event_t ev = make_move_event(HU_LIFE_EVENT_STATE_IN_PROGRESS, AS_OF_STR, NULL);
    int64_t now = hu_life_event_parse_date(NOW_STR, 10);
    HU_ASSERT_EQ(ev.expected_date, (int64_t)0);
    HU_ASSERT_EQ((int)hu_life_event_effective_state(&ev, now),
                 (int)HU_LIFE_EVENT_STATE_IN_PROGRESS);
}

/* ── The directive (THE HEADLINE CONTRACT) ────────────────────────────── */

/* Given an event whose date has passed with no confirmed completion, the built
 * prompt block MUST carry the do-not-assert guidance and MUST NOT claim the
 * event completed. */
static void directive_for_stale_event_forbids_completion_claim(void) {
    hu_life_event_t ev = make_move_event(HU_LIFE_EVENT_STATE_PENDING, AS_OF_STR, EXPECTED_STR);
    int64_t now = hu_life_event_parse_date(NOW_STR, 10);

    char out[1024];
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_life_events_build_directive(&ev, 1, now, out, sizeof(out), &out_len), HU_OK);
    HU_ASSERT_TRUE(out_len > 0);

    /* The event itself is present. */
    HU_ASSERT_TRUE(strstr(out, "moving to the waterfront place") != NULL);
    /* NEVER reported as completed. This is the load-bearing assertion — the one
     * that would have caught "done moving all settled in now" — so it is
     * checked FIRST: the framework stops a test at its first failure, and this
     * is the failure a reader most needs to see. Verified to discriminate by
     * reintroducing the cycle-4 bug (effective state -> COMPLETED) and
     * confirming this test fails. */
    HU_ASSERT_TRUE(strstr(out, STATUS_COMPLETED) == NULL);
    /* It is reported as unknown instead. */
    HU_ASSERT_TRUE(strstr(out, STATUS_UNKNOWN) != NULL);
    /* The do-not-assert guidance is attached. */
    HU_ASSERT_TRUE(strstr(out, GUIDANCE_MARKER) != NULL);
    /* And it offers the alternative move, so the model is not left to invent
     * one — a bare prohibition is what produced the confident guess. */
    HU_ASSERT_TRUE(strstr(out, "hedge or ask") != NULL);
    /* The closing rule IS present here — paired with its absence in
     * directive_for_completed_event_omits_the_guard, this pins that the rule
     * tracks whether anything is actually unresolved. */
    HU_ASSERT_TRUE(strstr(out, "Never upgrade") != NULL);
}

/* NON-VACUITY TWIN of the test above. Same builder, confirmed-completed event:
 * the status IS completed and the guidance is ABSENT. A builder that stamped
 * the guidance unconditionally would pass the stale test and fail this one. */
static void directive_for_completed_event_omits_the_guard(void) {
    hu_life_event_t ev = make_move_event(HU_LIFE_EVENT_STATE_COMPLETED, AS_OF_STR, EXPECTED_STR);
    int64_t now = hu_life_event_parse_date(NOW_STR, 10);

    char out[1024];
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_life_events_build_directive(&ev, 1, now, out, sizeof(out), &out_len), HU_OK);
    HU_ASSERT_TRUE(out_len > 0);

    HU_ASSERT_TRUE(strstr(out, STATUS_COMPLETED) != NULL);
    HU_ASSERT_TRUE(strstr(out, STATUS_UNKNOWN) == NULL);
    HU_ASSERT_TRUE(strstr(out, GUIDANCE_MARKER) == NULL);
    /* The closing "never upgrade to finished" rule is also absent: with nothing
     * unresolved it would be a rule with no referent, spending head budget and
     * inviting the model to hedge facts it should state plainly. */
    HU_ASSERT_TRUE(strstr(out, "Never upgrade") == NULL);
}

/* Mixed set: the guard attaches per-event, not to the whole block. */
static void directive_attaches_guidance_per_event(void) {
    hu_life_event_t evs[2];
    evs[0] = make_move_event(HU_LIFE_EVENT_STATE_COMPLETED, AS_OF_STR, EXPECTED_STR);
    evs[1] = make_move_event(HU_LIFE_EVENT_STATE_PENDING, AS_OF_STR, EXPECTED_STR);
    snprintf(evs[1].description, sizeof(evs[1].description), "last day at the old job");
    int64_t now = hu_life_event_parse_date(NOW_STR, 10);

    char out[2048];
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_life_events_build_directive(evs, 2, now, out, sizeof(out), &out_len), HU_OK);

    /* Both events present, one settled and one hedged. */
    HU_ASSERT_TRUE(strstr(out, STATUS_COMPLETED) != NULL);
    HU_ASSERT_TRUE(strstr(out, STATUS_UNKNOWN) != NULL);
    /* The guidance appears exactly once — attached to the stale event only. */
    const char *first = strstr(out, GUIDANCE_MARKER);
    HU_ASSERT_TRUE(first != NULL);
    HU_ASSERT_TRUE(strstr(first + 1, GUIDANCE_MARKER) == NULL);
    /* And it follows the stale event, not the completed one. */
    HU_ASSERT_TRUE(strstr(out, "last day at the old job") < first);
}

/* Backward compatibility: a persona declaring no life_events emits nothing, so
 * the prompt is byte-identical to today's. */
static void directive_empty_when_no_events(void) {
    char out[64];
    size_t out_len = 123; /* poisoned — the builder must reset it */
    HU_ASSERT_EQ(hu_life_events_build_directive(NULL, 0, 0, out, sizeof(out), &out_len), HU_OK);
    HU_ASSERT_EQ(out_len, (size_t)0);
    HU_ASSERT_EQ(out[0], '\0');

    hu_life_event_t blank;
    memset(&blank, 0, sizeof(blank));
    out_len = 123;
    HU_ASSERT_EQ(hu_life_events_build_directive(&blank, 1, 0, out, sizeof(out), &out_len), HU_OK);
    HU_ASSERT_EQ(out_len, (size_t)0);
}

/* A buffer too small to hold the block truncates safely and stays terminated —
 * the prompt head is budget-capped, so this path is reachable in production. */
static void directive_truncates_without_overflow(void) {
    hu_life_event_t ev = make_move_event(HU_LIFE_EVENT_STATE_PENDING, AS_OF_STR, EXPECTED_STR);
    int64_t now = hu_life_event_parse_date(NOW_STR, 10);

    char out[24];
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_life_events_build_directive(&ev, 1, now, out, sizeof(out), &out_len), HU_OK);
    HU_ASSERT_TRUE(out_len < sizeof(out));
    HU_ASSERT_EQ(out[out_len], '\0');

    HU_ASSERT_EQ(hu_life_events_build_directive(&ev, 1, now, NULL, 0, &out_len),
                 HU_ERR_INVALID_ARGUMENT);
}

/* Ships default OFF per .claude/rules/feature-gate-requires-measurement.md —
 * promotion is gated on the cycle-5 human blind A/B, not on this suite. */
static void life_events_gate_defaults_off(void) {
    HU_ASSERT_EQ((int)hu_life_events_gate(), (int)HU_GATE_OFF);
}

/* ── Persona wiring (the block must actually REACH a built prompt) ────── */

/* A persona declaring life_events parses them, including the lifecycle fields.
 * Defining a renderer that no persona can feed would be dead code. */
static const char *const persona_with_events_json =
    "{\"name\":\"tester\",\"core\":{\"identity\":\"Lives in the new place.\"},"
    "\"life_events\":[{\"description\":\"moving to the new place\","
    "\"state\":\"pending\",\"as_of\":\"" AS_OF_STR "\","
    "\"expected_date\":\"" EXPECTED_STR "\"}]}";

static void persona_life_events_parse_from_json(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p;
    memset(&p, 0, sizeof(p));
    HU_ASSERT_EQ(hu_persona_load_json(&alloc, persona_with_events_json,
                                      strlen(persona_with_events_json), &p),
                 HU_OK);
    HU_ASSERT_EQ(p.life_events_count, (size_t)1);
    HU_ASSERT_TRUE(p.life_events != NULL);
    HU_ASSERT_EQ((int)p.life_events[0].state, (int)HU_LIFE_EVENT_STATE_PENDING);
    HU_ASSERT_EQ(p.life_events[0].as_of, hu_life_event_parse_date(AS_OF_STR, 10));
    HU_ASSERT_EQ(p.life_events[0].expected_date, hu_life_event_parse_date(EXPECTED_STR, 10));
    hu_persona_deinit(&alloc, &p);
}

/* A persona that declares NO life_events is unaffected — the field is additive
 * and every persona predating it keeps its exact prompt. */
static void persona_without_life_events_parses_to_zero(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p;
    memset(&p, 0, sizeof(p));
    const char *json = "{\"name\":\"tester\",\"core\":{\"identity\":\"Someone.\"}}";
    HU_ASSERT_EQ(hu_persona_load_json(&alloc, json, strlen(json), &p), HU_OK);
    HU_ASSERT_EQ(p.life_events_count, (size_t)0);
    HU_ASSERT_TRUE(p.life_events == NULL);
    hu_persona_deinit(&alloc, &p);
}

/* Builds the prompt with HU_LIFE_EVENTS set to `gate`, returns whether the
 * life-event block is present and whether the guidance came with it. */
static void build_prompt_with_gate(const char *gate, bool *has_block, bool *has_guidance) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p;
    memset(&p, 0, sizeof(p));
    HU_ASSERT_EQ(hu_persona_load_json(&alloc, persona_with_events_json,
                                      strlen(persona_with_events_json), &p),
                 HU_OK);
    if (gate)
        setenv("HU_LIFE_EVENTS", gate, 1);
    else
        unsetenv("HU_LIFE_EVENTS");

    char *prompt = NULL;
    size_t prompt_len = 0;
    HU_ASSERT_EQ(hu_persona_build_prompt(&alloc, &p, "imessage", 8, NULL, 0, &prompt, &prompt_len),
                 HU_OK);
    HU_ASSERT_NOT_NULL(prompt);
    *has_block = strstr(prompt, "moving to the new place") != NULL;
    *has_guidance = strstr(prompt, GUIDANCE_MARKER) != NULL;

    alloc.free(alloc.ctx, prompt, prompt_len + 1);
    unsetenv("HU_LIFE_EVENTS");
    hu_persona_deinit(&alloc, &p);
}

/* THE GATE CONTRACT (.claude/rules/feature-gate-requires-measurement.md): OFF
 * must change nothing, LIVE must change the output. Asserting both in one test
 * makes each half non-vacuous — a block that never emitted would pass the OFF
 * half alone, and one that always emitted would pass the LIVE half alone. */
static void persona_prompt_life_events_gate_off_then_live(void) {
    bool block_off = true, guidance_off = true;
    build_prompt_with_gate(NULL, &block_off, &guidance_off); /* unset == default */
    HU_ASSERT_TRUE(!block_off);
    HU_ASSERT_TRUE(!guidance_off);

    bool block_shadow = true, guidance_shadow = true;
    build_prompt_with_gate("shadow", &block_shadow, &guidance_shadow);
    HU_ASSERT_TRUE(!block_shadow); /* shadow logs, emits nothing */
    HU_ASSERT_TRUE(!guidance_shadow);

    bool block_live = false, guidance_live = false;
    build_prompt_with_gate("live", &block_live, &guidance_live);
    HU_ASSERT_TRUE(block_live);
    HU_ASSERT_TRUE(guidance_live);
}

void run_persona_life_events_tests(void);
void run_persona_life_events_tests(void) {
    HU_TEST_SUITE("persona_life_events");
    HU_RUN_TEST(life_event_state_parse_is_exact_not_substring);
    HU_RUN_TEST(life_event_parse_date_is_deterministic);
    HU_RUN_TEST(life_event_passed_date_unconfirmed_is_unknown_not_completed);
    HU_RUN_TEST(life_event_before_expected_date_keeps_declared_state);
    HU_RUN_TEST(life_event_confirmed_after_expected_date_keeps_declared_state);
    HU_RUN_TEST(life_event_completed_stays_completed_and_is_assertable);
    HU_RUN_TEST(life_event_open_ended_keeps_declared_state);
    HU_RUN_TEST(directive_for_stale_event_forbids_completion_claim);
    HU_RUN_TEST(directive_for_completed_event_omits_the_guard);
    HU_RUN_TEST(directive_attaches_guidance_per_event);
    HU_RUN_TEST(directive_empty_when_no_events);
    HU_RUN_TEST(directive_truncates_without_overflow);
    HU_RUN_TEST(life_events_gate_defaults_off);
    HU_RUN_TEST(persona_life_events_parse_from_json);
    HU_RUN_TEST(persona_without_life_events_parses_to_zero);
    HU_RUN_TEST(persona_prompt_life_events_gate_off_then_live);
}
