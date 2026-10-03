/* Commitment guard (src/daemon/daemon_commitment_guard.c): a drafted reply that
 * commits Seth to a plan, money, a favour or a sensitive decision is checked
 * against a LOCAL detector and his calendar before it is sent. Hermetic: a
 * fake detector provider and a fake calendar stand in for the local model and
 * the EventKit helper. */
#include "human/agent.h"
#include "human/core/allocator.h"
#include "human/core/llm_purpose.h"
#include "human/core/string.h"
#include "human/daemon/calendar_free_busy.h"
#include "human/daemon/commitment_guard.h"
#include "human/daemon/owner_notify.h"
#include "human/providers/compatible.h"
#include "test_framework.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── fakes ────────────────────────────────────────────────────────────── */

typedef struct fake_llm {
    const char *replies[4]; /* NULL entry = that call fails */
    size_t n;
    size_t calls;
    char purpose_seen[4][32];
} fake_llm_t;

static hu_error_t fake_chat(void *ctx, hu_allocator_t *alloc, const char *sys, size_t sys_len,
                            const char *msg, size_t msg_len, const char *model, size_t model_len,
                            double temperature, char **out, size_t *out_len) {
    (void)sys;
    (void)sys_len;
    (void)msg;
    (void)msg_len;
    (void)model;
    (void)model_len;
    (void)temperature;
    fake_llm_t *f = (fake_llm_t *)ctx;
    size_t i = f->calls++;
    if (i < 4) {
        hu_llm_purpose_t cur = hu_llm_purpose_current();
        snprintf(f->purpose_seen[i], sizeof(f->purpose_seen[i]), "%s",
                 cur == HU_LLM_PURPOSE_UNTAGGED ? "" : hu_llm_purpose_name(cur));
    }
    if (i >= f->n || !f->replies[i])
        return HU_ERR_INTERNAL;
    size_t len = strlen(f->replies[i]);
    *out = hu_strndup(alloc, f->replies[i], len);
    *out_len = len;
    return *out ? HU_OK : HU_ERR_OUT_OF_MEMORY;
}

static const hu_provider_vtable_t fake_vtable = {.chat_with_system = fake_chat};

typedef struct fake_cal {
    hu_calendar_state_t state;
    unsigned calls;
    int64_t start, end;
} fake_cal_t;

static hu_calendar_state_t fake_calendar(void *ctx, int64_t start, int64_t end) {
    fake_cal_t *c = (fake_cal_t *)ctx;
    c->calls++;
    c->start = start;
    c->end = end;
    return c->state;
}

static int64_t local_unix(int y, int mo, int d, int h, int mi) {
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = y - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_isdst = -1;
    return (int64_t)mktime(&t);
}

static const char k_plan_sat7[] =
    "{\"kind\":\"plan\",\"stakes\":\"low\",\"when\":\"2026-10-03T19:00\",\"confidence\":0.9}";
static const char k_money[] =
    "{\"kind\":\"money\",\"stakes\":\"high\",\"when\":null,\"confidence\":0.85}";
static const char k_none[] = "{\"kind\":\"none\",\"confidence\":0.95}";
static const char k_rewrite[] = "ah i think i've got something then, lemme check";

static hu_allocator_t g_alloc;

static hu_commitment_guard_io_t make_io(fake_llm_t *f, fake_cal_t *c) {
    g_alloc = hu_system_allocator();
    hu_commitment_guard_io_t io;
    memset(&io, 0, sizeof(io));
    io.alloc = &g_alloc;
    io.local.ctx = f;
    io.local.vtable = f ? &fake_vtable : NULL;
    io.model = "local";
    io.model_len = 5;
    io.calendar = c ? fake_calendar : NULL;
    io.calendar_ctx = c;
    io.contact_name = "Lexi";
    io.now = local_unix(2026, 10, 2, 12, 0);
    return io;
}

static char *dup_draft(const char *s, size_t *len) {
    *len = strlen(s);
    return hu_strndup(&g_alloc, s, *len);
}

/* ── prefilter ────────────────────────────────────────────────────────── */

static bool pf(const char *draft, const char *inbound) {
    return hu_commitment_prefilter(draft, strlen(draft), inbound, inbound ? strlen(inbound) : 0);
}

static void prefilter_fires_on_plans_times_money_and_promises(void) {
    HU_ASSERT_TRUE(pf("yeah saturday works", NULL));
    HU_ASSERT_TRUE(pf("i'll be there at 7", NULL));
    HU_ASSERT_TRUE(pf("I\xE2\x80\x99ll be there by 7pm", NULL)); /* typographic apostrophe */
    HU_ASSERT_TRUE(pf("sure i can venmo you $40", NULL));
    HU_ASSERT_TRUE(pf("tmrw night is good", NULL));
    HU_ASSERT_TRUE(pf("count me in", NULL));
    HU_ASSERT_TRUE(pf("i promise", NULL));
}

static void prefilter_fires_on_a_bare_yes_to_a_request(void) {
    HU_ASSERT_TRUE(pf("yeah", "can you lend me 50 till friday?"));
    HU_ASSERT_TRUE(pf("sure", "could you pick me up from the airport"));
    HU_ASSERT_TRUE(pf("ok go ahead", "is it ok if i tell mom about it"));
}

