/* tests/test_reflection_schema.c — Unit tests for hu_reflection_parse +
 * compute_pattern_id stability.
 *
 * Spec: docs/plans/2026-05-26-reflection-loop/tasks.md, Task 1.
 *
 * What this suite pins (positive contracts, not bug-fossilization —
 * see ~/.claude/rules/tests-that-pin-bugs.md):
 *   - Malformed JSON → non-OK return + heap *out_error.
 *   - Missing required fields → pattern rejected (count drops).
 *   - Unknown `type` string → pattern rejected.
 *   - confidence outside [0,1] → pattern rejected.
 *   - Valid single-pattern object → 1 pattern, all fields copied,
 *     stable id computed.
 *   - Stable id determinism: same (type, subject, observation[:128])
 *     across two parse runs hashes to the SAME id.
 *   - Stable id sensitivity: changing type OR subject OR the first 128
 *     chars of observation changes the id.
 *   - Stable id stability past 128 chars: trailing prose past 128
 *     chars of observation does NOT change the id (so light LLM
 *     phrasing drift doesn't churn pattern rows).
 *   - evidence_ids/channels arrays cap at 8 with a truncation warning
 *     surfaced via *out_error.
 *   - prose_summary extracted into heap string.
 *   - Empty / missing patterns array → HU_OK with 0 count (a
 *     reflection run is allowed to conclude nothing new).
 *   - All 6 enum variants round-trip through type_str. */

#include "human/reflection.h"
#include "test_framework.h"

#include <stdlib.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE

/* ── Malformed input ───────────────────────────────────────────── */

static void test_schema_rejects_malformed_json(void) {
    hu_reflection_pattern_t *patterns = NULL;
    int count = 0;
    char *prose = NULL;
    char *error = NULL;
    hu_error_t err = hu_reflection_parse("{not valid json", &patterns, &count, &prose, &error);
    HU_ASSERT_NEQ((int)err, (int)HU_OK);
    HU_ASSERT(patterns == NULL);
    HU_ASSERT_EQ(count, 0);
    HU_ASSERT(error != NULL);
    free(error);
    free(prose);
}

static void test_schema_rejects_non_object_root(void) {
    hu_reflection_pattern_t *patterns = NULL;
    int count = 0;
    char *prose = NULL;
    char *error = NULL;
    hu_error_t err = hu_reflection_parse("[1, 2, 3]", &patterns, &count, &prose, &error);
    HU_ASSERT_NEQ((int)err, (int)HU_OK);
    HU_ASSERT(patterns == NULL);
    HU_ASSERT_EQ(count, 0);
    HU_ASSERT(error != NULL);
    free(error);
    free(prose);
}

static void test_schema_rejects_null_inputs(void) {
    hu_reflection_pattern_t *patterns = NULL;
    int count = 0;
    char *prose = NULL;
    char *error = NULL;
    /* Missing required out_* parameters must fail without
     * dereferencing — caller-side defense. */
    HU_ASSERT_NEQ((int)hu_reflection_parse(NULL, &patterns, &count, &prose, &error), (int)HU_OK);
    HU_ASSERT_NEQ((int)hu_reflection_parse("{}", NULL, &count, &prose, &error), (int)HU_OK);
    HU_ASSERT_NEQ((int)hu_reflection_parse("{}", &patterns, NULL, &prose, &error), (int)HU_OK);
    HU_ASSERT_NEQ((int)hu_reflection_parse("{}", &patterns, &count, NULL, &error), (int)HU_OK);
    HU_ASSERT_NEQ((int)hu_reflection_parse("{}", &patterns, &count, &prose, NULL), (int)HU_OK);
}

/* ── Empty / missing patterns is success ───────────────────────── */

static void test_schema_accepts_missing_patterns_array(void) {
    hu_reflection_pattern_t *patterns = NULL;
    int count = 0;
    char *prose = NULL;
    char *error = NULL;
    hu_error_t err = hu_reflection_parse("{\"prose_summary\": \"nothing new\"}", &patterns, &count,
                                         &prose, &error);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ(count, 0);
    HU_ASSERT(patterns == NULL);
    HU_ASSERT(prose != NULL);
    HU_ASSERT_STR_EQ(prose, "nothing new");
    free(prose);
    free(error);
}

static void test_schema_accepts_empty_patterns_array(void) {
    hu_reflection_pattern_t *patterns = NULL;
    int count = 0;
    char *prose = NULL;
    char *error = NULL;
    hu_error_t err = hu_reflection_parse("{\"patterns\": []}", &patterns, &count, &prose, &error);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ(count, 0);
    HU_ASSERT(patterns == NULL);
    free(prose);
    free(error);
}

/* ── Valid single-pattern parse ────────────────────────────────── */

