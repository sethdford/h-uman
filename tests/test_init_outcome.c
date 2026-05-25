/* Tests for hu_init_outcome_append — the JSONL persistence path for
 * init_proposer decisions. Each test uses a tmp file via the
 * test-only path override so no test writes to the real
 * ~/.human/initiative_proposals.jsonl. */

#include "human/agent/init_outcome.h"
#include "human/agent/init_proposer.h"
#include "human/core/allocator.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char s_tmp_path[256];

static void use_tmp_path(void) {
    snprintf(s_tmp_path, sizeof(s_tmp_path), "/tmp/hu_init_outcome_test_%d.jsonl", (int)getpid());
    /* Start clean. */
    unlink(s_tmp_path);
    hu_init_outcome_set_path_for_test(s_tmp_path);
}

static void cleanup_tmp_path(void) {
    unlink(s_tmp_path);
    hu_init_outcome_set_path_for_test(NULL);
}

/* Read whole tmp file into a heap buffer (caller frees via free()). */
static char *read_tmp_file(size_t *out_len) {
    FILE *f = fopen(s_tmp_path, "rb");
    if (!f) {
        *out_len = 0;
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        *out_len = 0;
        return NULL;
    }
    char *buf = (char *)malloc((size_t)sz + 1);
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    *out_len = got;
    return buf;
}

static void test_resolve_path_uses_home_by_default(void) {
    /* Force the no-override path by clearing test override first. */
    hu_init_outcome_set_path_for_test(NULL);
    char path[512] = {0};
    size_t n = hu_init_outcome_resolve_path(path, sizeof(path));
    HU_ASSERT(n > 0);
    HU_ASSERT(strstr(path, ".human/initiative_proposals.jsonl") != NULL);
}

static void test_resolve_path_null_buf_returns_zero(void) {
    HU_ASSERT_EQ(hu_init_outcome_resolve_path(NULL, 16), (size_t)0);
}

static void test_append_fired_decision_writes_complete_jsonl_line(void) {
    use_tmp_path();
    hu_allocator_t alloc = hu_system_allocator();
    hu_init_decision_t d;
    memset(&d, 0, sizeof(d));
    d.should_propose = true;
    d.confidence = 0.91;
    strcpy(d.draft, "Hey, just wanted to check in.");
    d.draft_len = strlen(d.draft);

    HU_ASSERT_EQ(hu_init_outcome_append(&alloc, 1779700000, 42, HU_INIT_RESULT_FIRED, &d,
                                        "+15551234567", true),
                 HU_OK);

    size_t len = 0;
    char *content = read_tmp_file(&len);
    HU_ASSERT_NOT_NULL(content);
    /* Schema marker present so future migrations can detect v1 records. */
    HU_ASSERT(strstr(content, "\"schema\":\"init_outcome_v1\"") != NULL);
    HU_ASSERT(strstr(content, "\"verdict\":\"FIRED\"") != NULL);
    HU_ASSERT(strstr(content, "\"confidence\":0.910") != NULL);
    HU_ASSERT(strstr(content, "\"tick_id\":42") != NULL);
    HU_ASSERT(strstr(content, "\"ts_unix\":1779700000") != NULL);
    HU_ASSERT(strstr(content, "\"draft\":\"Hey, just wanted to check in.\"") != NULL);
    HU_ASSERT(strstr(content, "\"target\":\"+15551234567\"") != NULL);
    HU_ASSERT(strstr(content, "\"dry_run\":true") != NULL);
    /* Trailing newline (JSONL contract). */
    HU_ASSERT_EQ(content[len - 1], '\n');
    free(content);
    cleanup_tmp_path();
}