static void prefilter_skips_chat_with_no_commitment_cue(void) {
    HU_ASSERT_FALSE(pf("lol that's wild", "my cat just knocked over the tree"));
    HU_ASSERT_FALSE(pf("haha yeah", "that movie was so bad"));
    HU_ASSERT_FALSE(pf("nice", "i got a new dog"));
    HU_ASSERT_FALSE(pf("yeah", "that movie was so bad"));
    HU_ASSERT_FALSE(hu_commitment_prefilter(NULL, 0, NULL, 0));
}

static void money_floor_matches_offers_to_pay_only(void) {
    HU_ASSERT_TRUE(hu_commitment_money_floor("ill venmo you tonight", 21));
    HU_ASSERT_TRUE(hu_commitment_money_floor("I can lend you $200", 19));
    HU_ASSERT_FALSE(hu_commitment_money_floor("venmo is so annoying lol", 24));
    HU_ASSERT_FALSE(hu_commitment_money_floor("see you saturday", 16));
}

/* ── parse ────────────────────────────────────────────────────────────── */

static void parse_plan_with_time_resolves_a_two_hour_local_window(void) {
    hu_commit_detection_t d;
    HU_ASSERT_TRUE(hu_commitment_parse(k_plan_sat7, strlen(k_plan_sat7), &d));
    HU_ASSERT_EQ((int)d.kind, (int)HU_COMMIT_PLAN);
    HU_ASSERT_TRUE(d.has_when);
    HU_ASSERT_FALSE(d.high_stakes);
    HU_ASSERT_EQ(d.when_start, local_unix(2026, 10, 3, 19, 0));
    HU_ASSERT_EQ(d.when_end, local_unix(2026, 10, 3, 19, 0) + HU_COMMIT_TIMED_WINDOW_S);
    HU_ASSERT_TRUE(d.confidence > 0.89 && d.confidence < 0.91);
}

static void parse_date_only_checks_the_waking_day(void) {
    const char *j = "{\"kind\":\"plan\",\"when\":\"2026-10-03\",\"confidence\":0.8}";
    hu_commit_detection_t d;
    HU_ASSERT_TRUE(hu_commitment_parse(j, strlen(j), &d));
    HU_ASSERT_TRUE(d.has_when);
    HU_ASSERT_EQ(d.when_start, local_unix(2026, 10, 3, 9, 0));
    HU_ASSERT_EQ(d.when_end, local_unix(2026, 10, 3, 22, 0));
}

static void parse_accepts_fenced_json_and_rejects_unknown_kinds(void) {
    const char *fenced = "```json\n{\"kind\":\"money\",\"stakes\":\"low\",\"confidence\":0.7}\n```";
    hu_commit_detection_t d;
    HU_ASSERT_TRUE(hu_commitment_parse(fenced, strlen(fenced), &d));
    HU_ASSERT_EQ((int)d.kind, (int)HU_COMMIT_MONEY);
    HU_ASSERT_TRUE(d.high_stakes); /* money is never low-stakes */
    HU_ASSERT_FALSE(d.has_when);
    const char *bad = "{\"kind\":\"vibes\",\"confidence\":0.9}";
    HU_ASSERT_FALSE(hu_commitment_parse(bad, strlen(bad), &d));
    HU_ASSERT_FALSE(hu_commitment_parse("no json here", 12, &d));
    const char *favour = "{\"kind\":\"favour\",\"confidence\":0.9}"; /* stakes missing */
    HU_ASSERT_TRUE(hu_commitment_parse(favour, strlen(favour), &d));
    HU_ASSERT_TRUE(d.high_stakes); /* missing stakes fails toward caution */
}

/* ── decide ───────────────────────────────────────────────────────────── */

static hu_commit_detection_t det(hu_commit_kind_t k, bool high, bool when, double conf) {
    hu_commit_detection_t d;
    memset(&d, 0, sizeof(d));
    d.kind = k;
    d.high_stakes = high;
    d.has_when = when;
    d.confidence = conf;
    return d;
}

