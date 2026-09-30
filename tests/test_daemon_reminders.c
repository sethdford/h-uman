/* Owner reminders (src/daemon/daemon_reminders.c): what the parser accepts,
 * which instant a phrase lands on, how that instant is named back, and the
 * daemon path from "remind me …" to a delivered text.
 *
 * Times are local, so every test pins TZ to a POSIX rule for US Eastern
 * ("EST5EDT,M3.2.0,M11.1.0") — no zoneinfo files needed, and DST ends on
 * Sun 2026-11-01 on every host. 2026-10-01 is a Thursday. */
#include "human/agent.h"
#include "human/channel.h"
#include "human/daemon.h"
#include "human/daemon/reminders.h"
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

/* Parse `text` as an ADD at `now` and return the instant it resolves to. */
static int64_t due_of(const char *text, int64_t now, char *what, size_t what_cap) {
    hu_reminder_cmd_t cmd;
    if (!hu_reminder_parse(text, strlen(text), &cmd) || cmd.kind != HU_REMINDER_CMD_ADD)
        return -1;
    if (what)
        snprintf(what, what_cap, "%s", cmd.what);
    return hu_reminder_resolve(&cmd.when, now);
}

/* ── Parsing ─────────────────────────────────────────────────────────────── */

static void reminder_time_at_the_end_splits_from_the_task(void) {
    tz_begin();
    int64_t now = local_at(10, 1, 10, 0);
    char what[128];
    HU_ASSERT_EQ(due_of("remind me to call mom at 5pm", now, what, sizeof(what)),
                 local_at(10, 1, 17, 0));
    HU_ASSERT_STR_EQ(what, "call mom");
    /* A number inside the task stays in the task. */
    HU_ASSERT_EQ(due_of("remind me to take the 5 train at 6", now, what, sizeof(what)),
                 local_at(10, 1, 18, 0));
    HU_ASSERT_STR_EQ(what, "take the 5 train");
    HU_ASSERT_EQ(due_of("Remind me to email Sam in 20m please", now, what, sizeof(what)),
                 now + 1200);
    HU_ASSERT_STR_EQ(what, "email Sam");
    tz_end();
}

static void reminder_time_first_then_to(void) {
    tz_begin();
    int64_t now = local_at(10, 1, 10, 0);
    char what[128];
    /* "at 9" on another day means the morning. */
    HU_ASSERT_EQ(due_of("remind me tomorrow at 9 to call the bank", now, what, sizeof(what)),
                 local_at(10, 2, 9, 0));
    HU_ASSERT_STR_EQ(what, "call the bank");
    HU_ASSERT_EQ(
        due_of("hey can you remind me in half an hour to stretch", now, what, sizeof(what)),
        now + 1800);
    HU_ASSERT_STR_EQ(what, "stretch");
    tz_end();
}

static void reminder_time_words_inside_the_task_are_not_a_time(void) {
    hu_reminder_cmd_t cmd;
    const char *t = "remind me to water the plants in the sun";
    HU_ASSERT_TRUE(hu_reminder_parse(t, strlen(t), &cmd));
    HU_ASSERT_EQ(cmd.kind, HU_REMINDER_CMD_ADD_NO_TIME);
    HU_ASSERT_STR_EQ(cmd.what, "water the plants in the sun");
    t = "remind me to pay rent at the bank";
    HU_ASSERT_TRUE(hu_reminder_parse(t, strlen(t), &cmd));
    HU_ASSERT_EQ(cmd.kind, HU_REMINDER_CMD_ADD_NO_TIME);
    HU_ASSERT_STR_EQ(cmd.what, "pay rent at the bank");
}

