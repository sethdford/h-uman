/* Morning briefing (src/daemon/daemon_briefing.c): what one message says,
 * how the calendar helper's output is read, and when the daemon sends it.
 *
 * TZ is pinned to a POSIX US-Eastern rule so local clock times are the same
 * on every host. 2026-10-01 is a Thursday. */
#include "human/agent.h"
#include "human/channel.h"
#include "human/daemon.h"
#include "human/daemon/briefing.h"
#include "human/memory.h"
#include "human/persona.h"
#include "test_framework.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static char g_saved_tz[128];
static bool g_had_tz;

static void tz_begin(void) {
    const char *tz = getenv("TZ");
    g_had_tz = tz != NULL;
    snprintf(g_saved_tz, sizeof(g_saved_tz), "%s", tz ? tz : "");
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
    tzset();
}

static void tz_end(void) {
    if (g_had_tz)
        setenv("TZ", g_saved_tz, 1);
    else
        unsetenv("TZ");
    tzset();
}

static int64_t local_at(int mon, int day, int hour, int min) {
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = 2026 - 1900;
    t.tm_mon = mon - 1;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min = min;
    t.tm_isdst = -1;
    return (int64_t)mktime(&t);
}

/* ── Composing ───────────────────────────────────────────────────────────── */

static void briefing_says_everything_in_one_message_with_sources(void) {
    tz_begin();
    int64_t now = local_at(10, 1, 8, 0);
    hu_briefing_inputs_t in;
    memset(&in, 0, sizeof(in));
    snprintf(in.events[0].name, sizeof(in.events[0].name), "standup");
    in.events[0].hour = 10;
    snprintf(in.events[1].name, sizeof(in.events[1].name), "dentist");
    in.events[1].hour = 14, in.events[1].minute = 30;
    in.events_n = 2;
    in.events_more = 1;
    snprintf(in.reminders[0].what, sizeof(in.reminders[0].what), "call mom");
    in.reminders[0].due_at = local_at(10, 1, 17, 0);
    in.reminders_n = 1;
    hu_briefing_commitment_t *c = &in.commitments[0];
    snprintf(c->what, sizeof(c->what), "i'll send the deck");
    snprintf(c->who_name, sizeof(c->who_name), "Dana");
    c->mine = true, c->overdue = true;
    c = &in.commitments[1];
    snprintf(c->what, sizeof(c->what), "i'll call you back");
    snprintf(c->who_name, sizeof(c->who_name), "Mom");
    in.commitments_n = 2;
    snprintf(in.dates[0].label, sizeof(in.dates[0].label), "birthday (\"happy birthday min!\")");
    in.dates[0].days_away = 2;
    in.dates_n = 1;
    in.have_weather = true;
    in.temp_f = 72;
    snprintf(in.condition, sizeof(in.condition), "sunny");

    char out[1024];
    size_t n = hu_briefing_compose(&in, now, out, sizeof(out));
    HU_ASSERT_EQ(n, strlen(out));
    HU_ASSERT_STR_EQ(out, "morning. here's today:\n"
                          "- on the calendar: 10am standup, 2:30pm dentist (+1 more)\n"
                          "- reminders: call mom (5pm)\n"
                          "- you told Dana: \"i'll send the deck\" (overdue)\n"
                          "- Mom told you: \"i'll call you back\"\n"
                          "- Saturday: birthday (\"happy birthday min!\")\n"
                          "- 72°F, sunny");
    /* Too small to hold it all: nothing, never half a briefing. */
    HU_ASSERT_EQ(hu_briefing_compose(&in, now, out, 64), 0);
    HU_ASSERT_STR_EQ(out, "");
    tz_end();
}

