/* tests/test_autoresponder.c
 *
 * Sprint B Story 3 — persona-aware autoresponder.
 * Contracts:
 *   ALLOWLIST:
 *     1. empty allowlist → reject all
 *     2. exact match → accept (case-insensitive)
 *     3. near-match (handle is prefix of allowlist entry) → reject
 *     4. cap respected (over-large allowlist_count → reject)
 *   DND WINDOW:
 *     5. no schedules → never in DND
 *     6. normal window (10:00-12:00) — inside / outside
 *     7. wrapped window (22:00-07:00) — both halves
 *     8. day-of-week mask honored
 *   SHOULD-RESPOND (composite):
 *     9. all true → accept
 *    10. allowlist=YES, DND=NO → reject
 *    11. allowlist=NO,  DND=YES → reject
 *    12. enabled=false → reject (master switch)
 *   PROMPT BUILDER:
 *    13. prompt includes "assistant" framing
 *    14. prompt forbids claiming to be the user
 *    15. NULL incoming_text → "(empty)" placeholder
 *   SANITIZE:
 *    16. blank input → fallback "Hey, this is X's assistant..."
 *    17. "I am Seth" (no 's assistant) → replaced with safe fallback
 *    18. "I'm Seth's assistant" → passed through unchanged
 *    19. clean reply → passed through unchanged
 *    20. very long reply → capped at HU_AUTORESPONDER_REPLY_MAX
 *   LOG WRITE:
 *    21. JSON line written + parseable
 *    22. quotes/backslashes escaped
 */

#include "human/autoresponder.h"
#include "test_framework.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── helpers ─────────────────────────────────────────────────────────── */

static void init_cfg(hu_autoresponder_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->enabled = true;
    snprintf(cfg->user_display_name, sizeof(cfg->user_display_name), "%s", "Seth");
}

static void add_allow(hu_autoresponder_config_t *cfg, const char *handle) {
    if (cfg->allowlist_count >= HU_AUTORESPONDER_MAX_ALLOWLIST)
        return;
    snprintf(cfg->allowlist[cfg->allowlist_count], HU_AUTORESPONDER_HANDLE_MAX, "%s", handle);
    cfg->allowlist_count++;
}

static void add_schedule(hu_autoresponder_config_t *cfg, int start_min, int end_min, uint8_t dow) {
    if (cfg->schedule_count >= HU_AUTORESPONDER_MAX_SCHEDULES)
        return;
    cfg->dnd_schedule[cfg->schedule_count].start_minute_of_day = (int16_t)start_min;
    cfg->dnd_schedule[cfg->schedule_count].end_minute_of_day = (int16_t)end_min;
    cfg->dnd_schedule[cfg->schedule_count].days_of_week_mask = dow;
    cfg->schedule_count++;
}

/* Unix epoch starts on Thursday 1970-01-01 UTC at 00:00. To get a
 * specific local time we can compute: (days_since_epoch * 86400) +
 * (hour*3600) + (min*60), with tz_offset_seconds=0 to keep things
 * pure. */
static int64_t mk_local_time(int days_since_epoch, int hour, int min) {
    return (int64_t)days_since_epoch * 86400 + hour * 3600 + min * 60;
}

/* ── allowlist tests ─────────────────────────────────────────────────── */

static void test_allowlist_empty_rejects_all(void) {
    hu_autoresponder_config_t cfg;
    init_cfg(&cfg);
    HU_ASSERT_TRUE(!hu_autoresponder_handle_allowlisted(&cfg, "alice"));
    HU_ASSERT_TRUE(!hu_autoresponder_handle_allowlisted(&cfg, "+15551234567"));
}

static void test_allowlist_exact_case_insensitive_accepts(void) {
    hu_autoresponder_config_t cfg;
    init_cfg(&cfg);
    add_allow(&cfg, "ALICE");
    add_allow(&cfg, "+15551234567");
    HU_ASSERT_TRUE(hu_autoresponder_handle_allowlisted(&cfg, "alice"));
    HU_ASSERT_TRUE(hu_autoresponder_handle_allowlisted(&cfg, "Alice"));
    HU_ASSERT_TRUE(hu_autoresponder_handle_allowlisted(&cfg, "+15551234567"));
}

static void test_allowlist_prefix_does_not_match(void) {
    /* "alice" must NOT match allowlist entry "alice_jones" — the
     * comparison is exact (length-bounded strncasecmp against full
     * HANDLE_MAX). */
    hu_autoresponder_config_t cfg;
    init_cfg(&cfg);
    add_allow(&cfg, "alice_jones");
    /* strncasecmp against the FULL handle slot terminates at the
     * shorter string's NUL, so "alice" vs "alice_jones" differs at
     * char 5 (\0 vs '_') and returns nonzero. */
    HU_ASSERT_TRUE(!hu_autoresponder_handle_allowlisted(&cfg, "alice"));
}