static void test_append_negative_decision_records_reason(void) {
    use_tmp_path();
    hu_allocator_t alloc = hu_system_allocator();
    hu_init_decision_t d;
    memset(&d, 0, sizeof(d));
    d.should_propose = false;
    d.confidence = 0.4;
    strcpy(d.skip_reason, "context too thin to justify outreach");
    d.skip_reason_len = strlen(d.skip_reason);

    HU_ASSERT_EQ(
        hu_init_outcome_append(&alloc, 1779700001, 43, HU_INIT_RESULT_NEGATIVE, &d, "", false),
        HU_OK);

    size_t len = 0;
    char *content = read_tmp_file(&len);
    HU_ASSERT_NOT_NULL(content);
    HU_ASSERT(strstr(content, "\"verdict\":\"NEGATIVE\"") != NULL);
    HU_ASSERT(strstr(content, "\"reason\":\"context too thin to justify outreach\"") != NULL);
    HU_ASSERT(strstr(content, "\"dry_run\":false") != NULL);
    free(content);
    cleanup_tmp_path();
}

static void test_append_escapes_quotes_and_newlines_in_draft(void) {
    /* JSONL must be valid line-delimited JSON — quotes and newlines in
     * the draft MUST be escaped so jq / future parsers don't choke. */
    use_tmp_path();
    hu_allocator_t alloc = hu_system_allocator();
    hu_init_decision_t d;
    memset(&d, 0, sizeof(d));
    d.should_propose = true;
    d.confidence = 0.9;
    strcpy(d.draft, "She said \"hi\"\nthen left");
    d.draft_len = strlen(d.draft);

    HU_ASSERT_EQ(hu_init_outcome_append(&alloc, 1779700002, 44, HU_INIT_RESULT_FIRED, &d, "", true),
                 HU_OK);

    size_t len = 0;
    char *content = read_tmp_file(&len);
    HU_ASSERT_NOT_NULL(content);
    /* Raw quote and newline MUST NOT appear in the draft value. */
    HU_ASSERT(strstr(content, "\"draft\":\"She said \\\"hi\\\"\\nthen left\"") != NULL);
    /* Verify the line itself contains exactly one '\n' (the JSONL terminator). */
    size_t nl_count = 0;
    for (size_t i = 0; i < len; i++)
        if (content[i] == '\n')
            nl_count++;
    HU_ASSERT_EQ(nl_count, (size_t)1);
    free(content);
    cleanup_tmp_path();
}

static void test_append_multiple_calls_produce_multiple_lines(void) {
    use_tmp_path();
    hu_allocator_t alloc = hu_system_allocator();
    hu_init_decision_t d;
    memset(&d, 0, sizeof(d));
    d.confidence = 0.5;

    for (int i = 0; i < 5; i++) {
        HU_ASSERT_EQ(hu_init_outcome_append(&alloc, 1779700000 + i, (uint64_t)i,
                                            HU_INIT_RESULT_LOW_CONFIDENCE, &d, "+15551234567",
                                            true),
                     HU_OK);
    }

    size_t len = 0;
    char *content = read_tmp_file(&len);
    HU_ASSERT_NOT_NULL(content);
    /* 5 records → 5 newlines. */
    size_t nl_count = 0;
    for (size_t i = 0; i < len; i++)
        if (content[i] == '\n')
            nl_count++;
    HU_ASSERT_EQ(nl_count, (size_t)5);
    /* Each line should carry its own tick_id. */
    HU_ASSERT(strstr(content, "\"tick_id\":0") != NULL);
    HU_ASSERT(strstr(content, "\"tick_id\":4") != NULL);
    free(content);
    cleanup_tmp_path();
}

static void test_append_null_decision_returns_invalid_argument(void) {
    use_tmp_path();
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ(
        hu_init_outcome_append(&alloc, 1779700000, 1, HU_INIT_RESULT_FIRED, NULL, "", true),
        HU_ERR_INVALID_ARGUMENT);
    cleanup_tmp_path();
}