static void briefing_with_only_weather_says_nothing(void) {
    hu_briefing_inputs_t in;
    memset(&in, 0, sizeof(in));
    in.have_weather = true;
    in.temp_f = 60;
    snprintf(in.condition, sizeof(in.condition), "cloudy");
    char out[256] = "x";
    HU_ASSERT_EQ(hu_briefing_compose(&in, 1790000000, out, sizeof(out)), 0);
    HU_ASSERT_STR_EQ(out, "");
    HU_ASSERT_EQ(hu_briefing_compose(NULL, 1790000000, out, sizeof(out)), 0);
}

static void briefing_reads_calendar_times_in_either_clock_and_sorts(void) {
    const char *json =
        "[{\"name\":\"dentist\",\"start\":\"Thursday, October 1, 2026 at 2:30:00 PM\"},"
        "{\"name\":\"gym\",\"start\":\"Thursday 1 October 2026 at 07:15:00\"},"
        "{\"name\":\"no time\"},"
        "{\"name\":\"standup\",\"start\":\"Thursday, October 1, 2026 at 10:00:00\xe2\x80\xaf"
        "AM\"},"
        "{\"name\":\"\"}]";
    hu_briefing_event_t ev[8];
    size_t n = 0, more = 9;
    HU_ASSERT_TRUE(hu_briefing_parse_calendar(json, strlen(json), ev, 8, &n, &more));
    HU_ASSERT_EQ(n, 4); /* the unnamed one is dropped */
    HU_ASSERT_EQ(more, 0);
    HU_ASSERT_STR_EQ(ev[0].name, "gym");
    HU_ASSERT_EQ(ev[0].hour, 7);
    HU_ASSERT_EQ(ev[0].minute, 15);
    HU_ASSERT_STR_EQ(ev[1].name, "standup");
    HU_ASSERT_EQ(ev[1].hour, 10);
    HU_ASSERT_STR_EQ(ev[2].name, "dentist");
    HU_ASSERT_EQ(ev[2].hour, 14);
    HU_ASSERT_EQ(ev[2].minute, 30);
    HU_ASSERT_STR_EQ(ev[3].name, "no time");
    HU_ASSERT_EQ(ev[3].hour, -1);

    /* More events than room: the rest are counted, not lost silently. */
    HU_ASSERT_TRUE(hu_briefing_parse_calendar(json, strlen(json), ev, 2, &n, &more));
    HU_ASSERT_EQ(n, 2);
    HU_ASSERT_EQ(more, 2);
}

static void briefing_prefers_numeric_times_and_lists_all_day_first(void) {
    /* Current helper output: numbers win over a start string in any locale. */
    const char *json =
        "[{\"name\":\"dentist\",\"start\":\"jeudi 1 octobre 2026 14 h 30\",\"h\":14,\"m\":30,"
        "\"allday\":false},"
        "{\"name\":\"Mom\\u2019s birthday\",\"start\":\"x\",\"h\":0,\"m\":0,\"allday\":true},"
        "{\"name\":\"say \\\"hi\\\"\",\"start\":\"x\",\"h\":9,\"m\":5,\"allday\":false}]";
    hu_briefing_event_t ev[4];
    size_t n = 0, more = 0;
    HU_ASSERT_TRUE(hu_briefing_parse_calendar(json, strlen(json), ev, 4, &n, &more));
    HU_ASSERT_EQ(n, 3);
    HU_ASSERT_TRUE(ev[0].all_day);
    HU_ASSERT_EQ(ev[0].hour, -1);
    HU_ASSERT_STR_EQ(ev[1].name, "say \"hi\""); /* an escaped title survives */
    HU_ASSERT_EQ(ev[1].hour, 9);
    HU_ASSERT_EQ(ev[1].minute, 5);
    HU_ASSERT_STR_EQ(ev[2].name, "dentist");
    HU_ASSERT_EQ(ev[2].hour, 14);
    HU_ASSERT_EQ(ev[2].minute, 30);
}

