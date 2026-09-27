/* tests/test_imessage_voice_record.c
 *
 * Native Messages voice delivery (W3): an iMessage voice reply recorded by
 * Messages itself through a BlackHole input instead of sent as a file. The
 * policy and orchestrator are pure; every real-world effect sits behind
 * hu_voice_record_port_t, so these tests touch no audio, AX or chat.db. */
#include "human/channels/imessage_voice_record.h"
#include "test_framework.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static hu_voice_record_facts_t all_ok(void) {
    hu_voice_record_facts_t f = {.ax_trusted = true,
                                 .messages_running = true,
                                 .blackhole_present = true,
                                 .real_mic_configured = true,
                                 .real_mic_present = true,
                                 .real_mic_busy = false,
                                 .user_idle_sec = 120.0,
                                 .min_idle_sec = 20.0};
    return f;
}

static void test_vrec_mode_parse_defaults_to_attachment(void) {
    HU_ASSERT_EQ(hu_voice_delivery_mode_parse(NULL), HU_VOICE_DELIVERY_ATTACHMENT);
    HU_ASSERT_EQ(hu_voice_delivery_mode_parse(""), HU_VOICE_DELIVERY_ATTACHMENT);
    HU_ASSERT_EQ(hu_voice_delivery_mode_parse("live"), HU_VOICE_DELIVERY_ATTACHMENT);
    HU_ASSERT_EQ(hu_voice_delivery_mode_parse("shadow"), HU_VOICE_DELIVERY_SHADOW);
    HU_ASSERT_EQ(hu_voice_delivery_mode_parse("messages"), HU_VOICE_DELIVERY_MESSAGES);
}

static void test_vrec_memo_send_requires_empty_text_and_one_audio(void) {
    const char *caf[] = {"/tmp/a/Audio Message.caf"};
    const char *mp3[] = {"/tmp/human_dtts_1.mp3"};
    const char *png[] = {"/tmp/pic.png"};
    const char *two[] = {"/tmp/a.caf", "/tmp/b.caf"};
    HU_ASSERT_TRUE(hu_voice_record_is_memo_send(0, caf, 1));
    HU_ASSERT_TRUE(hu_voice_record_is_memo_send(0, mp3, 1));
    HU_ASSERT_FALSE(hu_voice_record_is_memo_send(5, caf, 1));
    HU_ASSERT_FALSE(hu_voice_record_is_memo_send(0, png, 1));
    HU_ASSERT_FALSE(hu_voice_record_is_memo_send(0, two, 2));
    HU_ASSERT_FALSE(hu_voice_record_is_memo_send(0, NULL, 0));
}

static void test_vrec_preflight_ok_when_all_facts_hold(void) {
    hu_voice_record_facts_t f = all_ok();
    HU_ASSERT_EQ(hu_voice_record_preflight(&f), HU_VREC_OK);
}

static void test_vrec_preflight_blocks_each_fact(void) {
    hu_voice_record_facts_t f;
    f = all_ok();
    f.ax_trusted = false;
    HU_ASSERT_EQ(hu_voice_record_preflight(&f), HU_VREC_NO_AX);
    f = all_ok();
    f.messages_running = false;
    HU_ASSERT_EQ(hu_voice_record_preflight(&f), HU_VREC_NO_MESSAGES);
    f = all_ok();
    f.blackhole_present = false;
    HU_ASSERT_EQ(hu_voice_record_preflight(&f), HU_VREC_NO_BLACKHOLE);
    f = all_ok();
    f.real_mic_configured = false;
    HU_ASSERT_EQ(hu_voice_record_preflight(&f), HU_VREC_NO_REAL_MIC);
    f = all_ok();
    f.real_mic_present = false;
    HU_ASSERT_EQ(hu_voice_record_preflight(&f), HU_VREC_NO_REAL_MIC);
    f = all_ok();
    f.real_mic_busy = true;
    HU_ASSERT_EQ(hu_voice_record_preflight(&f), HU_VREC_MIC_BUSY);
    f = all_ok();
    f.user_idle_sec = 5.0;
    HU_ASSERT_EQ(hu_voice_record_preflight(&f), HU_VREC_USER_ACTIVE);
    HU_ASSERT_EQ(hu_voice_record_preflight(NULL), HU_VREC_NO_AX);
}