static void test_append_writes_all_verdict_strings(void) {
    /* Smoke-test that every verdict enum maps to a non-UNKNOWN string. */
    use_tmp_path();
    hu_allocator_t alloc = hu_system_allocator();
    hu_init_decision_t d;
    memset(&d, 0, sizeof(d));
    hu_init_proposer_result_t verdicts[] = {
        HU_INIT_RESULT_FIRED,     HU_INIT_RESULT_LOW_CONFIDENCE, HU_INIT_RESULT_NEGATIVE,
        HU_INIT_RESULT_LLM_ERROR, HU_INIT_RESULT_PARSE_ERROR,
    };
    const char *names[] = {"FIRED", "LOW_CONFIDENCE", "NEGATIVE", "LLM_ERROR", "PARSE_ERROR"};
    for (size_t i = 0; i < sizeof(verdicts) / sizeof(verdicts[0]); i++) {
        HU_ASSERT_EQ(
            hu_init_outcome_append(&alloc, 1779700000 + (int64_t)i, i, verdicts[i], &d, "", true),
            HU_OK);
    }
    size_t len = 0;
    char *content = read_tmp_file(&len);
    HU_ASSERT_NOT_NULL(content);
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char needle[64];
        snprintf(needle, sizeof(needle), "\"verdict\":\"%s\"", names[i]);
        HU_ASSERT(strstr(content, needle) != NULL);
    }
    /* No UNKNOWN slipped in. */
    HU_ASSERT(strstr(content, "UNKNOWN") == NULL);
    free(content);
    cleanup_tmp_path();
}

/* ──────────────────────────────────────────────────────────────────────────
 * Aggregator tests (status subcommand backbone).
 *
 * hu_init_outcome_aggregate_line is a pure predicate over one JSONL
 * line — testable in isolation without spinning the CLI or reading
 * a real file. */

static void test_aggregate_fired_increments_correct_counter(void) {
    hu_init_status_t s;
    memset(&s, 0, sizeof(s));
    const char *line = "{\"schema\":\"init_outcome_v1\",\"ts_unix\":1779700000,\"tick_id\":1,"
                       "\"verdict\":\"FIRED\",\"confidence\":0.91,\"dry_run\":true,"
                       "\"draft\":\"hi\",\"reason\":\"\",\"target\":\"+1\"}";
    hu_init_outcome_aggregate_line(&s, line, strlen(line));
    HU_ASSERT_EQ(s.total, (size_t)1);
    HU_ASSERT_EQ(s.count_fired, (size_t)1);
    HU_ASSERT_EQ(s.count_negative, (size_t)0);
    HU_ASSERT(s.sum_confidence > 0.9 && s.sum_confidence < 0.92);
    HU_ASSERT_EQ(s.last_fired_ts_unix, (int64_t)1779700000);
}

static void test_aggregate_tracks_latest_fired_ts(void) {
    /* Multiple FIRED lines → last_fired_ts_unix should be MAX, not last-seen. */
    hu_init_status_t s;
    memset(&s, 0, sizeof(s));
    const char *l1 = "{\"verdict\":\"FIRED\",\"ts_unix\":1779700000,\"confidence\":0.9}";
    const char *l2 = "{\"verdict\":\"FIRED\",\"ts_unix\":1779800000,\"confidence\":0.95}";
    const char *l3 = "{\"verdict\":\"FIRED\",\"ts_unix\":1779750000,\"confidence\":0.85}";
    hu_init_outcome_aggregate_line(&s, l1, strlen(l1));
    hu_init_outcome_aggregate_line(&s, l2, strlen(l2));
    hu_init_outcome_aggregate_line(&s, l3, strlen(l3));
    HU_ASSERT_EQ(s.count_fired, (size_t)3);
    /* MAX of the three ts_unix values. */
    HU_ASSERT_EQ(s.last_fired_ts_unix, (int64_t)1779800000);
}