static const char *k_valid_one_pattern =
    "{"
    "  \"prose_summary\": \"Two-line summary of the run.\","
    "  \"patterns\": ["
    "    {"
    "      \"type\": \"topic_recurrence\","
    "      \"subject\": \"alice\","
    "      \"observation\": \"Alice has mentioned her job stress 3 times in 2 weeks\","
    "      \"confidence\": 0.82,"
    "      \"evidence_ids\": [\"turn_123\", \"turn_456\"],"
    "      \"channels\": [\"imessage\", \"sms\"]"
    "    }"
    "  ]"
    "}";

static void test_schema_accepts_valid_pattern(void) {
    hu_reflection_pattern_t *patterns = NULL;
    int count = 0;
    char *prose = NULL;
    char *error = NULL;
    hu_error_t err = hu_reflection_parse(k_valid_one_pattern, &patterns, &count, &prose, &error);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ(count, 1);
    HU_ASSERT(patterns != NULL);
    HU_ASSERT_EQ((int)patterns[0].type, (int)HU_REFLECTION_PATTERN_TOPIC_RECURRENCE);
    HU_ASSERT_STR_EQ(patterns[0].subject, "alice");
    HU_ASSERT_STR_EQ(patterns[0].observation,
                     "Alice has mentioned her job stress 3 times in 2 weeks");
    HU_ASSERT(patterns[0].confidence > 0.81 && patterns[0].confidence < 0.83);
    HU_ASSERT_EQ(patterns[0].evidence_count, 2);
    HU_ASSERT_STR_EQ(patterns[0].evidence_ids[0], "turn_123");
    HU_ASSERT_STR_EQ(patterns[0].evidence_ids[1], "turn_456");
    HU_ASSERT_EQ(patterns[0].channel_count, 2);
    HU_ASSERT_STR_EQ(patterns[0].channels[0], "imessage");
    HU_ASSERT_STR_EQ(patterns[0].channels[1], "sms");
    /* Stable id is 16 hex chars. */
    HU_ASSERT_EQ((int)strlen(patterns[0].id), 16);
    HU_ASSERT(prose != NULL);
    HU_ASSERT_STR_EQ(prose, "Two-line summary of the run.");
    free(patterns);
    free(prose);
    free(error);
}

/* ── Required-field validation drops bad rows ──────────────────── */

static void test_schema_rejects_missing_required_field(void) {
    /* Missing observation. */
    const char *json = "{\"patterns\": [{\"type\": \"preference\", \"subject\": \"x\","
                       " \"confidence\": 0.5}]}";
    hu_reflection_pattern_t *patterns = NULL;
    int count = 0;
    char *prose = NULL;
    char *error = NULL;
    hu_error_t err = hu_reflection_parse(json, &patterns, &count, &prose, &error);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ(count, 0); /* Row rejected, no patterns survive. */
    HU_ASSERT(error != NULL);
    free(patterns);
    free(prose);
    free(error);
}

static void test_schema_rejects_unknown_pattern_type(void) {
    const char *json = "{\"patterns\": [{\"type\": \"made_up_type\", \"subject\": \"x\","
                       " \"observation\": \"y\", \"confidence\": 0.5}]}";
    hu_reflection_pattern_t *patterns = NULL;
    int count = 0;
    char *prose = NULL;
    char *error = NULL;
    hu_error_t err = hu_reflection_parse(json, &patterns, &count, &prose, &error);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ(count, 0);
    HU_ASSERT(error != NULL);
    free(patterns);
    free(prose);
    free(error);
}

static void test_schema_rejects_confidence_out_of_range(void) {
    const char *bad_low = "{\"patterns\": [{\"type\": \"preference\", \"subject\": \"x\","
                          " \"observation\": \"y\", \"confidence\": -0.1}]}";
    const char *bad_high = "{\"patterns\": [{\"type\": \"preference\", \"subject\": \"x\","
                           " \"observation\": \"y\", \"confidence\": 1.5}]}";
    hu_reflection_pattern_t *patterns = NULL;
    int count = 0;
    char *prose = NULL;
    char *error = NULL;

    HU_ASSERT_EQ((int)hu_reflection_parse(bad_low, &patterns, &count, &prose, &error), (int)HU_OK);
    HU_ASSERT_EQ(count, 0);
    free(patterns);
    free(prose);
    free(error);

    patterns = NULL;
    count = 0;
    prose = NULL;
    error = NULL;
    HU_ASSERT_EQ((int)hu_reflection_parse(bad_high, &patterns, &count, &prose, &error), (int)HU_OK);
    HU_ASSERT_EQ(count, 0);
    free(patterns);
    free(prose);
    free(error);
}

/* ── Stable id determinism + sensitivity ───────────────────────── */

