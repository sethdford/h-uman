/* tests/test_contact_narrative.c
 *
 * Sprint B Story 4 — long-horizon contact narratives.
 * Contracts (8+ pure-function tests; CLI integration tested by hand
 * against real chat.db):
 *   1. default_path: handle with safe chars → sanitized + dir created
 *   2. default_path: handle with email syntax sanitized
 *   3. default_path: NULL/empty handle → 0
 *   4. year_prompt: produces non-empty string with year + handle
 *   5. year_prompt: includes statistics block
 *   6. year_prompt: NULL bucket → 0
 *   7. synthesis_prompt: weaves years
 *   8. synthesis_prompt: empty count → 0
 *   9. render_markdown: contains "## Year YYYY" for each bucket
 *  10. render_markdown: includes synthesis section
 *  11. parse_existing_years: empty file → 0 count
 *  12. parse_existing_years: file with 3 years → 3 entries
 *  13. parse_existing_years: ignores non-matching lines
 *  14. scan: HU_ERR_NOT_SUPPORTED in test build (no chat.db)
 */

#include "human/research/contact_narrative.h"
#include "test_framework.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void test_default_path_sanitizes_handle(void) {
    char out[512] = {0};
    size_t n = hu_contact_narrative_default_path("alice", out, sizeof(out));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(out, "/.human/contacts/alice.md") != NULL);
}

static void test_default_path_handles_email_syntax(void) {
    char out[512] = {0};
    size_t n = hu_contact_narrative_default_path("alice@example.com", out, sizeof(out));
    HU_ASSERT_TRUE(n > 0);
    /* @ is not in [A-Za-z0-9._+-], replaced with _. */
    HU_ASSERT_TRUE(strstr(out, "alice_example.com.md") != NULL);
}

static void test_default_path_null_or_empty_returns_zero(void) {
    char out[512] = {0};
    HU_ASSERT_EQ((int)hu_contact_narrative_default_path(NULL, out, sizeof(out)), 0);
    HU_ASSERT_EQ((int)hu_contact_narrative_default_path("", out, sizeof(out)), 0);
    HU_ASSERT_EQ((int)hu_contact_narrative_default_path("a", NULL, 100), 0);
    HU_ASSERT_EQ((int)hu_contact_narrative_default_path("a", out, 5), 0);
}

static void test_year_prompt_includes_handle_and_year(void) {
    hu_contact_narrative_year_bucket_t b = {.year = 2024,
                                            .msg_count = 142,
                                            .from_them = 70,
                                            .from_me = 72,
                                            .first_ts = 1,
                                            .last_ts = 2};
    char buf[2048] = {0};
    size_t n = hu_contact_narrative_build_year_prompt("alice", 2024, &b, NULL, buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(buf, "alice") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "2024") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "Summary:") != NULL);
}

static void test_year_prompt_includes_stats(void) {
    hu_contact_narrative_year_bucket_t b = {.year = 2024,
                                            .msg_count = 142,
                                            .from_them = 70,
                                            .from_me = 72,
                                            .first_ts = 1,
                                            .last_ts = 2};
    char buf[2048] = {0};
    hu_contact_narrative_build_year_prompt("alice", 2024, &b, NULL, buf, sizeof(buf));
    HU_ASSERT_TRUE(strstr(buf, "142") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "Total messages") != NULL);
}

static void test_year_prompt_null_bucket_returns_zero(void) {
    char buf[2048] = {0};
    HU_ASSERT_EQ(
        (int)hu_contact_narrative_build_year_prompt("alice", 2024, NULL, NULL, buf, sizeof(buf)),
        0);
}

static void test_synthesis_prompt_includes_year_summaries(void) {
    const char *summaries[] = {"Met at conference; mostly project work.",
                               "Shifted to weekend hiking trips.", "Quieter year, fewer messages."};
    int years[] = {2022, 2023, 2024};
    char buf[4096] = {0};
    size_t n =
        hu_contact_narrative_build_synthesis_prompt("alice", summaries, years, 3, buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(buf, "weekend hiking") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "## Year 2022") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "Narrative:") != NULL);
}

static void test_synthesis_prompt_empty_count_returns_zero(void) {
    char buf[1024] = {0};
    HU_ASSERT_EQ(
        (int)hu_contact_narrative_build_synthesis_prompt("alice", NULL, NULL, 0, buf, sizeof(buf)),
        0);
}