static void decide_table_covers_every_branch(void) {
    hu_commit_detection_t d = det(HU_COMMIT_PLAN, false, true, 0.9);
    HU_ASSERT_EQ((int)hu_commitment_decide(&d, HU_CAL_BUSY), (int)HU_COMMIT_REWRITE_CONFLICT);
    HU_ASSERT_EQ((int)hu_commitment_decide(&d, HU_CAL_FREE), (int)HU_COMMIT_ALLOW);
    HU_ASSERT_EQ((int)hu_commitment_decide(&d, HU_CAL_UNKNOWN), (int)HU_COMMIT_HOLD);
    d = det(HU_COMMIT_PLAN, false, false, 0.9); /* no time to check */
    HU_ASSERT_EQ((int)hu_commitment_decide(&d, HU_CAL_NOT_CHECKED), (int)HU_COMMIT_HOLD);
    d = det(HU_COMMIT_MONEY, true, false, 0.9);
    HU_ASSERT_EQ((int)hu_commitment_decide(&d, HU_CAL_FREE), (int)HU_COMMIT_HOLD);
    d = det(HU_COMMIT_SENSITIVE, true, false, 0.9);
    HU_ASSERT_EQ((int)hu_commitment_decide(&d, HU_CAL_NOT_CHECKED), (int)HU_COMMIT_HOLD);
    d = det(HU_COMMIT_FAVOUR, true, false, 0.9);
    HU_ASSERT_EQ((int)hu_commitment_decide(&d, HU_CAL_NOT_CHECKED), (int)HU_COMMIT_HOLD);
    d = det(HU_COMMIT_FAVOUR, false, true, 0.9);
    HU_ASSERT_EQ((int)hu_commitment_decide(&d, HU_CAL_BUSY), (int)HU_COMMIT_REWRITE_CONFLICT);
    HU_ASSERT_EQ((int)hu_commitment_decide(&d, HU_CAL_FREE), (int)HU_COMMIT_ALLOW);
    d = det(HU_COMMIT_MONEY, true, false, 0.4); /* below the confidence floor */
    HU_ASSERT_EQ((int)hu_commitment_decide(&d, HU_CAL_NOT_CHECKED), (int)HU_COMMIT_ALLOW);
    d = det(HU_COMMIT_NONE, false, false, 0.99);
    HU_ASSERT_EQ((int)hu_commitment_decide(&d, HU_CAL_NOT_CHECKED), (int)HU_COMMIT_ALLOW);
}

/* ── calendar JSON ────────────────────────────────────────────────────── */

static void calendar_parse_overlap_busy_gap_free_and_no_access_unknown(void) {
    hu_allocator_t a = hu_system_allocator();
    const char *j = "{\"access\":\"granted\",\"busy\":[{\"start\":100,\"end\":200}]}";
    HU_ASSERT_EQ((int)hu_calendar_free_busy_parse(&a, j, strlen(j), 150, 300), (int)HU_CAL_BUSY);
    HU_ASSERT_EQ((int)hu_calendar_free_busy_parse(&a, j, strlen(j), 200, 300), (int)HU_CAL_FREE);
    HU_ASSERT_EQ((int)hu_calendar_free_busy_parse(&a, j, strlen(j), 0, 100), (int)HU_CAL_FREE);
    const char *empty = "{\"access\":\"granted\",\"busy\":[]}";
    HU_ASSERT_EQ((int)hu_calendar_free_busy_parse(&a, empty, strlen(empty), 0, 100),
                 (int)HU_CAL_FREE);
    const char *denied = "{\"access\":\"denied\"}";
    HU_ASSERT_EQ((int)hu_calendar_free_busy_parse(&a, denied, strlen(denied), 0, 100),
                 (int)HU_CAL_UNKNOWN);
    HU_ASSERT_EQ((int)hu_calendar_free_busy_parse(&a, "garbage", 7, 0, 100), (int)HU_CAL_UNKNOWN);
    HU_ASSERT_EQ((int)hu_calendar_free_busy_parse(&a, NULL, 0, 0, 100), (int)HU_CAL_UNKNOWN);
}

static void calendar_query_never_spawns_in_tests(void) {
    HU_ASSERT_EQ((int)hu_calendar_free_busy_query(NULL, 0, 100), (int)HU_CAL_UNKNOWN);
}

/* ── run: OFF / SHADOW / LIVE ─────────────────────────────────────────── */

static void run_off_is_byte_identical_and_calls_nothing(void) {
    fake_llm_t f = {.replies = {k_plan_sat7, k_rewrite, k_none}, .n = 3};
    fake_cal_t c = {.state = HU_CAL_BUSY};
    hu_commitment_guard_io_t io = make_io(&f, &c);
    hu_owner_notify_test_reset();
    size_t len;
    char *draft = dup_draft("yeah saturday at 7 works", &len);
    char *before = draft;
    hu_commitment_guard_result_t r;
    HU_ASSERT_EQ(hu_commitment_guard_run(HU_GATE_OFF, &io, "u free sat?", 11, &draft, &len, &r),
                 HU_OK);
    HU_ASSERT_TRUE(draft == before);
    HU_ASSERT_EQ(len, strlen("yeah saturday at 7 works"));
    HU_ASSERT_STR_EQ(draft, "yeah saturday at 7 works");
    HU_ASSERT_EQ(f.calls, 0u);
    HU_ASSERT_EQ(c.calls, 0u);
    HU_ASSERT_EQ(hu_owner_notify_test_count(), 0u);
    HU_ASSERT_EQ((int)r.decision, (int)HU_COMMIT_SKIP);
    g_alloc.free(g_alloc.ctx, draft, len + 1);
}