static void test_allowlist_count_over_cap_rejects(void) {
    hu_autoresponder_config_t cfg;
    init_cfg(&cfg);
    add_allow(&cfg, "alice");
    cfg.allowlist_count = HU_AUTORESPONDER_MAX_ALLOWLIST + 99; /* corrupted */
    HU_ASSERT_TRUE(!hu_autoresponder_handle_allowlisted(&cfg, "alice"));
}

/* ── DND-window tests ───────────────────────────────────────────────── */

static void test_dnd_no_schedules_never_in_window(void) {
    hu_autoresponder_config_t cfg;
    init_cfg(&cfg);
    HU_ASSERT_TRUE(!hu_autoresponder_in_dnd_window(&cfg, mk_local_time(0, 12, 0), 0));
}

static void test_dnd_normal_window_inside_outside(void) {
    hu_autoresponder_config_t cfg;
    init_cfg(&cfg);
    /* 10:00-12:00, every day. */
    add_schedule(&cfg, 10 * 60, 12 * 60, HU_DOW_MASK_DAILY);
    HU_ASSERT_TRUE(hu_autoresponder_in_dnd_window(&cfg, mk_local_time(0, 10, 30), 0));
    HU_ASSERT_TRUE(hu_autoresponder_in_dnd_window(&cfg, mk_local_time(0, 11, 59), 0));
    HU_ASSERT_TRUE(
        !hu_autoresponder_in_dnd_window(&cfg, mk_local_time(0, 12, 0), 0)); /* end exclusive */
    HU_ASSERT_TRUE(!hu_autoresponder_in_dnd_window(&cfg, mk_local_time(0, 9, 59), 0));
    HU_ASSERT_TRUE(!hu_autoresponder_in_dnd_window(&cfg, mk_local_time(0, 13, 0), 0));
}

static void test_dnd_wrapped_window_both_halves(void) {
    /* 22:00 → 07:00 every day. Wraps midnight. */
    hu_autoresponder_config_t cfg;
    init_cfg(&cfg);
    add_schedule(&cfg, 22 * 60, 7 * 60, HU_DOW_MASK_DAILY);
    HU_ASSERT_TRUE(hu_autoresponder_in_dnd_window(&cfg, mk_local_time(0, 23, 30), 0)); /* late */
    HU_ASSERT_TRUE(hu_autoresponder_in_dnd_window(&cfg, mk_local_time(0, 6, 30), 0));  /* early */
    HU_ASSERT_TRUE(hu_autoresponder_in_dnd_window(&cfg, mk_local_time(0, 22, 0), 0)); /* boundary */
    HU_ASSERT_TRUE(!hu_autoresponder_in_dnd_window(&cfg, mk_local_time(0, 7, 0), 0)); /* end excl */
    HU_ASSERT_TRUE(!hu_autoresponder_in_dnd_window(&cfg, mk_local_time(0, 12, 0), 0)); /* mid-day */
}

static void test_dnd_day_of_week_mask_honored(void) {
    hu_autoresponder_config_t cfg;
    init_cfg(&cfg);
    /* 10:00-12:00 weekdays only (Mon-Fri). */
    add_schedule(&cfg, 10 * 60, 12 * 60, HU_DOW_MASK_WEEKDAYS);
    /* 1970-01-01 was a Thursday → days_since_epoch=0 → dow=4 (weekday)
     * 1970-01-03 was Saturday → days_since_epoch=2 → dow=6 (weekend). */
    HU_ASSERT_TRUE(hu_autoresponder_in_dnd_window(&cfg, mk_local_time(0, 11, 0), 0));
    HU_ASSERT_TRUE(!hu_autoresponder_in_dnd_window(&cfg, mk_local_time(2, 11, 0), 0));
}

/* ── composite should_respond ────────────────────────────────────────── */

static void test_should_respond_all_true_accepts(void) {
    hu_autoresponder_config_t cfg;
    init_cfg(&cfg);
    add_allow(&cfg, "alice");
    add_schedule(&cfg, 10 * 60, 12 * 60, HU_DOW_MASK_DAILY);
    HU_ASSERT_TRUE(hu_autoresponder_should_respond(&cfg, "alice", mk_local_time(0, 11, 0), 0));
}

