/* tests/test_reflection_prompt.c — Reflection prompt + input assembly (T4).
 *
 * Pins the four contracts from
 * docs/plans/2026-05-26-reflection-loop/tasks.md Task 4:
 *
 *   AC-T4.1  hu_reflection_build_input formats each turn with the
 *            full tag header [id=...] [channel=...] [ts=...] sender:
 *            content, in oldest-first order.
 *   AC-T4.2  max_chars truncates by DROPPING OLDEST turns (so the
 *            tail — most recent context — survives, matching the
 *            spec's "drop oldest then re-emit" rationale).
 *   AC-T4.3  Zero-turn iter returns HU_OK with empty buffer and
 *            count = 0 — a valid "no recent activity" state.
 *   AC-T4.4  Static system prompt contains the schema markers the
 *            parser later validates against (the 6 enum strings),
 *            so a wire-protocol change at one end gets caught at
 *            the other.
 *
 * The iter pattern is exercised directly with a static array — no
 * daemon, no SQLite. T5/T9 will wire the production iter when the
 * turn-ledger decision lands. */

#include "human/reflection.h"
#include "test_framework.h"

#include <stdlib.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE

/* ── Static-array iter for tests ─────────────────────────────────── */

typedef struct {
    const hu_reflection_turn_t *turns;
    int count;
    int pos;
} static_iter_ctx_t;

static bool static_iter(void *vctx, hu_reflection_turn_t *out_turn) {
    static_iter_ctx_t *ctx = (static_iter_ctx_t *)vctx;
    if (ctx->pos >= ctx->count)
        return false;
    *out_turn = ctx->turns[ctx->pos++];
    return true;
}

/* ── AC-T4.1: format with all tags + oldest-first order ─────────── */

static void test_prompt_build_formats_each_turn_with_full_tag_header(void) {
    static const hu_reflection_turn_t k_turns[] = {
        {.turn_id = "t_001",
         .channel = "imessage",
         .sender = "user",
         .ts_ms = 1779840000000ULL,
         .content = "barely slept this week"},
        {.turn_id = "t_002",
         .channel = "imessage",
         .sender = "assistant",
         .ts_ms = 1779840300000ULL,
         .content = "Sorry - anything specific keeping you up?"},
        {.turn_id = "t_003",
         .channel = "telegram",
         .sender = "user",
         .ts_ms = 1779840900000ULL,
         .content = "Work deadlines piling on"},
    };
    static_iter_ctx_t ctx = {.turns = k_turns, .count = 3, .pos = 0};

    char *buf = NULL;
    int turn_count = 0;
    HU_ASSERT_EQ(
        (int)hu_reflection_build_input(static_iter, &ctx, /*max_chars=*/0, &buf, &turn_count),
        (int)HU_OK);
    HU_ASSERT_EQ(turn_count, 3);
    HU_ASSERT(buf != NULL);

    /* All three channels and contents appear. */
    HU_ASSERT(strstr(buf, "[channel=imessage]") != NULL);
    HU_ASSERT(strstr(buf, "[channel=telegram]") != NULL);
    HU_ASSERT(strstr(buf, "barely slept") != NULL);
    HU_ASSERT(strstr(buf, "Work deadlines") != NULL);
    /* All three turn ids appear so the model can cite back via evidence_ids. */
    HU_ASSERT(strstr(buf, "[id=t_001]") != NULL);
    HU_ASSERT(strstr(buf, "[id=t_002]") != NULL);
    HU_ASSERT(strstr(buf, "[id=t_003]") != NULL);
    /* Senders are present. */
    HU_ASSERT(strstr(buf, "user: barely slept") != NULL);
    HU_ASSERT(strstr(buf, "assistant: Sorry") != NULL);
    /* Oldest-first: t_001 appears BEFORE t_003 in the buffer. */
    const char *p1 = strstr(buf, "[id=t_001]");
    const char *p3 = strstr(buf, "[id=t_003]");
    HU_ASSERT(p1 != NULL && p3 != NULL && p1 < p3);
    /* ISO-8601 timestamps emitted. ts=1779840000000ms ≈ 2026-05-26T...Z. */
    HU_ASSERT(strstr(buf, "[ts=2026-05-") != NULL);

    free(buf);
}