static void reminder_days_and_parts_of_day(void) {
    tz_begin();
    int64_t now = local_at(10, 1, 10, 0); /* Thursday */
    HU_ASSERT_EQ(due_of("remind me to pay rent on friday", now, NULL, 0), local_at(10, 2, 9, 0));
    HU_ASSERT_EQ(due_of("remind me to pay rent on fri at 5", now, NULL, 0),
                 local_at(10, 2, 17, 0)); /* 1–6 on another day: afternoon */
    HU_ASSERT_EQ(due_of("remind me to email Sam tonight", now, NULL, 0), local_at(10, 1, 20, 0));
    HU_ASSERT_EQ(due_of("remind me to run tomorrow morning", now, NULL, 0), local_at(10, 2, 9, 0));
    HU_ASSERT_EQ(due_of("remind me to call tomorrow evening at 7", now, NULL, 0),
                 local_at(10, 2, 19, 0));
    /* Today's weekday already past at that time rolls a week. */
    HU_ASSERT_EQ(due_of("remind me to file on thursday at 9am", now, NULL, 0),
                 local_at(10, 8, 9, 0));
    tz_end();
}

static void reminder_bare_hour_picks_the_next_one(void) {
    tz_begin();
    int64_t morning = local_at(10, 1, 10, 0);
    int64_t night = local_at(10, 1, 21, 0);
    HU_ASSERT_EQ(due_of("remind me to call mom at 7", morning, NULL, 0), local_at(10, 1, 19, 0));
    HU_ASSERT_EQ(due_of("remind me to call mom at 7", night, NULL, 0), local_at(10, 2, 7, 0));
    HU_ASSERT_EQ(due_of("remind me to call mom at 11", morning, NULL, 0), local_at(10, 1, 11, 0));
    HU_ASSERT_EQ(due_of("remind me to eat at noon", night, NULL, 0), local_at(10, 2, 12, 0));
    HU_ASSERT_EQ(due_of("remind me to leave at 17:30", morning, NULL, 0), local_at(10, 1, 17, 30));
    /* A time that has passed today with no day named means tomorrow. */
    HU_ASSERT_EQ(due_of("remind me to leave at 8am", morning, NULL, 0), local_at(10, 2, 8, 0));
    tz_end();
}

static void reminder_tomorrow_across_the_dst_change_keeps_the_clock_time(void) {
    tz_begin();
    int64_t now = local_at(10, 31, 10, 0); /* Sat, EDT */
    int64_t due = due_of("remind me to set the clocks tomorrow at 9am", now, NULL, 0);
    time_t td = (time_t)due;
    struct tm d;
    HU_ASSERT_NOT_NULL(localtime_r(&td, &d));
    HU_ASSERT_EQ(d.tm_mday, 1);
    HU_ASSERT_EQ(d.tm_hour, 9);
    HU_ASSERT_EQ(d.tm_min, 0);
    HU_ASSERT_EQ(due - now, (int64_t)24 * 3600); /* 23h of clock + the hour DST gives back */
    tz_end();
}

static void reminder_commands_other_than_add(void) {
    hu_reminder_cmd_t cmd;
    const char *t = "What are my reminders?";
    HU_ASSERT_TRUE(hu_reminder_parse(t, strlen(t), &cmd));
    HU_ASSERT_EQ(cmd.kind, HU_REMINDER_CMD_LIST);
    t = "done";
    HU_ASSERT_TRUE(hu_reminder_parse(t, strlen(t), &cmd));
    HU_ASSERT_EQ(cmd.kind, HU_REMINDER_CMD_DONE);
    t = "snooze";
    HU_ASSERT_TRUE(hu_reminder_parse(t, strlen(t), &cmd));
    HU_ASSERT_EQ(cmd.kind, HU_REMINDER_CMD_SNOOZE);
    HU_ASSERT_EQ(cmd.snooze_s, 900);
    t = "snooze 10";
    HU_ASSERT_TRUE(hu_reminder_parse(t, strlen(t), &cmd));
    HU_ASSERT_EQ(cmd.snooze_s, 600);
    t = "snooze for an hour";
    HU_ASSERT_TRUE(hu_reminder_parse(t, strlen(t), &cmd));
    HU_ASSERT_EQ(cmd.snooze_s, 3600);
    t = "remind me again in 20 min";
    HU_ASSERT_TRUE(hu_reminder_parse(t, strlen(t), &cmd));
    HU_ASSERT_EQ(cmd.kind, HU_REMINDER_CMD_SNOOZE);
    HU_ASSERT_EQ(cmd.when.relative_s, 1200);
    t = "remind me later";
    HU_ASSERT_TRUE(hu_reminder_parse(t, strlen(t), &cmd));
    HU_ASSERT_EQ(cmd.kind, HU_REMINDER_CMD_SNOOZE);
    HU_ASSERT_EQ(cmd.snooze_s, 3600);
    t = "tomorrow at 9";
    HU_ASSERT_TRUE(hu_reminder_parse(t, strlen(t), &cmd));
    HU_ASSERT_EQ(cmd.kind, HU_REMINDER_CMD_WHEN);
    HU_ASSERT_EQ(cmd.when.day_offset, 1);
}