static void test_should_respond_allowed_but_not_dnd_rejects(void) {
    hu_autoresponder_config_t cfg;
    init_cfg(&cfg);
    add_allow(&cfg, "alice");
    add_schedule(&cfg, 10 * 60, 12 * 60, HU_DOW_MASK_DAILY);
    HU_ASSERT_TRUE(!hu_autoresponder_should_respond(&cfg, "alice", mk_local_time(0, 15, 0), 0));
}

static void test_should_respond_dnd_but_not_allowed_rejects(void) {
    hu_autoresponder_config_t cfg;
    init_cfg(&cfg);
    add_allow(&cfg, "bob");
    add_schedule(&cfg, 10 * 60, 12 * 60, HU_DOW_MASK_DAILY);
    HU_ASSERT_TRUE(!hu_autoresponder_should_respond(&cfg, "alice", mk_local_time(0, 11, 0), 0));
}

static void test_should_respond_disabled_master_switch(void) {
    hu_autoresponder_config_t cfg;
    init_cfg(&cfg);
    cfg.enabled = false;
    add_allow(&cfg, "alice");
    add_schedule(&cfg, 10 * 60, 12 * 60, HU_DOW_MASK_DAILY);
    HU_ASSERT_TRUE(!hu_autoresponder_should_respond(&cfg, "alice", mk_local_time(0, 11, 0), 0));
}

/* ── prompt builder ──────────────────────────────────────────────────── */

static void test_prompt_includes_assistant_framing(void) {
    hu_autoresponder_config_t cfg;
    init_cfg(&cfg);
    char buf[2048] = {0};
    size_t n = hu_autoresponder_build_prompt(&cfg, "alice", "imessage", "hey are you free?",
                                             "Tone: warm, casual.", buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(buf, "assistant") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "Seth") != NULL);
}

static void test_prompt_forbids_claiming_to_be_user(void) {
    hu_autoresponder_config_t cfg;
    init_cfg(&cfg);
    char buf[2048] = {0};
    hu_autoresponder_build_prompt(&cfg, "alice", "imessage", "hi", NULL, buf, sizeof(buf));
    /* Must contain explicit prohibition. */
    HU_ASSERT_TRUE(strstr(buf, "NEVER claim to be the user") != NULL);
}

static void test_prompt_null_incoming_falls_back_to_empty(void) {
    hu_autoresponder_config_t cfg;
    init_cfg(&cfg);
    char buf[1024] = {0};
    hu_autoresponder_build_prompt(&cfg, "alice", "imessage", NULL, NULL, buf, sizeof(buf));
    HU_ASSERT_TRUE(strstr(buf, "(empty)") != NULL);
}

/* ── sanitize tests ──────────────────────────────────────────────────── */

static void test_sanitize_blank_input_writes_fallback(void) {
    hu_autoresponder_config_t cfg;
    init_cfg(&cfg);
    char buf[256] = {0};
    size_t n = hu_autoresponder_sanitize_reply(&cfg, "", buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(buf, "Seth's assistant") != NULL);
}

static void test_sanitize_false_user_claim_replaced(void) {
    hu_autoresponder_config_t cfg;
    init_cfg(&cfg);
    char buf[256] = {0};
    /* Model output: "Hi! I am Seth — busy right now." — claims to BE
     * the user (no 's assistant follow-up). Must be replaced. */
    size_t n =
        hu_autoresponder_sanitize_reply(&cfg, "Hi! I am Seth — busy right now.", buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(buf, "Seth's assistant") != NULL);
    /* Original phrasing must be gone (we replaced with the canned fallback). */
    HU_ASSERT_TRUE(strstr(buf, "busy right now") == NULL);
}