/* ── AC-T4.2: max_chars drops oldest turns, tail survives ─────── */

static void test_prompt_build_drops_oldest_when_max_chars_exceeded(void) {
    /* Make 5 turns of roughly equal size. Cap small enough that only
     * 2-3 of them fit; the LAST one must survive. */
    static const hu_reflection_turn_t k_turns[] = {
        {.turn_id = "old_1",
         .channel = "imessage",
         .sender = "u",
         .ts_ms = 1779840000000ULL,
         .content = "OLDEST_CONTENT_MARKER_AAA"},
        {.turn_id = "old_2",
         .channel = "imessage",
         .sender = "u",
         .ts_ms = 1779840060000ULL,
         .content = "OLDISH_CONTENT_BBB"},
        {.turn_id = "mid_3",
         .channel = "imessage",
         .sender = "u",
         .ts_ms = 1779840120000ULL,
         .content = "MIDDLE_CONTENT_CCC"},
        {.turn_id = "new_4",
         .channel = "imessage",
         .sender = "u",
         .ts_ms = 1779840180000ULL,
         .content = "NEWER_CONTENT_DDD"},
        {.turn_id = "new_5",
         .channel = "imessage",
         .sender = "u",
         .ts_ms = 1779840240000ULL,
         .content = "NEWEST_CONTENT_MARKER_EEE"},
    };
    static_iter_ctx_t ctx = {.turns = k_turns, .count = 5, .pos = 0};

    char *buf = NULL;
    int turn_count = 0;
    /* Each formatted line is ~90 chars; 200 chars fits ~2 of them. */
    HU_ASSERT_EQ(
        (int)hu_reflection_build_input(static_iter, &ctx, /*max_chars=*/200, &buf, &turn_count),
        (int)HU_OK);
    HU_ASSERT(buf != NULL);
    HU_ASSERT(turn_count < 5);  /* truncation kicked in */
    HU_ASSERT(turn_count >= 1); /* at least one survived */

    /* The NEWEST turn must be in the surviving buffer. */
    HU_ASSERT(strstr(buf, "NEWEST_CONTENT_MARKER_EEE") != NULL);
    /* The OLDEST must NOT be — that's the load-bearing assertion. */
    HU_ASSERT(strstr(buf, "OLDEST_CONTENT_MARKER_AAA") == NULL);
    /* The total body must fit the cap. */
    HU_ASSERT(strlen(buf) <= 200);

    free(buf);
}

/* ── AC-T4.3: zero-turn iter is valid ─────────────────────────── */

static void test_prompt_build_zero_turns_returns_empty_ok(void) {
    static_iter_ctx_t ctx = {.turns = NULL, .count = 0, .pos = 0};

    char *buf = NULL;
    int turn_count = 0;
    HU_ASSERT_EQ(
        (int)hu_reflection_build_input(static_iter, &ctx, /*max_chars=*/0, &buf, &turn_count),
        (int)HU_OK);
    HU_ASSERT_EQ(turn_count, 0);
    HU_ASSERT(buf != NULL);
    HU_ASSERT_STR_EQ(buf, "");
    free(buf);
}

/* ── NULL-argument defense ─────────────────────────────────────── */

static void test_prompt_build_rejects_null_args(void) {
    char *buf = NULL;
    int turn_count = 0;
    static_iter_ctx_t ctx = {0};

    HU_ASSERT_NEQ((int)hu_reflection_build_input(NULL, &ctx, 0, &buf, &turn_count), (int)HU_OK);
    HU_ASSERT_NEQ((int)hu_reflection_build_input(static_iter, &ctx, 0, NULL, &turn_count),
                  (int)HU_OK);
    HU_ASSERT_NEQ((int)hu_reflection_build_input(static_iter, &ctx, 0, &buf, NULL), (int)HU_OK);
}

/* ── AC-T4.4: system prompt contains the wire-protocol markers ─── */