static void test_aggregate_all_verdict_strings_route_to_distinct_counters(void) {
    hu_init_status_t s;
    memset(&s, 0, sizeof(s));
    const char *lines[] = {
        "{\"verdict\":\"FIRED\",\"confidence\":0.9,\"ts_unix\":1}",
        "{\"verdict\":\"LOW_CONFIDENCE\",\"confidence\":0.5,\"ts_unix\":2}",
        "{\"verdict\":\"NEGATIVE\",\"confidence\":0.0,\"ts_unix\":3}",
        "{\"verdict\":\"LLM_ERROR\",\"confidence\":0.0,\"ts_unix\":4}",
        "{\"verdict\":\"PARSE_ERROR\",\"confidence\":0.0,\"ts_unix\":5}",
    };
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
        hu_init_outcome_aggregate_line(&s, lines[i], strlen(lines[i]));
    HU_ASSERT_EQ(s.total, (size_t)5);
    HU_ASSERT_EQ(s.count_fired, (size_t)1);
    HU_ASSERT_EQ(s.count_low_confidence, (size_t)1);
    HU_ASSERT_EQ(s.count_negative, (size_t)1);
    HU_ASSERT_EQ(s.count_llm_error, (size_t)1);
    HU_ASSERT_EQ(s.count_parse_error, (size_t)1);
    HU_ASSERT_EQ(s.count_unknown_verdict, (size_t)0);
}

static void test_aggregate_unknown_verdict_caught_separately(void) {
    /* Schema drift safety: a future verdict we don't know about must
     * bump count_unknown_verdict rather than silently miscounting. */
    hu_init_status_t s;
    memset(&s, 0, sizeof(s));
    const char *line =
        "{\"verdict\":\"WHAT_IS_THIS_NEW_VERDICT\",\"confidence\":0.5,\"ts_unix\":1}";
    hu_init_outcome_aggregate_line(&s, line, strlen(line));
    HU_ASSERT_EQ(s.total, (size_t)1);
    HU_ASSERT_EQ(s.count_unknown_verdict, (size_t)1);
    HU_ASSERT_EQ(s.count_fired, (size_t)0);
}

static void test_aggregate_malformed_line_is_noop(void) {
    hu_init_status_t s;
    memset(&s, 0, sizeof(s));
    /* Truncated JSON. */
    hu_init_outcome_aggregate_line(&s, "{\"verdict\":\"FIRE", 16);
    /* Not JSON at all. */
    hu_init_outcome_aggregate_line(&s, "not json", 8);
    /* JSON array instead of object. */
    hu_init_outcome_aggregate_line(&s, "[\"x\"]", 5);
    /* Object without verdict field. */
    hu_init_outcome_aggregate_line(&s, "{\"confidence\":0.5}", 18);
    /* None should mutate state. */
    HU_ASSERT_EQ(s.total, (size_t)0);
    HU_ASSERT_EQ(s.count_unknown_verdict, (size_t)0);
}

static void test_aggregate_null_args_no_crash(void) {
    hu_init_status_t s;
    memset(&s, 0, sizeof(s));
    hu_init_outcome_aggregate_line(NULL, "abc", 3);
    hu_init_outcome_aggregate_line(&s, NULL, 3);
    hu_init_outcome_aggregate_line(&s, "abc", 0);
    HU_ASSERT_EQ(s.total, (size_t)0);
}

void run_init_outcome_tests(void);
void run_init_outcome_tests(void) {
    HU_TEST_SUITE("init_outcome");
    HU_RUN_TEST(test_resolve_path_uses_home_by_default);
    HU_RUN_TEST(test_resolve_path_null_buf_returns_zero);
    HU_RUN_TEST(test_append_fired_decision_writes_complete_jsonl_line);
    HU_RUN_TEST(test_append_negative_decision_records_reason);
    HU_RUN_TEST(test_append_escapes_quotes_and_newlines_in_draft);
    HU_RUN_TEST(test_append_multiple_calls_produce_multiple_lines);
    HU_RUN_TEST(test_append_null_decision_returns_invalid_argument);
    HU_RUN_TEST(test_append_writes_all_verdict_strings);
    HU_RUN_TEST(test_aggregate_fired_increments_correct_counter);
    HU_RUN_TEST(test_aggregate_tracks_latest_fired_ts);
    HU_RUN_TEST(test_aggregate_all_verdict_strings_route_to_distinct_counters);
    HU_RUN_TEST(test_aggregate_unknown_verdict_caught_separately);
    HU_RUN_TEST(test_aggregate_malformed_line_is_noop);
    HU_RUN_TEST(test_aggregate_null_args_no_crash);
}