static void reminder_ordinary_chat_is_not_a_command(void) {
    static const char *const chat[] = {
        "done with the report yet?",
        "snooze purple",
        "hey how are you",
        "i'll be there at 5",
        "remind me",
        "the sun is out",
        "",
    };
    for (size_t i = 0; i < sizeof(chat) / sizeof(chat[0]); i++) {
        hu_reminder_cmd_t cmd;
        HU_ASSERT_FALSE(hu_reminder_parse(chat[i], strlen(chat[i]), &cmd));
        HU_ASSERT_EQ(cmd.kind, HU_REMINDER_CMD_NONE);
    }
}

static void reminder_due_is_named_the_way_a_person_would(void) {
    tz_begin();
    int64_t now = local_at(10, 1, 10, 0);
    char buf[48];
    HU_ASSERT_TRUE(hu_reminder_format_due(now + 20 * 60, now, buf, sizeof(buf)) > 0);
    HU_ASSERT_STR_EQ(buf, "in 20 min");
    hu_reminder_format_due(local_at(10, 1, 17, 0), now, buf, sizeof(buf));
    HU_ASSERT_STR_EQ(buf, "today at 5pm");
    hu_reminder_format_due(local_at(10, 2, 9, 30), now, buf, sizeof(buf));
    HU_ASSERT_STR_EQ(buf, "tomorrow at 9:30am");
    hu_reminder_format_due(local_at(10, 5, 9, 0), now, buf, sizeof(buf));
    HU_ASSERT_STR_EQ(buf, "Monday at 9am");
    hu_reminder_format_due(local_at(10, 20, 12, 0), now, buf, sizeof(buf));
    HU_ASSERT_STR_EQ(buf, "Oct 20 at 12pm");
    HU_ASSERT_EQ(hu_reminder_format_due(now, now, buf, 4), 0); /* would not fit */
    HU_ASSERT_STR_EQ(buf, "");
    tz_end();
}

/* ── Daemon side ─────────────────────────────────────────────────────────── */

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

#define OWNER  "+15550000009"
#define SISTER "+15550000001"

typedef struct rec_channel {
    int sends;
    bool fail;
    char target[64];
    char text[320];
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
    char saved_gate[32];
    bool had_gate;
} fixture_t;