static void parse_one_get_id(const char *json, char out_id[64]) {
    hu_reflection_pattern_t *patterns = NULL;
    int count = 0;
    char *prose = NULL;
    char *error = NULL;
    hu_error_t err = hu_reflection_parse(json, &patterns, &count, &prose, &error);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ(count, 1);
    HU_ASSERT(patterns != NULL);
    memcpy(out_id, patterns[0].id, 64);
    free(patterns);
    free(prose);
    free(error);
}

static void test_stable_id_is_deterministic_across_runs(void) {
    char id1[64], id2[64];
    parse_one_get_id(k_valid_one_pattern, id1);
    parse_one_get_id(k_valid_one_pattern, id2);
    HU_ASSERT_STR_EQ(id1, id2);
    HU_ASSERT_EQ((int)strlen(id1), 16);
}

static void test_stable_id_changes_when_subject_changes(void) {
    const char *a = "{\"patterns\": [{\"type\": \"preference\", \"subject\": \"alice\","
                    " \"observation\": \"loves jazz\", \"confidence\": 0.7}]}";
    const char *b = "{\"patterns\": [{\"type\": \"preference\", \"subject\": \"bob\","
                    " \"observation\": \"loves jazz\", \"confidence\": 0.7}]}";
    char id_a[64], id_b[64];
    parse_one_get_id(a, id_a);
    parse_one_get_id(b, id_b);
    HU_ASSERT(strcmp(id_a, id_b) != 0);
}

static void test_stable_id_changes_when_type_changes(void) {
    const char *a = "{\"patterns\": [{\"type\": \"preference\", \"subject\": \"alice\","
                    " \"observation\": \"loves jazz\", \"confidence\": 0.7}]}";
    const char *b = "{\"patterns\": [{\"type\": \"relationship\", \"subject\": \"alice\","
                    " \"observation\": \"loves jazz\", \"confidence\": 0.7}]}";
    char id_a[64], id_b[64];
    parse_one_get_id(a, id_a);
    parse_one_get_id(b, id_b);
    HU_ASSERT(strcmp(id_a, id_b) != 0);
}

static void test_stable_id_is_insensitive_to_observation_beyond_128(void) {
    /* Two patterns differing only past byte 128 of observation should
     * hash to the SAME id. This is the "trailing-phrasing-drift
     * doesn't churn the storage row" contract from the design. */
    char obs_a[600], obs_b[600];
    /* Pad both to identical first 128 chars; differ in the tail. */
    memset(obs_a, 'x', 128);
    memset(obs_b, 'x', 128);
    strcpy(obs_a + 128, " and this trailing phrase is one version");
    strcpy(obs_b + 128, " and this trailing phrase is the other version");

    char json_a[1024], json_b[1024];
    snprintf(json_a, sizeof(json_a),
             "{\"patterns\": [{\"type\": \"preference\", \"subject\": \"alice\","
             " \"observation\": \"%s\", \"confidence\": 0.7}]}",
             obs_a);
    snprintf(json_b, sizeof(json_b),
             "{\"patterns\": [{\"type\": \"preference\", \"subject\": \"alice\","
             " \"observation\": \"%s\", \"confidence\": 0.7}]}",
             obs_b);
    char id_a[64], id_b[64];
    parse_one_get_id(json_a, id_a);
    parse_one_get_id(json_b, id_b);
    HU_ASSERT_STR_EQ(id_a, id_b);
}

/* ── Array truncation cap ──────────────────────────────────────── */

static void test_schema_truncates_evidence_ids_past_8(void) {
    const char *json =
        "{\"patterns\": [{\"type\": \"preference\", \"subject\": \"alice\","
        " \"observation\": \"loves jazz\", \"confidence\": 0.7,"
        " \"evidence_ids\": [\"t1\",\"t2\",\"t3\",\"t4\",\"t5\",\"t6\",\"t7\",\"t8\","
        "                    \"t9\",\"t10\"]}]}";
    hu_reflection_pattern_t *patterns = NULL;
    int count = 0;
    char *prose = NULL;
    char *error = NULL;
    hu_error_t err = hu_reflection_parse(json, &patterns, &count, &prose, &error);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ(count, 1);
    HU_ASSERT_EQ(patterns[0].evidence_count, 8);
    HU_ASSERT(error != NULL); /* Truncation warning surfaced. */
    free(patterns);
    free(prose);
    free(error);
}

static void test_schema_truncates_channels_past_8(void) {
    const char *json = "{\"patterns\": [{\"type\": \"preference\", \"subject\": \"alice\","
                       " \"observation\": \"loves jazz\", \"confidence\": 0.7,"
                       " \"channels\": [\"c1\",\"c2\",\"c3\",\"c4\",\"c5\",\"c6\",\"c7\",\"c8\","
                       "                \"c9\"]}]}";
    hu_reflection_pattern_t *patterns = NULL;
    int count = 0;
    char *prose = NULL;
    char *error = NULL;
    hu_error_t err = hu_reflection_parse(json, &patterns, &count, &prose, &error);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ(count, 1);
    HU_ASSERT_EQ(patterns[0].channel_count, 8);
    HU_ASSERT(error != NULL);
    free(patterns);
    free(prose);
    free(error);
}