static void briefing_refuses_calendar_output_it_cannot_parse(void) {
    /* A title with an unescaped quote breaks the helper's JSON. */
    const char *bad = "[{\"name\":\"say \"hi\"\",\"start\":\"x\"}]";
    hu_briefing_event_t ev[4];
    size_t n = 7, more = 7;
    HU_ASSERT_FALSE(hu_briefing_parse_calendar(bad, strlen(bad), ev, 4, &n, &more));
    HU_ASSERT_EQ(n, 0);
    HU_ASSERT_EQ(more, 0);
    HU_ASSERT_FALSE(hu_briefing_parse_calendar("{}", 2, ev, 4, &n, &more));
}

static void briefing_window_is_send_hour_until_noon(void) {
    HU_ASSERT_FALSE(hu_briefing_in_window(7, 8));
    HU_ASSERT_TRUE(hu_briefing_in_window(8, 8));
    HU_ASSERT_TRUE(hu_briefing_in_window(11, 8));
    HU_ASSERT_FALSE(hu_briefing_in_window(12, 8)); /* late is skipped, not sent */
    HU_ASSERT_FALSE(hu_briefing_in_window(4, 4));  /* out-of-range hours never send */
}

/* ── Daemon side ─────────────────────────────────────────────────────────── */

#ifdef HU_ENABLE_SQLITE
#include "human/memory/briefing_repo.h"
#include "human/memory/reminder_repo.h"
#include "human/memory/superhuman.h"
#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>

#define OWNER  "+15550000009"
#define SISTER "+15550000001"

typedef struct rec_channel {
    int sends;
    bool fail;
    char target[64];
    char text[2048];
} rec_channel_t;

static hu_error_t rec_send(void *ctx, const char *target, size_t target_len, const char *msg,
                           size_t msg_len, const char *const *media, size_t media_count) {
    (void)media, (void)media_count;
    rec_channel_t *r = ctx;
    if (r->fail)
        return HU_ERR_IO;
    r->sends++;
    snprintf(r->target, sizeof(r->target), "%.*s", (int)target_len, target);
    snprintf(r->text, sizeof(r->text), "%.*s", (int)msg_len, msg);
    return HU_OK;
}

static const char *rec_name(void *ctx) {
    (void)ctx;
    return "imessage";
}

static const hu_channel_vtable_t k_rec_vtable = {.send = rec_send, .name = rec_name};

typedef struct fixture {
    hu_contact_profile_t contacts[2];
    hu_persona_t persona;
    hu_allocator_t alloc;
    hu_memory_t mem;
    hu_agent_t agent;
    rec_channel_t rec;
    hu_channel_t channel;
    hu_service_channel_t svc;
    char state_dir[128];
} fixture_t;

static void fixture_begin(fixture_t *f, const char *gate) {
    memset(f, 0, sizeof(*f));
    tz_begin();
    setenv("HU_BRIEFING", gate, 1);
    unsetenv("HU_BRIEFING_HOUR");
    snprintf(f->state_dir, sizeof(f->state_dir), "/tmp/hu_briefing_test_XXXXXX");
    HU_ASSERT_NOT_NULL(mkdtemp(f->state_dir));
    setenv("HU_STATE_DIR", f->state_dir, 1);
    f->contacts[0].contact_id = SISTER;
    f->contacts[0].name = "Mindy Ford";
    f->contacts[0].relationship = "sister";
    f->contacts[1].contact_id = OWNER;
    f->contacts[1].name = "Seth";
    f->contacts[1].relationship = "test";
    f->persona.contacts = f->contacts;
    f->persona.contacts_count = 2;
    f->alloc = hu_system_allocator();
    f->mem = hu_sqlite_memory_create(&f->alloc, ":memory:");
    f->agent.persona = &f->persona;
    f->agent.memory = &f->mem;
    f->channel.ctx = &f->rec;
    f->channel.vtable = &k_rec_vtable;
    f->svc.channel = &f->channel;
    f->svc.channel_ctx = &f->rec;
}