static void test_render_markdown_contains_year_headers(void) {
    hu_contact_narrative_scan_result_t scan = {0};
    scan.bucket_count = 2;
    scan.buckets[0] = (hu_contact_narrative_year_bucket_t){
        .year = 2023, .msg_count = 50, .from_them = 25, .from_me = 25};
    scan.buckets[1] = (hu_contact_narrative_year_bucket_t){
        .year = 2024, .msg_count = 80, .from_them = 40, .from_me = 40};
    const char *summaries[] = {"Project year.", "Hiking year."};
    char buf[4096] = {0};
    size_t n = hu_contact_narrative_render_markdown(
        "alice", &scan, summaries, "Steady close friendship.", 1700000000, buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(buf, "## Year 2023") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "## Year 2024") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "Hiking year.") != NULL);
}

static void test_render_markdown_includes_synthesis(void) {
    hu_contact_narrative_scan_result_t scan = {0};
    scan.bucket_count = 1;
    scan.buckets[0] = (hu_contact_narrative_year_bucket_t){.year = 2024, .msg_count = 10};
    const char *summaries[] = {"Sparse."};
    char buf[2048] = {0};
    size_t n = hu_contact_narrative_render_markdown("alice", &scan, summaries, "Acquaintance only.",
                                                    1700000000, buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(buf, "## Synthesis") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "Acquaintance only.") != NULL);
}

static void test_parse_existing_years_empty_file(void) {
    char path[256];
    snprintf(path, sizeof(path), "/tmp/hu_cn_test_empty_%d.md", (int)getpid());
    FILE *fp = fopen(path, "w");
    fclose(fp);
    int years[HU_CONTACT_NARRATIVE_MAX_YEARS];
    size_t count = 99; /* should be reset */
    hu_error_t err = hu_contact_narrative_parse_existing_years(path, years, &count);
    unlink(path);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ((int)count, 0);
}

static void test_parse_existing_years_three_entries(void) {
    char path[256];
    snprintf(path, sizeof(path), "/tmp/hu_cn_test_three_%d.md", (int)getpid());
    FILE *fp = fopen(path, "w");
    fputs("# alice\n\n## Year 2022\nsome text\n## Year 2023\nmore text\n## Year 2024\nfinal\n", fp);
    fclose(fp);
    int years[HU_CONTACT_NARRATIVE_MAX_YEARS];
    size_t count = 0;
    hu_error_t err = hu_contact_narrative_parse_existing_years(path, years, &count);
    unlink(path);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    HU_ASSERT_EQ((int)count, 3);
    HU_ASSERT_EQ(years[0], 2022);
    HU_ASSERT_EQ(years[1], 2023);
    HU_ASSERT_EQ(years[2], 2024);
}

static void test_parse_existing_years_ignores_non_matching(void) {
    char path[256];
    snprintf(path, sizeof(path), "/tmp/hu_cn_test_ignore_%d.md", (int)getpid());
    FILE *fp = fopen(path, "w");
    fputs("# alice\n## Year 2024\ntext\n## Other heading\n# Year 9999 (no ##)\n", fp);
    fclose(fp);
    int years[HU_CONTACT_NARRATIVE_MAX_YEARS];
    size_t count = 0;
    hu_contact_narrative_parse_existing_years(path, years, &count);
    unlink(path);
    HU_ASSERT_EQ((int)count, 1);
    HU_ASSERT_EQ(years[0], 2024);
}

static void test_scan_returns_not_supported_in_test_build(void) {
    /* In HU_IS_TEST builds the SQL path stubs out. */
    hu_contact_narrative_scan_result_t scan;
    hu_error_t err = hu_contact_narrative_scan("/tmp/nonexistent.db", "alice", &scan);
    HU_ASSERT_EQ((int)err, (int)HU_ERR_NOT_SUPPORTED);
}

void run_contact_narrative_tests(void) {
    HU_TEST_SUITE("contact_narrative");
    HU_RUN_TEST(test_default_path_sanitizes_handle);
    HU_RUN_TEST(test_default_path_handles_email_syntax);
    HU_RUN_TEST(test_default_path_null_or_empty_returns_zero);
    HU_RUN_TEST(test_year_prompt_includes_handle_and_year);
    HU_RUN_TEST(test_year_prompt_includes_stats);
    HU_RUN_TEST(test_year_prompt_null_bucket_returns_zero);
    HU_RUN_TEST(test_synthesis_prompt_includes_year_summaries);
    HU_RUN_TEST(test_synthesis_prompt_empty_count_returns_zero);
    HU_RUN_TEST(test_render_markdown_contains_year_headers);
    HU_RUN_TEST(test_render_markdown_includes_synthesis);
    HU_RUN_TEST(test_parse_existing_years_empty_file);
    HU_RUN_TEST(test_parse_existing_years_three_entries);
    HU_RUN_TEST(test_parse_existing_years_ignores_non_matching);
    HU_RUN_TEST(test_scan_returns_not_supported_in_test_build);
}