static void fixture_begin(fixture_t *f, const char *gate) {
    memset(f, 0, sizeof(*f));
    tz_begin();
    const char *g = getenv("HU_REMINDERS");
    f->had_gate = g != NULL;
    snprintf(f->saved_gate, sizeof(f->saved_gate), "%s", g ? g : "");
    if (gate)
        setenv("HU_REMINDERS", gate, 1);
    else
        unsetenv("HU_REMINDERS");
    f->contacts[0].contact_id = SISTER;
    f->contacts[0].name = "Mindy";
    f->contacts[0].relationship = "sister";
    f->contacts[1].contact_id = OWNER;
    f->contacts[1].name = "Seth";
    f->contacts[1].relationship = "test"; /* the owner's own number */
    f->persona.contacts = f->contacts;
    f->persona.contacts_count = 2;
    f->alloc = hu_system_allocator(); /* the memory keeps this pointer */
    f->mem = hu_sqlite_memory_create(&f->alloc, ":memory:");
    f->agent.persona = &f->persona;
    f->agent.memory = &f->mem;
    f->channel.ctx = &f->rec;
    f->channel.vtable = &k_rec_vtable;
    f->svc.channel = &f->channel;
    f->svc.channel_ctx = &f->rec;
}

static void fixture_end(fixture_t *f) {
    f->mem.vtable->deinit(f->mem.ctx);
    if (f->had_gate)
        setenv("HU_REMINDERS", f->saved_gate, 1);
    else
        unsetenv("HU_REMINDERS");
    tz_end();
}

static bool say(fixture_t *f, const char *from, const char *text, int64_t now, char *reply,
                size_t cap) {
    return hu_reminders_handle_owner_message(&f->agent, from, strlen(from), "imessage", text,
                                             strlen(text), now, reply, cap);
}

static int64_t rows_with_status(fixture_t *f, const char *status) {
    sqlite3 *db = hu_sqlite_memory_get_db(&f->mem);
    sqlite3_stmt *st = NULL;
    int64_t n = -1;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM sqlite_master WHERE name='reminders';", -1,
                           &st, NULL) != SQLITE_OK)
        return -1;
    bool exists = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 1;
    sqlite3_finalize(st);
    if (!exists)
        return 0;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM reminders WHERE status=?1;", -1, &st, NULL) !=
        SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, status, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW)
        n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n;
}

static void reminders_do_nothing_unless_the_gate_is_live(void) {
    const char *gates[] = {NULL, "off", "shadow", "bogus"};
    for (size_t i = 0; i < 4; i++) {
        fixture_t f;
        fixture_begin(&f, gates[i]);
        int64_t now = local_at(10, 1, 10, 0);
        char reply[256] = "untouched";
        HU_ASSERT_FALSE(say(&f, OWNER, "remind me to call mom at 5pm", now, reply, sizeof(reply)));
        HU_ASSERT_EQ(rows_with_status(&f, "pending"), 0);
        hu_reminders_tick(&f.agent, &f.svc, 1, now + 86400);
        HU_ASSERT_EQ(f.rec.sends, 0);
        fixture_end(&f);
    }
}

static void reminders_file_and_ack_only_for_the_owner(void) {
    fixture_t f;
    fixture_begin(&f, "live");
    int64_t now = local_at(10, 2, 10, 0);
    char reply[256];
    HU_ASSERT_FALSE(say(&f, SISTER, "remind me to call mom at 5pm", now, reply, sizeof(reply)));
    HU_ASSERT_EQ(rows_with_status(&f, "pending"), 0);

    HU_ASSERT_TRUE(say(&f, OWNER, "remind me to call mom at 5pm", now, reply, sizeof(reply)));
    HU_ASSERT_STR_EQ(reply, "ok, today at 5pm: call mom");
    HU_ASSERT_EQ(rows_with_status(&f, "pending"), 1);
    /* Ordinary owner chat still goes to the model. */
    HU_ASSERT_FALSE(say(&f, OWNER, "how's your day going", now, reply, sizeof(reply)));
    fixture_end(&f);
}