static void fixture_end(fixture_t *f) {
    rmdir(f->state_dir); /* empty unless a shadow test left a file, which it removes */
    f->mem.vtable->deinit(f->mem.ctx);
    unsetenv("HU_BRIEFING");
    unsetenv("HU_BRIEFING_HOUR");
    unsetenv("HU_STATE_DIR");
    tz_end();
}

static sqlite3 *db_of(fixture_t *f) {
    return hu_sqlite_memory_get_db(&f->mem);
}

static void briefing_live_sends_one_message_a_day_to_the_owner(void) {
    fixture_t f;
    fixture_begin(&f, "live");
    int64_t now = local_at(10, 10, 8, 5);
    int64_t id = 0;
    HU_ASSERT_EQ(hu_reminder_repo_add(db_of(&f), OWNER, "imessage", "call mom",
                                      local_at(10, 10, 17, 0), now - 3600, "t", &id),
                 HU_OK);
    static const char said[] = "i'll send the deck";
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&f.mem, &f.alloc, SISTER, strlen(SISTER), said,
                                                strlen(said), "me", 2, local_at(10, 9, 12, 0)),
                 HU_OK);

    hu_briefing_tick(&f.agent, &f.svc, 1, now);
    HU_ASSERT_EQ(f.rec.sends, 1);
    HU_ASSERT_STR_EQ(f.rec.target, OWNER);
    HU_ASSERT_STR_EQ(f.rec.text, "morning. here's today:\n"
                                 "- reminders: call mom (5pm)\n"
                                 "- you told Mindy: \"i'll send the deck\" (overdue)");
    hu_briefing_tick(&f.agent, &f.svc, 1, now + 120);
    hu_briefing_tick(&f.agent, &f.svc, 1, now + 3600);
    HU_ASSERT_EQ(f.rec.sends, 1); /* once a day */
    bool claimed = true;
    HU_ASSERT_EQ(hu_briefing_repo_claim(db_of(&f), "2026-10-10", now, "live", &claimed), HU_OK);
    HU_ASSERT_FALSE(claimed); /* durably: a restart cannot send it again */
    fixture_end(&f);
}

static void briefing_waits_for_the_hour_and_skips_a_late_morning(void) {
    fixture_t f;
    fixture_begin(&f, "live");
    int64_t id = 0;
    HU_ASSERT_EQ(hu_reminder_repo_add(db_of(&f), OWNER, "imessage", "pay rent",
                                      local_at(10, 11, 18, 0), local_at(10, 11, 6, 0), "t", &id),
                 HU_OK);
    hu_briefing_tick(&f.agent, &f.svc, 1, local_at(10, 11, 7, 30));
    HU_ASSERT_EQ(f.rec.sends, 0); /* before 8 */
    hu_briefing_tick(&f.agent, &f.svc, 1, local_at(10, 11, 12, 30));
    HU_ASSERT_EQ(f.rec.sends, 0); /* after noon: the day is skipped */
    setenv("HU_BRIEFING_HOUR", "6", 1);
    int64_t id2 = 0;
    HU_ASSERT_EQ(hu_reminder_repo_add(db_of(&f), OWNER, "imessage", "run", local_at(10, 12, 18, 0),
                                      local_at(10, 12, 5, 0), "t", &id2),
                 HU_OK);
    hu_briefing_tick(&f.agent, &f.svc, 1, local_at(10, 12, 6, 1));
    HU_ASSERT_EQ(f.rec.sends, 1); /* HU_BRIEFING_HOUR moves it */
    fixture_end(&f);
}

static void briefing_empty_day_sends_nothing(void) {
    fixture_t f;
    fixture_begin(&f, "live");
    hu_briefing_tick(&f.agent, &f.svc, 1, local_at(10, 13, 9, 0));
    HU_ASSERT_EQ(f.rec.sends, 0);
    bool claimed = true;
    HU_ASSERT_EQ(hu_briefing_repo_claim(db_of(&f), "2026-10-13", 1, "live", &claimed), HU_OK);
    HU_ASSERT_FALSE(claimed); /* decided once for the day, not re-checked every minute */
    fixture_end(&f);
}