static void run_shadow_computes_conflict_but_sends_the_draft_unchanged(void) {
    fake_llm_t f = {.replies = {k_plan_sat7, k_rewrite, k_none}, .n = 3};
    fake_cal_t c = {.state = HU_CAL_BUSY};
    hu_commitment_guard_io_t io = make_io(&f, &c);
    hu_owner_notify_test_reset();
    size_t len;
    char *draft = dup_draft("yeah saturday at 7 works", &len);
    char *before = draft;
    hu_commitment_guard_result_t r;
    HU_ASSERT_EQ(hu_commitment_guard_run(HU_GATE_SHADOW, &io, "u free sat?", 11, &draft, &len, &r),
                 HU_OK);
    HU_ASSERT_EQ((int)r.decision, (int)HU_COMMIT_REWRITE_CONFLICT);
    HU_ASSERT_EQ((int)r.calendar, (int)HU_CAL_BUSY);
    HU_ASSERT_TRUE(r.prefilter);
    HU_ASSERT_TRUE(r.detector_ok);
    HU_ASSERT_TRUE(draft == before);
    HU_ASSERT_STR_EQ(draft, "yeah saturday at 7 works");
    HU_ASSERT_EQ(f.calls, 1u); /* detector only: shadow never rewrites */
    HU_ASSERT_FALSE(r.rewritten);
    HU_ASSERT_EQ(hu_owner_notify_test_count(), 0u);
    g_alloc.free(g_alloc.ctx, draft, len + 1);
}

static void run_live_conflict_rewrites_and_notifies_owner(void) {
    fake_llm_t f = {.replies = {k_plan_sat7, k_rewrite, k_none}, .n = 3};
    fake_cal_t c = {.state = HU_CAL_BUSY};
    hu_commitment_guard_io_t io = make_io(&f, &c);
    hu_owner_notify_test_reset();
    size_t len;
    char *draft = dup_draft("yeah saturday at 7 works", &len);
    hu_commitment_guard_result_t r;
    HU_ASSERT_EQ(hu_commitment_guard_run(HU_GATE_LIVE, &io, "u free sat?", 11, &draft, &len, &r),
                 HU_OK);
    HU_ASSERT_EQ((int)r.decision, (int)HU_COMMIT_REWRITE_CONFLICT);
    HU_ASSERT_TRUE(r.rewritten);
    HU_ASSERT_FALSE(r.suppressed);
    HU_ASSERT_NOT_NULL(draft);
    HU_ASSERT_STR_EQ(draft, k_rewrite);
    HU_ASSERT_EQ(len, strlen(k_rewrite));
    HU_ASSERT_EQ(c.calls, 1u);
    HU_ASSERT_EQ(c.start, local_unix(2026, 10, 3, 19, 0));
    HU_ASSERT_EQ(hu_owner_notify_test_count(), 1u);
    const char *body = hu_owner_notify_test_last_body();
    HU_ASSERT_TRUE(strstr(body, "Lexi") != NULL);
    HU_ASSERT_TRUE(strstr(body, "plan") != NULL);
    HU_ASSERT_TRUE(strstr(body, "saturday") == NULL); /* never quotes the draft */
    g_alloc.free(g_alloc.ctx, draft, len + 1);
}

static void run_live_money_holds_without_a_calendar_check(void) {
    fake_llm_t f = {.replies = {k_money, "ah lemme see what i've got this week", k_none}, .n = 3};
    fake_cal_t c = {.state = HU_CAL_FREE};
    hu_commitment_guard_io_t io = make_io(&f, &c);
    hu_owner_notify_test_reset();
    size_t len;
    char *draft = dup_draft("yeah ill send it tonight", &len);
    hu_commitment_guard_result_t r;
    const char *in = "can you lend me $200 till friday";
    HU_ASSERT_EQ(hu_commitment_guard_run(HU_GATE_LIVE, &io, in, strlen(in), &draft, &len, &r),
                 HU_OK);
    HU_ASSERT_EQ((int)r.decision, (int)HU_COMMIT_HOLD);
    HU_ASSERT_EQ(c.calls, 0u);
    HU_ASSERT_TRUE(r.rewritten);
    HU_ASSERT_STR_EQ(draft, "ah lemme see what i've got this week");
    HU_ASSERT_EQ(hu_owner_notify_test_count(), 1u);
    HU_ASSERT_TRUE(strstr(hu_owner_notify_test_last_body(), "money") != NULL);
    g_alloc.free(g_alloc.ctx, draft, len + 1);
}

static void run_live_free_low_stakes_plan_is_allowed(void) {
    fake_llm_t f = {.replies = {k_plan_sat7}, .n = 1};
    fake_cal_t c = {.state = HU_CAL_FREE};
    hu_commitment_guard_io_t io = make_io(&f, &c);
    hu_owner_notify_test_reset();
    size_t len;
    char *draft = dup_draft("yeah saturday at 7 works", &len);
    char *before = draft;
    hu_commitment_guard_result_t r;
    HU_ASSERT_EQ(hu_commitment_guard_run(HU_GATE_LIVE, &io, "u free sat?", 11, &draft, &len, &r),
                 HU_OK);
    HU_ASSERT_EQ((int)r.decision, (int)HU_COMMIT_ALLOW);
    HU_ASSERT_TRUE(draft == before);
    HU_ASSERT_STR_EQ(draft, "yeah saturday at 7 works");
    HU_ASSERT_EQ(f.calls, 1u);
    HU_ASSERT_EQ(hu_owner_notify_test_count(), 0u);
    g_alloc.free(g_alloc.ctx, draft, len + 1);
}