static void test_vrec_timing_stays_in_human_ranges(void) {
    for (uint32_t s = 0; s < 500; s++) {
        hu_voice_record_timing_t t;
        hu_voice_record_timing(s * 2654435761u, &t);
        HU_ASSERT_TRUE(t.lead_in_ms >= 350 && t.lead_in_ms <= 700);
        HU_ASSERT_TRUE(t.tail_ms >= 500 && t.tail_ms <= 900);
    }
}

static void test_vrec_block_names_are_distinct(void) {
    HU_ASSERT_STR_EQ(hu_voice_record_block_name(HU_VREC_OK), "ok");
    HU_ASSERT_STR_EQ(hu_voice_record_block_name(HU_VREC_MIC_BUSY), "mic_busy");
    HU_ASSERT_STR_EQ(hu_voice_record_block_name(HU_VREC_USER_ACTIVE), "user_active");
}

static void test_vrec_route_only_memo_sends_in_non_attachment_modes(void) {
    const char *caf[] = {"/tmp/Audio Message.caf"};
    HU_ASSERT_EQ(hu_voice_record_route(HU_VOICE_DELIVERY_ATTACHMENT, 0, caf, 1),
                 HU_VREC_ROUTE_ATTACHMENT);
    HU_ASSERT_EQ(hu_voice_record_route(HU_VOICE_DELIVERY_SHADOW, 0, caf, 1), HU_VREC_ROUTE_SHADOW);
    HU_ASSERT_EQ(hu_voice_record_route(HU_VOICE_DELIVERY_MESSAGES, 0, caf, 1),
                 HU_VREC_ROUTE_RECORD);
    HU_ASSERT_EQ(hu_voice_record_route(HU_VOICE_DELIVERY_MESSAGES, 4, caf, 1),
                 HU_VREC_ROUTE_ATTACHMENT);
    HU_ASSERT_EQ(hu_voice_record_route(HU_VOICE_DELIVERY_MESSAGES, 4, NULL, 0),
                 HU_VREC_ROUTE_ATTACHMENT);
}

/* ── Orchestrator: a fake port records every call so the order and the
 * restore-on-every-path contract are asserted exactly. ─────────────────── */
typedef struct {
    char trace[1024];
    const char *fail_at; /* call name that fails */
    hu_voice_record_facts_t facts;
    char input[64];
    bool row_found;
    int send_presses;      /* Send presses so far */
    int record_back_after; /* compose bar returns to Record after this many Sends; 0 = never */
} fake_port_t;

static void fp_log(fake_port_t *f, const char *s) {
    strncat(f->trace, s, sizeof(f->trace) - strlen(f->trace) - 2);
    strncat(f->trace, ",", sizeof(f->trace) - strlen(f->trace) - 1);
}
static bool fp_fail(const fake_port_t *f, const char *s) {
    return f->fail_at && strcmp(f->fail_at, s) == 0;
}
static hu_error_t fp_gather(void *c, const char *mic, hu_voice_record_facts_t *o) {
    (void)mic;
    fake_port_t *f = c;
    fp_log(f, "facts");
    *o = f->facts;
    return HU_OK;
}
static hu_error_t fp_set_input(void *c, const char *d) {
    fake_port_t *f = c;
    fp_log(f, strcmp(d, HU_VREC_BLACKHOLE_NAME) == 0 ? "in:bh" : "in:real");
    if (!fp_fail(f, "set_input"))
        snprintf(f->input, sizeof(f->input), "%s", d);
    return HU_OK;
}
static hu_error_t fp_get_input(void *c, char *b, size_t n) {
    fake_port_t *f = c;
    snprintf(b, n, "%s", f->input);
    return HU_OK;
}
static hu_error_t fp_remember(void *c) {
    fp_log(c, "remember");
    return HU_OK;
}
static void fp_restore_ui(void *c) {
    fp_log(c, "restore_ui");
}
static hu_error_t fp_open(void *c, const char *h, size_t n) {
    (void)h;
    (void)n;
    fp_log(c, "open");
    return HU_OK;
}
static hu_error_t fp_press(void *c, const char *label) {
    fake_port_t *f = c;
    if (strcmp(label, HU_VREC_LABEL_SEND) == 0)
        f->send_presses++;
    char b[48];
    snprintf(b, sizeof(b), "press:%s", label);
    fp_log(c, b);
    return HU_OK;
}
static bool fp_wait(void *c, const char *label, uint32_t timeout_ms) {
    (void)timeout_ms;
    fake_port_t *f = c;
    /* After a Send, "Record audio" reappearing is the UI's proof the memo left. */
    if (strcmp(label, HU_VREC_LABEL_RECORD) == 0 && f->send_presses > 0)
        return f->record_back_after > 0 && f->send_presses >= f->record_back_after;
    char b[48];
    snprintf(b, sizeof(b), "wait:%s", label);
    return !fp_fail(f, b);
}
static hu_error_t fp_prep(void *c, const char *path) {
    (void)path;
    fp_log(c, "prep");
    return HU_OK;
}
static hu_error_t fp_run(void *c) {
    fp_log(c, "play");
    return fp_fail(c, "play") ? HU_ERR_IO : HU_OK;
}
static void fp_dispose(void *c) {
    fp_log(c, "dispose");
}
static void fp_sleep(void *c, uint32_t ms) {
    if (ms == HU_VREC_SEND_SETTLE_MS)
        fp_log(c, "settle");
}
static int64_t fp_rowid(void *c) {
    (void)c;
    return 73000;
}
static bool fp_row(void *c, const char *h, size_t n, int64_t after, uint32_t timeout_ms) {
    (void)h;
    (void)n;
    (void)after;
    (void)timeout_ms;
    return ((fake_port_t *)c)->row_found;
}