static void test_sanitize_assistant_framing_passes_through(void) {
    hu_autoresponder_config_t cfg;
    init_cfg(&cfg);
    char buf[256] = {0};
    const char *good = "Hey, this is Seth's assistant — they'll be back online tonight!";
    size_t n = hu_autoresponder_sanitize_reply(&cfg, good, buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_STR_EQ(buf, good);
}

static void test_sanitize_clean_reply_passes_through(void) {
    hu_autoresponder_config_t cfg;
    init_cfg(&cfg);
    char buf[256] = {0};
    const char *good = "Got it — Seth should be free this evening.";
    size_t n = hu_autoresponder_sanitize_reply(&cfg, good, buf, sizeof(buf));
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_STR_EQ(buf, good);
}

static void test_sanitize_very_long_reply_capped(void) {
    hu_autoresponder_config_t cfg;
    init_cfg(&cfg);
    /* 600-char safe reply. */
    char input[700];
    memset(input, 'A', sizeof(input) - 1);
    input[sizeof(input) - 1] = '\0';
    char buf[1024] = {0};
    size_t n = hu_autoresponder_sanitize_reply(&cfg, input, buf, sizeof(buf));
    HU_ASSERT_TRUE(n <= HU_AUTORESPONDER_REPLY_MAX);
}

/* ── log write ──────────────────────────────────────────────────────── */

static void test_log_writes_parseable_json_line(void) {
    hu_autoresponder_config_t cfg;
    init_cfg(&cfg);
    /* Custom log path to a tmpfile we can read back. */
    snprintf(cfg.log_path, sizeof(cfg.log_path), "/tmp/hu_autoresponder_test_%d.log",
             (int)getpid());
    unlink(cfg.log_path);
    hu_error_t err = hu_autoresponder_log_reply(&cfg, "alice", "imessage",
                                                "Hey, this is Seth's assistant", 1700000000);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    FILE *fp = fopen(cfg.log_path, "r");
    HU_ASSERT_NOT_NULL(fp);
    char line[2048] = {0};
    HU_ASSERT_NOT_NULL(fgets(line, sizeof(line), fp));
    fclose(fp);
    unlink(cfg.log_path);
    HU_ASSERT_TRUE(strstr(line, "\"ts\":1700000000") != NULL);
    HU_ASSERT_TRUE(strstr(line, "\"contact\":\"alice\"") != NULL);
    HU_ASSERT_TRUE(strstr(line, "\"channel\":\"imessage\"") != NULL);
    HU_ASSERT_TRUE(strstr(line, "Seth's assistant") != NULL);
}

static void test_log_escapes_quotes_and_backslashes(void) {
    hu_autoresponder_config_t cfg;
    init_cfg(&cfg);
    snprintf(cfg.log_path, sizeof(cfg.log_path), "/tmp/hu_autoresponder_test_esc_%d.log",
             (int)getpid());
    unlink(cfg.log_path);
    /* Reply text with a literal " and \ — must escape. */
    hu_error_t err =
        hu_autoresponder_log_reply(&cfg, "alice", "imessage", "He said \"hi\\there\"", 1700000000);
    HU_ASSERT_EQ((int)err, (int)HU_OK);
    FILE *fp = fopen(cfg.log_path, "r");
    HU_ASSERT_NOT_NULL(fp);
    char line[2048] = {0};
    HU_ASSERT_NOT_NULL(fgets(line, sizeof(line), fp));
    fclose(fp);
    unlink(cfg.log_path);
    /* The literal quote should appear as \" in the JSON. */
    HU_ASSERT_TRUE(strstr(line, "\\\"hi") != NULL);
    HU_ASSERT_TRUE(strstr(line, "\\\\there") != NULL);
}

void run_autoresponder_tests(void) {
    HU_TEST_SUITE("autoresponder");
    /* allowlist */
    HU_RUN_TEST(test_allowlist_empty_rejects_all);
    HU_RUN_TEST(test_allowlist_exact_case_insensitive_accepts);
    HU_RUN_TEST(test_allowlist_prefix_does_not_match);
    HU_RUN_TEST(test_allowlist_count_over_cap_rejects);
    /* DND window */
    HU_RUN_TEST(test_dnd_no_schedules_never_in_window);
    HU_RUN_TEST(test_dnd_normal_window_inside_outside);
    HU_RUN_TEST(test_dnd_wrapped_window_both_halves);
    HU_RUN_TEST(test_dnd_day_of_week_mask_honored);
    /* composite */
    HU_RUN_TEST(test_should_respond_all_true_accepts);
    HU_RUN_TEST(test_should_respond_allowed_but_not_dnd_rejects);
    HU_RUN_TEST(test_should_respond_dnd_but_not_allowed_rejects);
    HU_RUN_TEST(test_should_respond_disabled_master_switch);
    /* prompt */
    HU_RUN_TEST(test_prompt_includes_assistant_framing);
    HU_RUN_TEST(test_prompt_forbids_claiming_to_be_user);
    HU_RUN_TEST(test_prompt_null_incoming_falls_back_to_empty);
    /* sanitize */
    HU_RUN_TEST(test_sanitize_blank_input_writes_fallback);
    HU_RUN_TEST(test_sanitize_false_user_claim_replaced);
    HU_RUN_TEST(test_sanitize_assistant_framing_passes_through);
    HU_RUN_TEST(test_sanitize_clean_reply_passes_through);
    HU_RUN_TEST(test_sanitize_very_long_reply_capped);
    /* log */
    HU_RUN_TEST(test_log_writes_parseable_json_line);
    HU_RUN_TEST(test_log_escapes_quotes_and_backslashes);
}