static void reminders_ask_when_then_take_the_next_message_as_the_time(void) {
    fixture_t f;
    fixture_begin(&f, "live");
    int64_t now = local_at(10, 3, 10, 0);
    char reply[256];
    HU_ASSERT_TRUE(say(&f, OWNER, "remind me to renew my passport", now, reply, sizeof(reply)));
    HU_ASSERT_STR_EQ(reply, "sure, when?");
    HU_ASSERT_EQ(rows_with_status(&f, "pending"), 0);
    HU_ASSERT_TRUE(say(&f, OWNER, "tomorrow", now + 30, reply, sizeof(reply)));
    HU_ASSERT_STR_EQ(reply, "ok, tomorrow at 9am: renew my passport");
    HU_ASSERT_EQ(rows_with_status(&f, "pending"), 1);
    /* Answered: the next bare time is just chat again. */
    HU_ASSERT_FALSE(say(&f, OWNER, "tomorrow", now + 60, reply, sizeof(reply)));
    HU_ASSERT_EQ(rows_with_status(&f, "pending"), 1);
    fixture_end(&f);
}

static void reminders_deliver_once_to_the_owner_then_done_crosses_off(void) {
    fixture_t f;
    fixture_begin(&f, "live");
    int64_t now = local_at(10, 4, 10, 0);
    char reply[256];
    HU_ASSERT_TRUE(say(&f, OWNER, "remind me in 5 min to stretch", now, reply, sizeof(reply)));
    hu_reminders_tick(&f.agent, &f.svc, 1, now + 60);
    HU_ASSERT_EQ(f.rec.sends, 0); /* not due yet */
    hu_reminders_tick(&f.agent, &f.svc, 1, now + 301);
    HU_ASSERT_EQ(f.rec.sends, 1);
    HU_ASSERT_STR_EQ(f.rec.target, OWNER);
    HU_ASSERT_STR_EQ(f.rec.text, "reminder: stretch");
    hu_reminders_tick(&f.agent, &f.svc, 1, now + 400);
    HU_ASSERT_EQ(f.rec.sends, 1); /* never twice */

    HU_ASSERT_TRUE(say(&f, OWNER, "done", now + 420, reply, sizeof(reply)));
    HU_ASSERT_STR_EQ(reply, "nice, crossed off");
    HU_ASSERT_EQ(rows_with_status(&f, "done"), 1);
    /* Nothing delivered recently any more: "done" is chat. */
    HU_ASSERT_FALSE(say(&f, OWNER, "done", now + 430, reply, sizeof(reply)));
    fixture_end(&f);
}

static void reminders_retry_a_failed_send_and_snooze_rearms(void) {
    fixture_t f;
    fixture_begin(&f, "live");
    int64_t now = local_at(10, 5, 10, 0);
    char reply[256];
    HU_ASSERT_TRUE(say(&f, OWNER, "remind me in 10m to call the vet", now, reply, sizeof(reply)));
    f.rec.fail = true;
    hu_reminders_tick(&f.agent, &f.svc, 1, now + 600);
    HU_ASSERT_EQ(f.rec.sends, 0);
    HU_ASSERT_EQ(rows_with_status(&f, "pending"), 1); /* not marked sent */
    f.rec.fail = false;
    hu_reminders_tick(&f.agent, &f.svc, 1, now + 660);
    HU_ASSERT_EQ(f.rec.sends, 1);

    HU_ASSERT_TRUE(say(&f, OWNER, "snooze 30m", now + 700, reply, sizeof(reply)));
    HU_ASSERT_STR_EQ(reply, "ok, again in 30 min");
    hu_reminders_tick(&f.agent, &f.svc, 1, now + 700 + 1780);
    HU_ASSERT_EQ(f.rec.sends, 1);
    hu_reminders_tick(&f.agent, &f.svc, 1, now + 700 + 1800);
    HU_ASSERT_EQ(f.rec.sends, 2);
    fixture_end(&f);
}