static void run_live_unknown_calendar_holds_a_plan(void) {
    fake_llm_t f = {.replies = {k_plan_sat7, k_rewrite, k_none}, .n = 3};
    hu_commitment_guard_io_t io = make_io(&f, NULL); /* no calendar helper */
    hu_owner_notify_test_reset();
    size_t len;
    char *draft = dup_draft("yeah saturday at 7 works", &len);
    hu_commitment_guard_result_t r;
    HU_ASSERT_EQ(hu_commitment_guard_run(HU_GATE_LIVE, &io, "u free sat?", 11, &draft, &len, &r),
                 HU_OK);
    HU_ASSERT_EQ((int)r.calendar, (int)HU_CAL_UNKNOWN);
    HU_ASSERT_EQ((int)r.decision, (int)HU_COMMIT_HOLD);
    HU_ASSERT_STR_EQ(draft, k_rewrite);
    HU_ASSERT_EQ(hu_owner_notify_test_count(), 1u);
    g_alloc.free(g_alloc.ctx, draft, len + 1);
}

static void run_live_rewrite_that_still_commits_is_suppressed(void) {
    fake_llm_t f = {.replies = {k_plan_sat7, "ok see u at 7 then", k_plan_sat7}, .n = 3};
    fake_cal_t c = {.state = HU_CAL_BUSY};
    hu_commitment_guard_io_t io = make_io(&f, &c);
    hu_owner_notify_test_reset();
    size_t len;
    char *draft = dup_draft("yeah saturday at 7 works", &len);
    hu_commitment_guard_result_t r;
    HU_ASSERT_EQ(hu_commitment_guard_run(HU_GATE_LIVE, &io, "u free sat?", 11, &draft, &len, &r),
                 HU_OK);
    HU_ASSERT_TRUE(r.suppressed);
    HU_ASSERT_NULL(draft);
    HU_ASSERT_EQ(len, 0u);
    HU_ASSERT_EQ(hu_owner_notify_test_count(), 1u);
}

static void run_live_failed_rewrite_is_suppressed(void) {
    fake_llm_t f = {.replies = {k_money, NULL}, .n = 2};
    hu_commitment_guard_io_t io = make_io(&f, NULL);
    hu_owner_notify_test_reset();
    size_t len;
    char *draft = dup_draft("sure ill venmo you", &len);
    hu_commitment_guard_result_t r;
    HU_ASSERT_EQ(hu_commitment_guard_run(HU_GATE_LIVE, &io, "lend me 50?", 11, &draft, &len, &r),
                 HU_OK);
    HU_ASSERT_TRUE(r.suppressed);
    HU_ASSERT_NULL(draft);
    HU_ASSERT_EQ(hu_owner_notify_test_count(), 1u);
}

/* Owner notice fails (osascript down): critic MED on PR #609. The held reply
 * must stay held, never fall back to the committing draft; the notice is
 * retried once; a persistent failure is counted, not silent. */
static unsigned g_notify_calls;
static unsigned g_notify_fail_first;
static bool failing_notify(const char *body) {
    (void)body;
    g_notify_calls++;
    return g_notify_calls > g_notify_fail_first;
}

static void run_live_notify_failure_keeps_reply_held_and_is_counted(void) {
    fake_llm_t f = {.replies = {k_money, NULL}, .n = 2}; /* rewrite fails -> suppress */
    hu_commitment_guard_io_t io = make_io(&f, NULL);
    io.notify = failing_notify;
    g_notify_calls = 0;
    g_notify_fail_first = 100; /* never succeeds */
    hu_commitment_guard_test_reset();
    HU_ASSERT_EQ(hu_commitment_guard_test_notify_failed_count(), 0u);
    hu_owner_notify_test_reset();
    size_t len;
    char *draft = dup_draft("sure ill venmo you", &len);
    hu_commitment_guard_result_t r;
    HU_ASSERT_EQ(hu_commitment_guard_run(HU_GATE_LIVE, &io, "lend me 50?", 11, &draft, &len, &r),
                 HU_OK);
    HU_ASSERT_EQ(g_notify_calls, 2u); /* one try + one retry */
    HU_ASSERT_TRUE(r.suppressed);
    HU_ASSERT_NULL(draft); /* still held: nothing is sent */
    HU_ASSERT_EQ(len, 0u);
    HU_ASSERT_FALSE(r.notified);
    HU_ASSERT_EQ(hu_commitment_guard_test_notify_failed_count(), 1u);
    HU_ASSERT_EQ(hu_owner_notify_test_count(), 0u); /* the injected notifier was used */
}

static void run_live_notify_retry_success_is_not_counted(void) {
    fake_llm_t f = {.replies = {k_money, NULL}, .n = 2};
    hu_commitment_guard_io_t io = make_io(&f, NULL);
    io.notify = failing_notify;
    g_notify_calls = 0;
    g_notify_fail_first = 1; /* first try fails, retry succeeds */
    hu_commitment_guard_test_reset();
    size_t len;
    char *draft = dup_draft("sure ill venmo you", &len);
    hu_commitment_guard_result_t r;
    HU_ASSERT_EQ(hu_commitment_guard_run(HU_GATE_LIVE, &io, "lend me 50?", 11, &draft, &len, &r),
                 HU_OK);
    HU_ASSERT_EQ(g_notify_calls, 2u);
    HU_ASSERT_TRUE(r.notified);
    HU_ASSERT_NULL(draft);
    HU_ASSERT_EQ(hu_commitment_guard_test_notify_failed_count(), 0u);
}