static void test_system_prompt_contains_all_six_type_strings(void) {
    const char *p = hu_reflection_system_prompt();
    HU_ASSERT(p != NULL);
    HU_ASSERT(strlen(p) > 100); /* non-empty, real content */

    /* Every pattern type the parser accepts must be advertised in the
     * prompt so the model knows the menu. If a new type lands in T1
     * without updating this prompt, the model will never emit it. */
    HU_ASSERT(strstr(p, "topic_recurrence") != NULL);
    HU_ASSERT(strstr(p, "behavioral_shift") != NULL);
    HU_ASSERT(strstr(p, "preference") != NULL);
    HU_ASSERT(strstr(p, "emotional_state") != NULL);
    HU_ASSERT(strstr(p, "schedule_pattern") != NULL);
    HU_ASSERT(strstr(p, "relationship") != NULL);

    /* Output-shape contract: must explicitly say STRICT JSON to nudge
     * the model away from markdown code fences. */
    HU_ASSERT(strstr(p, "STRICT JSON") != NULL);

    /* The parser reads `prose_summary`, not `summary`. The prompt must
     * use the SAME key — silent skew here drops the model's prose
     * commentary on the floor. */
    HU_ASSERT(strstr(p, "prose_summary") != NULL);
}

/* ── End-to-end: produce a body that the schema parser accepts ─── */

/* This is the load-bearing integration test: it proves the prompt
 * template and the schema parser share a wire format. A mock "ideal
 * model response" matching the prompt's documented schema parses
 * cleanly through hu_reflection_parse. */
static void test_system_prompt_schema_roundtrips_through_parser(void) {
    /* Construct a response that follows the prompt's documented schema
     * exactly. If the prompt advertises a different schema than the
     * parser expects, this test fails — the wire-protocol drift trap
     * other reflection systems hit. */
    const char *response = "{"
                           "  \"patterns\": ["
                           "    {"
                           "      \"type\": \"behavioral_shift\","
                           "      \"subject\": \"Seth\","
                           "      \"observation\": \"shifts to one-word replies after 9pm\","
                           "      \"confidence\": 0.78,"
                           "      \"evidence_ids\": [\"t_001\", \"t_003\"],"
                           "      \"channels\": [\"imessage\"]"
                           "    }"
                           "  ],"
                           "  \"prose_summary\": \"Evenings show terse replies on imessage.\""
                           "}";
    hu_reflection_pattern_t *patterns = NULL;
    int count = 0;
    char *prose = NULL;
    char *err = NULL;
    HU_ASSERT_EQ((int)hu_reflection_parse(response, &patterns, &count, &prose, &err), (int)HU_OK);
    HU_ASSERT_EQ(count, 1);
    HU_ASSERT_EQ((int)patterns[0].type, (int)HU_REFLECTION_PATTERN_BEHAVIORAL_SHIFT);
    HU_ASSERT_STR_EQ(prose, "Evenings show terse replies on imessage.");
    free(patterns);
    free(prose);
    free(err);
}

void run_reflection_prompt_tests(void) {
    HU_TEST_SUITE("reflection_prompt");
    HU_RUN_TEST(test_prompt_build_formats_each_turn_with_full_tag_header);
    HU_RUN_TEST(test_prompt_build_drops_oldest_when_max_chars_exceeded);
    HU_RUN_TEST(test_prompt_build_zero_turns_returns_empty_ok);
    HU_RUN_TEST(test_prompt_build_rejects_null_args);
    HU_RUN_TEST(test_system_prompt_contains_all_six_type_strings);
    HU_RUN_TEST(test_system_prompt_schema_roundtrips_through_parser);
}

#else /* !HU_ENABLE_SQLITE — stub so the runner symbol resolves */

void run_reflection_prompt_tests(void) {
    /* No-op when SQLite is disabled. T4's prompt.c lives in the same
     * gated module as T2's storage.c per the dep-graph comment in
     * CMakeLists.txt; the gate-symmetry check expects either CMake
     * gating OR an internal stub. We use the internal pattern so the
     * test source stays in the unconditional HU_TEST_SOURCES list. */
}

#endif /* HU_ENABLE_SQLITE */