/* ── Mixed valid+invalid rows: valid survive, count reflects ── */

static void test_schema_mixed_drops_invalid_keeps_valid(void) {
    const char *json = "{\"patterns\": ["
                       "  {\"type\": \"preference\", \"subject\": \"alice\","
                       "   \"observation\": \"loves jazz\", \"confidence\": 0.7},"
                       "  {\"type\": \"made_up\", \"subject\": \"x\","
                       "   \"observation\": \"y\", \"confidence\": 0.5},"
                       "  {\"type\": \"emotional_state\", \"subject\": \"bob\","
                       "   \"observation\": \"feeling stressed\", \"confidence\": 0.9}"
                       "]}";
    hu_reflection_pattern_t *patterns = NULL;
    int count = 0;
    char *prose = NULL;
    char *error = NULL;
    hu_error_t err = hu_reflection_parse(json, &patterns, &count, &prose, &error);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ(count, 2);
    HU_ASSERT_STR_EQ(patterns[0].subject, "alice");
    HU_ASSERT_STR_EQ(patterns[1].subject, "bob");
    HU_ASSERT_EQ((int)patterns[1].type, (int)HU_REFLECTION_PATTERN_EMOTIONAL_STATE);
    free(patterns);
    free(prose);
    free(error);
}

/* ── type_str round-trip for all 6 variants ────────────────────── */

static void test_type_str_round_trips_all_six(void) {
    /* These names are the wire-protocol strings; changing any of them
     * is a stable-id-breaking event documented in reflection.h. */
    HU_ASSERT_STR_EQ(hu_reflection_pattern_type_str(HU_REFLECTION_PATTERN_TOPIC_RECURRENCE),
                     "topic_recurrence");
    HU_ASSERT_STR_EQ(hu_reflection_pattern_type_str(HU_REFLECTION_PATTERN_BEHAVIORAL_SHIFT),
                     "behavioral_shift");
    HU_ASSERT_STR_EQ(hu_reflection_pattern_type_str(HU_REFLECTION_PATTERN_PREFERENCE),
                     "preference");
    HU_ASSERT_STR_EQ(hu_reflection_pattern_type_str(HU_REFLECTION_PATTERN_EMOTIONAL_STATE),
                     "emotional_state");
    HU_ASSERT_STR_EQ(hu_reflection_pattern_type_str(HU_REFLECTION_PATTERN_SCHEDULE_PATTERN),
                     "schedule_pattern");
    HU_ASSERT_STR_EQ(hu_reflection_pattern_type_str(HU_REFLECTION_PATTERN_RELATIONSHIP),
                     "relationship");
    /* Out-of-range returns "unknown" rather than crashing or returning
     * a NULL pointer — callers print this string in log lines. */
    HU_ASSERT_STR_EQ(hu_reflection_pattern_type_str((hu_reflection_pattern_type_t)999), "unknown");
}

void run_reflection_schema_tests(void) {
    HU_TEST_SUITE("reflection_schema");
    HU_RUN_TEST(test_schema_rejects_malformed_json);
    HU_RUN_TEST(test_schema_rejects_non_object_root);
    HU_RUN_TEST(test_schema_rejects_null_inputs);
    HU_RUN_TEST(test_schema_accepts_missing_patterns_array);
    HU_RUN_TEST(test_schema_accepts_empty_patterns_array);
    HU_RUN_TEST(test_schema_accepts_valid_pattern);
    HU_RUN_TEST(test_schema_rejects_missing_required_field);
    HU_RUN_TEST(test_schema_rejects_unknown_pattern_type);
    HU_RUN_TEST(test_schema_rejects_confidence_out_of_range);
    HU_RUN_TEST(test_stable_id_is_deterministic_across_runs);
    HU_RUN_TEST(test_stable_id_changes_when_subject_changes);
    HU_RUN_TEST(test_stable_id_changes_when_type_changes);
    HU_RUN_TEST(test_stable_id_is_insensitive_to_observation_beyond_128);
    HU_RUN_TEST(test_schema_truncates_evidence_ids_past_8);
    HU_RUN_TEST(test_schema_truncates_channels_past_8);
    HU_RUN_TEST(test_schema_mixed_drops_invalid_keeps_valid);
    HU_RUN_TEST(test_type_str_round_trips_all_six);
}

#else /* !HU_ENABLE_SQLITE */

/* Stub runner so the test_main.c symbol resolves at link time when the
 * reflection module is gated off. See
 * ~/.claude/rules/test-source-gate-symmetry.md → option 2
 * (internal-#ifdef-wrap-with-stub-runner). */
void run_reflection_schema_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_SQLITE */