static void run_live_detector_down_falls_back_to_money_floor_only(void) {
    /* Detector fails, draft offers money: the deterministic floor holds it. */
    fake_llm_t f = {.replies = {NULL, k_rewrite, k_none}, .n = 3};
    hu_commitment_guard_io_t io = make_io(&f, NULL);
    hu_owner_notify_test_reset();
    size_t len;
    char *draft = dup_draft("ya ill venmo you tonight", &len);
    hu_commitment_guard_result_t r;
    HU_ASSERT_EQ(hu_commitment_guard_run(HU_GATE_LIVE, &io, "lend me 50?", 11, &draft, &len, &r),
                 HU_OK);
    HU_ASSERT_FALSE(r.detector_ok);
    HU_ASSERT_EQ((int)r.detection.kind, (int)HU_COMMIT_MONEY);
    HU_ASSERT_EQ((int)r.decision, (int)HU_COMMIT_HOLD);
    HU_ASSERT_STR_EQ(draft, k_rewrite);
    g_alloc.free(g_alloc.ctx, draft, len + 1);

    /* Detector fails, no money offer: unchanged, reported as DETECT_FAILED. */
    fake_llm_t f2 = {.replies = {NULL}, .n = 1};
    io = make_io(&f2, NULL);
    hu_owner_notify_test_reset();
    draft = dup_draft("yeah saturday works", &len);
    char *before = draft;
    HU_ASSERT_EQ(hu_commitment_guard_run(HU_GATE_LIVE, &io, "u free sat?", 11, &draft, &len, &r),
                 HU_OK);
    HU_ASSERT_EQ((int)r.decision, (int)HU_COMMIT_DETECT_FAILED);
    HU_ASSERT_TRUE(draft == before);
    HU_ASSERT_EQ(hu_owner_notify_test_count(), 0u);
    g_alloc.free(g_alloc.ctx, draft, len + 1);
}

static void run_live_prefilter_miss_never_calls_the_model(void) {
    fake_llm_t f = {.replies = {k_money}, .n = 1};
    hu_commitment_guard_io_t io = make_io(&f, NULL);
    hu_commitment_guard_test_reset();
    size_t len;
    char *draft = dup_draft("lol that's wild", &len);
    char *before = draft;
    hu_commitment_guard_result_t r;
    const char *in = "my cat knocked the tree over";
    HU_ASSERT_EQ(hu_commitment_guard_run(HU_GATE_LIVE, &io, in, strlen(in), &draft, &len, &r),
                 HU_OK);
    HU_ASSERT_EQ((int)r.decision, (int)HU_COMMIT_SKIP);
    HU_ASSERT_EQ(f.calls, 0u);
    HU_ASSERT_TRUE(draft == before);
    g_alloc.free(g_alloc.ctx, draft, len + 1);
}

static void run_shadow_audits_every_nth_prefilter_miss(void) {
    fake_llm_t f = {.replies = {k_none}, .n = 1};
    hu_commitment_guard_io_t io = make_io(&f, NULL);
    hu_commitment_guard_test_reset();
    size_t audited = 0;
    for (int i = 0; i < HU_COMMIT_AUDIT_EVERY; i++) {
        size_t len;
        char *draft = dup_draft("lol that's wild", &len);
        hu_commitment_guard_result_t r;
        f.calls = 0;
        HU_ASSERT_EQ(hu_commitment_guard_run(HU_GATE_SHADOW, &io, "haha", 4, &draft, &len, &r),
                     HU_OK);
        HU_ASSERT_STR_EQ(draft, "lol that's wild");
        if (r.audited) {
            audited++;
            HU_ASSERT_EQ(f.calls, 1u);
        }
        g_alloc.free(g_alloc.ctx, draft, len + 1);
    }
    HU_ASSERT_EQ(audited, 1u);
}

static void run_never_calls_a_non_local_provider(void) {
    /* io.local.vtable NULL is what the glue passes when the agent's provider
     * is not a loopback model: no call is made anywhere. */
    hu_commitment_guard_io_t io = make_io(NULL, NULL);
    size_t len;
    char *draft = dup_draft("yeah saturday works", &len);
    char *before = draft;
    hu_commitment_guard_result_t r;
    HU_ASSERT_EQ(hu_commitment_guard_run(HU_GATE_LIVE, &io, "u free sat?", 11, &draft, &len, &r),
                 HU_OK);
    HU_ASSERT_FALSE(r.detector_ok);
    HU_ASSERT_EQ((int)r.decision, (int)HU_COMMIT_DETECT_FAILED);
    HU_ASSERT_TRUE(draft == before);
    g_alloc.free(g_alloc.ctx, draft, len + 1);
}