static hu_voice_record_port_t fake_port(fake_port_t *f) {
    hu_voice_record_port_t p = {f,           fp_gather,     fp_set_input, fp_get_input,
                                fp_remember, fp_restore_ui, fp_open,      fp_press,
                                fp_wait,     fp_prep,       fp_run,       fp_dispose,
                                fp_sleep,    fp_rowid,      fp_row};
    return p;
}

static fake_port_t fake_ok(void) {
    fake_port_t f;
    memset(&f, 0, sizeof(f));
    f.facts = all_ok();
    f.row_found = true;
    f.record_back_after = 1;
    snprintf(f.input, sizeof(f.input), "Shure MV7");
    return f;
}

static hu_voice_record_request_t req_ok(void) {
    hu_voice_record_request_t r = {"+15550000001", 12, "/tmp/a.caf", "Shure MV7", 20.0, 7};
    return r;
}

static void test_vrec_send_happy_path_order(void) {
    fake_port_t f = fake_ok();
    hu_voice_record_port_t p = fake_port(&f);
    hu_voice_record_request_t r = req_ok();
    hu_voice_record_result_t res;
    HU_ASSERT_EQ(hu_voice_record_send(&p, &r, &res), HU_OK);
    HU_ASSERT_STR_EQ(f.trace, "facts,remember,prep,in:bh,open,press:Record audio,play,"
                              "press:Stop,settle,press:Send,dispose,in:real,restore_ui,");
    HU_ASSERT_TRUE(res.verified);
    HU_ASSERT_TRUE(res.restored);
    HU_ASSERT_EQ(res.stage, HU_VREC_STAGE_SENT);
    HU_ASSERT_EQ(res.prior_max_rowid, 73000);
}