static void reminders_list_and_missed(void) {
    fixture_t f;
    fixture_begin(&f, "live");
    int64_t now = local_at(10, 6, 10, 0);
    char reply[256];
    HU_ASSERT_TRUE(say(&f, OWNER, "reminders", now, reply, sizeof(reply)));
    HU_ASSERT_STR_EQ(reply, "nothing on your list");
    HU_ASSERT_TRUE(say(&f, OWNER, "remind me to call mom at 5pm", now, reply, sizeof(reply)));
    HU_ASSERT_TRUE(say(&f, OWNER, "remind me to pay rent tomorrow", now, reply, sizeof(reply)));
    HU_ASSERT_TRUE(say(&f, OWNER, "what are my reminders?", now, reply, sizeof(reply)));
    HU_ASSERT_STR_EQ(reply, "2 coming up:\n- call mom, today at 5pm\n- pay rent, tomorrow at 9am");

    /* The daemon was down until well after 5pm: that one is not sent as if
     * on time. It is mentioned once, and a failed mention is retried. */
    f.rec.fail = true;
    hu_reminders_tick(&f.agent, &f.svc, 1, local_at(10, 6, 20, 0));
    HU_ASSERT_EQ(f.rec.sends, 0);
    HU_ASSERT_EQ(rows_with_status(&f, "missed"), 1);
    f.rec.fail = false;
    hu_reminders_tick(&f.agent, &f.svc, 1, local_at(10, 6, 20, 1));
    HU_ASSERT_EQ(f.rec.sends, 1);
    HU_ASSERT_STR_EQ(f.rec.text, "missed these while I was offline:\n- call mom (today at 5pm)");
    HU_ASSERT_EQ(rows_with_status(&f, "missed_told"), 1);
    hu_reminders_tick(&f.agent, &f.svc, 1, local_at(10, 6, 20, 2));
    HU_ASSERT_EQ(f.rec.sends, 1);                     /* told once */
    HU_ASSERT_EQ(rows_with_status(&f, "pending"), 1); /* rent is still ahead */
    fixture_end(&f);
}

static void reminders_never_go_to_anyone_but_the_owner(void) {
    fixture_t f;
    fixture_begin(&f, "live");
    int64_t now = local_at(10, 7, 10, 0);
    char reply[256];
    HU_ASSERT_TRUE(say(&f, OWNER, "remind me in 5 min to stretch", now, reply, sizeof(reply)));
    /* The owner changes before it is due: the row must not be sent to the old number. */
    f.contacts[1].relationship = "friend";
    hu_reminders_tick(&f.agent, &f.svc, 1, now + 400);
    HU_ASSERT_EQ(f.rec.sends, 0);
    HU_ASSERT_EQ(rows_with_status(&f, "done"), 1);
    fixture_end(&f);
}
#endif /* HU_ENABLE_SQLITE */

void run_daemon_reminders_tests(void) {
    HU_TEST_SUITE("daemon reminders");
    HU_RUN_TEST(reminder_time_at_the_end_splits_from_the_task);
    HU_RUN_TEST(reminder_time_first_then_to);
    HU_RUN_TEST(reminder_time_words_inside_the_task_are_not_a_time);
    HU_RUN_TEST(reminder_days_and_parts_of_day);
    HU_RUN_TEST(reminder_bare_hour_picks_the_next_one);
    HU_RUN_TEST(reminder_tomorrow_across_the_dst_change_keeps_the_clock_time);
    HU_RUN_TEST(reminder_commands_other_than_add);
    HU_RUN_TEST(reminder_ordinary_chat_is_not_a_command);
    HU_RUN_TEST(reminder_due_is_named_the_way_a_person_would);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(reminders_do_nothing_unless_the_gate_is_live);
    HU_RUN_TEST(reminders_file_and_ack_only_for_the_owner);
    HU_RUN_TEST(reminders_ask_when_then_take_the_next_message_as_the_time);
    HU_RUN_TEST(reminders_deliver_once_to_the_owner_then_done_crosses_off);
    HU_RUN_TEST(reminders_retry_a_failed_send_and_snooze_rearms);
    HU_RUN_TEST(reminders_list_and_missed);
    HU_RUN_TEST(reminders_never_go_to_anyone_but_the_owner);
#endif
}