static void run_tags_detector_calls_with_commitment_check_purpose(void) {
    fake_llm_t f = {.replies = {k_plan_sat7, k_rewrite, k_none}, .n = 3};
    fake_cal_t c = {.state = HU_CAL_BUSY};
    hu_commitment_guard_io_t io = make_io(&f, &c);
    size_t len;
    char *draft = dup_draft("yeah saturday at 7 works", &len);
    hu_commitment_guard_result_t r;
    HU_ASSERT_EQ(hu_llm_purpose_current(), HU_LLM_PURPOSE_UNTAGGED);
    HU_ASSERT_EQ(hu_commitment_guard_run(HU_GATE_LIVE, &io, "u free sat?", 11, &draft, &len, &r),
                 HU_OK);
    HU_ASSERT_STR_EQ(f.purpose_seen[0], "commitment_check");
    HU_ASSERT_STR_EQ(f.purpose_seen[1], "commitment_check");
    HU_ASSERT_EQ(hu_llm_purpose_current(), HU_LLM_PURPOSE_UNTAGGED); /* restored */
    g_alloc.free(g_alloc.ctx, draft, len + 1);
}

/* A proactive caller's PROACTIVE tag (batch priority) survives the guard's
 * local calls: the guard labels only an untagged thread (#612 review). */
static void run_keeps_a_proactive_callers_purpose(void) {
    fake_llm_t f = {.replies = {k_plan_sat7, k_rewrite, k_none}, .n = 3};
    fake_cal_t c = {.state = HU_CAL_BUSY};
    hu_commitment_guard_io_t io = make_io(&f, &c);
    size_t len;
    char *draft = dup_draft("yeah saturday at 7 works", &len);
    hu_commitment_guard_result_t r;
    hu_llm_purpose_t outer = hu_llm_purpose_set(HU_LLM_PURPOSE_PROACTIVE);
    HU_ASSERT_EQ(hu_commitment_guard_run(HU_GATE_LIVE, &io, "u free sat?", 11, &draft, &len, &r),
                 HU_OK);
    HU_ASSERT_STR_EQ(f.purpose_seen[0], "proactive");
    HU_ASSERT_STR_EQ(f.purpose_seen[1], "proactive");
    HU_ASSERT_EQ(hu_llm_purpose_current(), HU_LLM_PURPOSE_PROACTIVE); /* still the caller's */
    (void)hu_llm_purpose_set(outer);
    g_alloc.free(g_alloc.ctx, draft, len + 1);
}

/* ── compatible provider: the purpose reaches the wire ────────────────── */

static void compatible_chat_sends_purpose_header_and_no_priority(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_provider_t p;
    HU_ASSERT_EQ(hu_compatible_create(&a, NULL, 0, "http://127.0.0.1:8741/v1", 24, &p), HU_OK);
    hu_llm_purpose_t prev = hu_llm_purpose_set(HU_LLM_PURPOSE_COMMITMENT_CHECK);
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(p.vtable->chat_with_system(p.ctx, &a, "s", 1, "m", 1, "x", 1, 0.0, &out, &out_len),
                 HU_OK);
    (void)hu_llm_purpose_set(prev);
    /* Foreground: the purpose is named, no X-HU-Priority (unmarked is live). */
    HU_ASSERT_STR_EQ(hu_compatible_test_last_headers(), "X-HU-Purpose: commitment_check\r\n");
    HU_ASSERT_FALSE(hu_llm_purpose_is_background(HU_LLM_PURPOSE_COMMITMENT_CHECK));
    if (out)
        a.free(a.ctx, out, out_len + 1);
    p.vtable->deinit(p.ctx, &a);
}

/* ── daemon glue ──────────────────────────────────────────────────────── */

static void glue_off_by_default_leaves_reply_untouched(void) {
    const char *prev = getenv("HU_COMMITMENT_GUARD");
    char *saved = prev ? strdup(prev) : NULL;
    unsetenv("HU_COMMITMENT_GUARD");
    HU_ASSERT_EQ((int)hu_commitment_guard_mode(), (int)HU_GATE_OFF);

    fake_llm_t f = {.replies = {k_plan_sat7, k_rewrite, k_none}, .n = 3};
    fake_cal_t c = {.state = HU_CAL_BUSY};
    hu_provider_t tp = {.ctx = &f, .vtable = &fake_vtable};
    hu_commitment_guard_set_test_provider(&tp);
    hu_commitment_guard_set_test_calendar(fake_calendar, &c);
    g_alloc = hu_system_allocator();
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &g_alloc;
    size_t len;
    char *draft = dup_draft("yeah saturday at 7 works", &len);
    char *before = draft;
    HU_ASSERT_FALSE(
        hu_daemon_commitment_guard_apply(&agent, "+15550100", 9, "u free sat?", 11, &draft, &len));
    HU_ASSERT_TRUE(draft == before);
    HU_ASSERT_STR_EQ(draft, "yeah saturday at 7 works");
    HU_ASSERT_EQ(f.calls, 0u);

    setenv("HU_COMMITMENT_GUARD", "live", 1);
    HU_ASSERT_TRUE(
        hu_daemon_commitment_guard_apply(&agent, "+15550100", 9, "u free sat?", 11, &draft, &len));
    HU_ASSERT_STR_EQ(draft, k_rewrite);
    g_alloc.free(g_alloc.ctx, draft, len + 1);

    hu_commitment_guard_set_test_provider(NULL);
    hu_commitment_guard_set_test_calendar(NULL, NULL);
    if (saved) {
        setenv("HU_COMMITMENT_GUARD", saved, 1);
        free(saved);
    } else {
        unsetenv("HU_COMMITMENT_GUARD");
    }
}