static void briefing_shadow_writes_the_file_and_sends_nothing(void) {
    fixture_t f;
    fixture_begin(&f, "shadow");
    int64_t id = 0;
    HU_ASSERT_EQ(hu_reminder_repo_add(db_of(&f), OWNER, "imessage", "call the vet",
                                      local_at(10, 14, 15, 30), local_at(10, 14, 6, 0), "t", &id),
                 HU_OK);
    hu_briefing_tick(&f.agent, &f.svc, 1, local_at(10, 14, 8, 0));
    HU_ASSERT_EQ(f.rec.sends, 0);
    char path[256], got[512] = "";
    snprintf(path, sizeof(path), "%s/briefings/2026-10-14.txt", f.state_dir);
    struct stat st;
    HU_ASSERT_EQ(stat(path, &st), 0);
    HU_ASSERT_EQ(st.st_mode & 0777, 0600); /* private messages and calendar titles */
    FILE *fp = fopen(path, "r");
    HU_ASSERT_NOT_NULL(fp);
    size_t n = fread(got, 1, sizeof(got) - 1, fp);
    fclose(fp);
    got[n] = '\0';
    HU_ASSERT_STR_EQ(got, "morning. here's today:\n- reminders: call the vet (3:30pm)\n");
    unlink(path);
    char dir[200];
    snprintf(dir, sizeof(dir), "%s/briefings", f.state_dir);
    rmdir(dir);
    fixture_end(&f);
}

static void briefing_failed_send_is_retried_and_off_does_nothing(void) {
    fixture_t f;
    fixture_begin(&f, "live");
    int64_t id = 0;
    HU_ASSERT_EQ(hu_reminder_repo_add(db_of(&f), OWNER, "imessage", "stretch",
                                      local_at(10, 15, 10, 0), local_at(10, 15, 6, 0), "t", &id),
                 HU_OK);
    f.rec.fail = true;
    hu_briefing_tick(&f.agent, &f.svc, 1, local_at(10, 15, 8, 0));
    HU_ASSERT_EQ(f.rec.sends, 0);
    f.rec.fail = false;
    hu_briefing_tick(&f.agent, &f.svc, 1, local_at(10, 15, 8, 2));
    HU_ASSERT_EQ(f.rec.sends, 1);

    setenv("HU_BRIEFING", "off", 1);
    int64_t id2 = 0;
    HU_ASSERT_EQ(hu_reminder_repo_add(db_of(&f), OWNER, "imessage", "x", local_at(10, 16, 10, 0),
                                      local_at(10, 16, 6, 0), "t", &id2),
                 HU_OK);
    hu_briefing_tick(&f.agent, &f.svc, 1, local_at(10, 16, 8, 0));
    HU_ASSERT_EQ(f.rec.sends, 1);
    fixture_end(&f);
}
#endif /* HU_ENABLE_SQLITE */

void run_daemon_briefing_tests(void) {
    HU_TEST_SUITE("daemon briefing");
    HU_RUN_TEST(briefing_says_everything_in_one_message_with_sources);
    HU_RUN_TEST(briefing_with_only_weather_says_nothing);
    HU_RUN_TEST(briefing_reads_calendar_times_in_either_clock_and_sorts);
    HU_RUN_TEST(briefing_prefers_numeric_times_and_lists_all_day_first);
    HU_RUN_TEST(briefing_refuses_calendar_output_it_cannot_parse);
    HU_RUN_TEST(briefing_window_is_send_hour_until_noon);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(briefing_live_sends_one_message_a_day_to_the_owner);
    HU_RUN_TEST(briefing_waits_for_the_hour_and_skips_a_late_morning);
    HU_RUN_TEST(briefing_empty_day_sends_nothing);
    HU_RUN_TEST(briefing_shadow_writes_the_file_and_sends_nothing);
    HU_RUN_TEST(briefing_failed_send_is_retried_and_off_does_nothing);
#endif
}