static void test_vrec_send_mic_busy_touches_nothing(void) {
    fake_port_t f = fake_ok();
    f.facts.real_mic_busy = true;
    hu_voice_record_port_t p = fake_port(&f);
    hu_voice_record_request_t r = req_ok();
    hu_voice_record_result_t res;
    HU_ASSERT_EQ(hu_voice_record_send(&p, &r, &res), HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_STR_EQ(f.trace, "facts,");
    HU_ASSERT_EQ(res.block, HU_VREC_MIC_BUSY);
}

static void test_vrec_send_input_readback_mismatch_aborts_before_record(void) {
    fake_port_t f = fake_ok();
    f.fail_at = "set_input";
    hu_voice_record_port_t p = fake_port(&f);
    hu_voice_record_request_t r = req_ok();
    hu_voice_record_result_t res;
    HU_ASSERT_EQ(hu_voice_record_send(&p, &r, &res), HU_ERR_IO);
    HU_ASSERT_TRUE(strstr(f.trace, "press:Record audio") == NULL);
    HU_ASSERT_TRUE(strstr(f.trace, "restore_ui") != NULL);
}

static void test_vrec_send_playback_failure_cancels_and_restores(void) {
    fake_port_t f = fake_ok();
    f.fail_at = "play";
    hu_voice_record_port_t p = fake_port(&f);
    hu_voice_record_request_t r = req_ok();
    hu_voice_record_result_t res;
    HU_ASSERT_EQ(hu_voice_record_send(&p, &r, &res), HU_ERR_IO);
    HU_ASSERT_TRUE(strstr(f.trace, "press:Cancel audio recording") != NULL);
    HU_ASSERT_TRUE(strstr(f.trace, "press:Send") == NULL);
    HU_ASSERT_TRUE(strstr(f.trace, "in:real,restore_ui,") != NULL);
    HU_ASSERT_TRUE(res.restored);
}

static void test_vrec_send_missing_send_button_cancels(void) {
    fake_port_t f = fake_ok();
    f.fail_at = "wait:Send";
    hu_voice_record_port_t p = fake_port(&f);
    hu_voice_record_request_t r = req_ok();
    hu_voice_record_result_t res;
    HU_ASSERT_EQ(hu_voice_record_send(&p, &r, &res), HU_ERR_IO);
    HU_ASSERT_TRUE(strstr(f.trace, "press:Cancel audio recording") != NULL);
}

static void test_vrec_send_unverified_row_is_ok_not_resent(void) {
    fake_port_t f = fake_ok();
    f.row_found = false;
    hu_voice_record_port_t p = fake_port(&f);
    hu_voice_record_request_t r = req_ok();
    hu_voice_record_result_t res;
    HU_ASSERT_EQ(hu_voice_record_send(&p, &r, &res), HU_OK); /* caller must NOT fall back */
    HU_ASSERT_FALSE(res.verified);
}

static void test_vrec_send_restore_readback_failure_is_reported(void) {
    /* set_input silently does nothing and the input starts on BlackHole, so the
     * recording proceeds but restore cannot bring the real mic back. */
    fake_port_t f = fake_ok();
    f.fail_at = "set_input";
    snprintf(f.input, sizeof(f.input), HU_VREC_BLACKHOLE_NAME);
    hu_voice_record_port_t p = fake_port(&f);
    hu_voice_record_request_t r = req_ok();
    hu_voice_record_result_t res;
    (void)hu_voice_record_send(&p, &r, &res);
    HU_ASSERT_FALSE(res.restored);
}

/* Live test 2026-09-27: a Send pressed right after Stop was silently ignored
 * and the memo sat unsent in the compose bar. */
static void test_vrec_send_ignored_first_press_is_retried(void) {
    fake_port_t f = fake_ok();
    f.record_back_after = 2;
    hu_voice_record_port_t p = fake_port(&f);
    hu_voice_record_request_t r = req_ok();
    hu_voice_record_result_t res;
    HU_ASSERT_EQ(hu_voice_record_send(&p, &r, &res), HU_OK);
    HU_ASSERT_EQ(f.send_presses, 2);
    HU_ASSERT_TRUE(strstr(f.trace, "press:Cancel audio recording") == NULL);
    HU_ASSERT_EQ(res.stage, HU_VREC_STAGE_SENT);
}

static void test_vrec_send_never_confirmed_cancels_and_falls_back(void) {
    fake_port_t f = fake_ok();
    f.record_back_after = 0;
    f.row_found = false;
    hu_voice_record_port_t p = fake_port(&f);
    hu_voice_record_request_t r = req_ok();
    hu_voice_record_result_t res;
    HU_ASSERT_EQ(hu_voice_record_send(&p, &r, &res), HU_ERR_IO); /* attachment fallback */
    HU_ASSERT_EQ(f.send_presses, 2);
    HU_ASSERT_TRUE(strstr(f.trace, "press:Cancel audio recording") != NULL);
    HU_ASSERT_TRUE(res.restored);
}

static void test_vrec_send_ui_unconfirmed_but_in_chatdb_counts_as_sent(void) {
    fake_port_t f = fake_ok();
    f.record_back_after = 0;
    f.row_found = true;
    hu_voice_record_port_t p = fake_port(&f);
    hu_voice_record_request_t r = req_ok();
    hu_voice_record_result_t res;
    HU_ASSERT_EQ(hu_voice_record_send(&p, &r, &res), HU_OK); /* never resend */
    HU_ASSERT_TRUE(res.verified);
    HU_ASSERT_TRUE(strstr(f.trace, "press:Cancel audio recording") == NULL);
}

/* The test binary never touches audio/AX: the macOS port is a stub here that
 * always blocks at preflight, and none of its members is NULL. */
static void test_vrec_macos_port_blocks_under_test(void) {
    const hu_voice_record_port_t *p = hu_voice_record_macos_port();
    HU_ASSERT_NOT_NULL(p);
    HU_ASSERT_NOT_NULL(p->set_input);
    HU_ASSERT_NOT_NULL(p->audio_row_after);
    hu_voice_record_request_t r = req_ok();
    hu_voice_record_result_t res;
    HU_ASSERT_EQ(hu_voice_record_send(p, &r, &res), HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_EQ(res.block, HU_VREC_NO_AX);
}

/* The request the daemon and the CLI both build from the environment. */
static void test_vrec_request_from_env_reads_mic_and_idle(void) {
    const char *old_mic = getenv("HU_VOICE_REAL_INPUT");
    const char *old_idle = getenv("HU_VOICE_MIN_IDLE_SEC");
    char saved_mic[128] = {0}, saved_idle[32] = {0};
    if (old_mic)
        snprintf(saved_mic, sizeof(saved_mic), "%s", old_mic);
    if (old_idle)
        snprintf(saved_idle, sizeof(saved_idle), "%s", old_idle);

    setenv("HU_VOICE_REAL_INPUT", "Shure MV7", 1);
    setenv("HU_VOICE_MIN_IDLE_SEC", "45", 1);
    hu_voice_record_request_t r;
    hu_voice_record_request_from_env("+15550000001", 12, "/tmp/a.caf", 9, &r);
    HU_ASSERT_STR_EQ(r.real_mic, "Shure MV7");
    HU_ASSERT_TRUE(r.min_idle_sec > 44.9 && r.min_idle_sec < 45.1);
    HU_ASSERT_STR_EQ(r.audio_path, "/tmp/a.caf");
    HU_ASSERT_EQ(r.handle_len, 12);
    HU_ASSERT_EQ(r.seed, 9u);

    unsetenv("HU_VOICE_REAL_INPUT");
    unsetenv("HU_VOICE_MIN_IDLE_SEC");
    hu_voice_record_request_from_env("+15550000001", 12, "/tmp/a.caf", 9, &r);
    HU_ASSERT_STR_EQ(r.real_mic, ""); /* unset -> preflight blocks as no_real_mic */
    HU_ASSERT_TRUE(r.min_idle_sec > 19.9 && r.min_idle_sec < 20.1);

    if (old_mic)
        setenv("HU_VOICE_REAL_INPUT", saved_mic, 1);
    if (old_idle)
        setenv("HU_VOICE_MIN_IDLE_SEC", saved_idle, 1);
}

static void test_vrec_send_from_env_blocks_under_test(void) {
    hu_voice_record_result_t res;
    HU_ASSERT_EQ(hu_voice_record_send_from_env("+15550000001", 12, "/tmp/a.caf", &res),
                 HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_EQ(res.block, HU_VREC_NO_AX);
}

void run_imessage_voice_record_tests(void) {
    HU_TEST_SUITE("imessage voice record");
    HU_RUN_TEST(test_vrec_mode_parse_defaults_to_attachment);
    HU_RUN_TEST(test_vrec_memo_send_requires_empty_text_and_one_audio);
    HU_RUN_TEST(test_vrec_preflight_ok_when_all_facts_hold);
    HU_RUN_TEST(test_vrec_preflight_blocks_each_fact);
    HU_RUN_TEST(test_vrec_timing_stays_in_human_ranges);
    HU_RUN_TEST(test_vrec_block_names_are_distinct);
    HU_RUN_TEST(test_vrec_route_only_memo_sends_in_non_attachment_modes);
    HU_RUN_TEST(test_vrec_send_happy_path_order);
    HU_RUN_TEST(test_vrec_send_mic_busy_touches_nothing);
    HU_RUN_TEST(test_vrec_send_input_readback_mismatch_aborts_before_record);
    HU_RUN_TEST(test_vrec_send_playback_failure_cancels_and_restores);
    HU_RUN_TEST(test_vrec_send_missing_send_button_cancels);
    HU_RUN_TEST(test_vrec_send_unverified_row_is_ok_not_resent);
    HU_RUN_TEST(test_vrec_send_restore_readback_failure_is_reported);
    HU_RUN_TEST(test_vrec_send_ignored_first_press_is_retried);
    HU_RUN_TEST(test_vrec_send_never_confirmed_cancels_and_falls_back);
    HU_RUN_TEST(test_vrec_send_ui_unconfirmed_but_in_chatdb_counts_as_sent);
    HU_RUN_TEST(test_vrec_macos_port_blocks_under_test);
    HU_RUN_TEST(test_vrec_request_from_env_reads_mic_and_idle);
    HU_RUN_TEST(test_vrec_send_from_env_blocks_under_test);
}