static void glue_live_without_a_local_provider_changes_nothing(void) {
    setenv("HU_COMMITMENT_GUARD", "live", 1);
    hu_commitment_guard_set_test_provider(NULL);
    g_alloc = hu_system_allocator();
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &g_alloc;
    size_t len;
    char *draft = dup_draft("yeah saturday at 7 works", &len);
    char *before = draft;
    HU_ASSERT_FALSE(
        hu_daemon_commitment_guard_apply(&agent, "+15550100", 9, "u free sat?", 11, &draft, &len));
    HU_ASSERT_TRUE(draft == before);
    g_alloc.free(g_alloc.ctx, draft, len + 1);
    unsetenv("HU_COMMITMENT_GUARD");
}

/* Same failure through the real reply-path glue. */
static void glue_live_notify_failure_holds_reply_and_counts(void) {
    setenv("HU_COMMITMENT_GUARD", "live", 1);
    fake_llm_t f = {.replies = {k_money, NULL}, .n = 2};
    hu_provider_t tp = {.ctx = &f, .vtable = &fake_vtable};
    hu_commitment_guard_set_test_provider(&tp);
    hu_commitment_guard_set_test_notifier(failing_notify);
    g_notify_calls = 0;
    g_notify_fail_first = 100;
    hu_commitment_guard_test_reset();
    g_alloc = hu_system_allocator();
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &g_alloc;
    size_t len;
    char *draft = dup_draft("sure ill venmo you", &len);
    HU_ASSERT_TRUE(
        hu_daemon_commitment_guard_apply(&agent, "+15550100", 9, "lend me 50?", 11, &draft, &len));
    HU_ASSERT_NULL(draft);
    HU_ASSERT_EQ(len, 0u);
    HU_ASSERT_EQ(g_notify_calls, 2u);
    HU_ASSERT_EQ(hu_commitment_guard_test_notify_failed_count(), 1u);
    hu_commitment_guard_set_test_notifier(NULL);
    hu_commitment_guard_set_test_provider(NULL);
    unsetenv("HU_COMMITMENT_GUARD");
}

void run_daemon_commitment_guard_tests(void) {
    HU_TEST_SUITE("daemon_commitment_guard");
    HU_RUN_TEST(prefilter_fires_on_plans_times_money_and_promises);
    HU_RUN_TEST(prefilter_fires_on_a_bare_yes_to_a_request);
    HU_RUN_TEST(prefilter_skips_chat_with_no_commitment_cue);
    HU_RUN_TEST(money_floor_matches_offers_to_pay_only);
    HU_RUN_TEST(parse_plan_with_time_resolves_a_two_hour_local_window);
    HU_RUN_TEST(parse_date_only_checks_the_waking_day);
    HU_RUN_TEST(parse_accepts_fenced_json_and_rejects_unknown_kinds);
    HU_RUN_TEST(decide_table_covers_every_branch);
    HU_RUN_TEST(calendar_parse_overlap_busy_gap_free_and_no_access_unknown);
    HU_RUN_TEST(calendar_query_never_spawns_in_tests);
    HU_RUN_TEST(run_off_is_byte_identical_and_calls_nothing);
    HU_RUN_TEST(run_shadow_computes_conflict_but_sends_the_draft_unchanged);
    HU_RUN_TEST(run_live_conflict_rewrites_and_notifies_owner);
    HU_RUN_TEST(run_live_money_holds_without_a_calendar_check);
    HU_RUN_TEST(run_live_free_low_stakes_plan_is_allowed);
    HU_RUN_TEST(run_live_unknown_calendar_holds_a_plan);
    HU_RUN_TEST(run_live_rewrite_that_still_commits_is_suppressed);
    HU_RUN_TEST(run_live_failed_rewrite_is_suppressed);
    HU_RUN_TEST(run_live_detector_down_falls_back_to_money_floor_only);
    HU_RUN_TEST(run_live_prefilter_miss_never_calls_the_model);
    HU_RUN_TEST(run_shadow_audits_every_nth_prefilter_miss);
    HU_RUN_TEST(run_never_calls_a_non_local_provider);
    HU_RUN_TEST(run_tags_detector_calls_with_commitment_check_purpose);
    HU_RUN_TEST(run_keeps_a_proactive_callers_purpose);
    HU_RUN_TEST(compatible_chat_sends_purpose_header_and_no_priority);
    HU_RUN_TEST(glue_off_by_default_leaves_reply_untouched);
    HU_RUN_TEST(glue_live_without_a_local_provider_changes_nothing);
    HU_RUN_TEST(run_live_notify_failure_keeps_reply_held_and_is_counted);
    HU_RUN_TEST(run_live_notify_retry_success_is_not_counted);
    HU_RUN_TEST(glue_live_notify_failure_holds_reply_and_counts);
}
