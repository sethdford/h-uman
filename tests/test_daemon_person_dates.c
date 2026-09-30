/* Person dates (src/daemon/daemon_person_dates.c): what the owner can say,
 * how far away a date is, and how owner-given dates and Contacts birthdays
 * reach the morning briefing with a name on them.
 *
 * TZ is pinned to a POSIX US-Eastern rule. 2026-02-28 is a Saturday. */
#include "human/agent.h"
#include "human/channel.h"
#include "human/daemon.h"
#include "human/daemon/briefing.h"
#include "human/daemon/person_dates.h"
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

static int64_t local_at(int year, int mon, int day, int hour) {
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = year - 1900;
    t.tm_mon = mon - 1;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_isdst = -1;
    return (int64_t)mktime(&t);
}

static void expect_parse(const char *text, const char *who, const char *label, int m, int d) {
    hu_person_date_cmd_t c;
    HU_ASSERT_TRUE(hu_person_date_parse(text, strlen(text), &c));
    HU_ASSERT_STR_EQ(c.who, who);
    HU_ASSERT_STR_EQ(c.label, label);
    HU_ASSERT_EQ(c.month, m);
    HU_ASSERT_EQ(c.day, d);
}

static void person_date_parse_accepts_the_ways_people_say_it(void) {
    expect_parse("mom's birthday is march 3", "mom", "birthday", 3, 3);
    expect_parse("Remember Mindy\xe2\x80\x99s anniversary is 6/12.", "mindy", "anniversary", 6, 12);
    expect_parse("our anniversary is June 12th, 2015", "our", "anniversary", 6, 12);
    expect_parse("dad's bday: 3rd of march", "dad", "birthday", 3, 3);
    expect_parse("btw jane doe's birthday is 12 october", "jane doe", "birthday", 10, 12);
    expect_parse("leo's b-day is feb 29", "leo", "birthday", 2, 29);
    expect_parse("fyi my birthday falls on 07-04", "my", "birthday", 7, 4);
}

static void person_date_parse_refuses_what_is_not_a_date_statement(void) {
    static const char *const no[] = {
        "mom's birthday is soon",                   /* no date */
        "mom's birthday is feb 30",                 /* no such day */
        "the kids and mom's birthday is march 3",   /* a sentence, not a person */
        "mom's party is march 3",                   /* not a birthday or anniversary */
        "what's your birthday",                     /* a question */
        "mom's birthday is march 3 and dad's is 4", /* trailing words */
        "",
    };
    for (size_t i = 0; i < sizeof(no) / sizeof(no[0]); i++) {
        hu_person_date_cmd_t c;
        HU_ASSERT_FALSE(hu_person_date_parse(no[i], strlen(no[i]), &c));
    }
}

static void person_date_days_away_wraps_the_year_and_handles_feb_29(void) {
    tz_begin();
    int64_t now = local_at(2026, 10, 1, 9);
    HU_ASSERT_EQ(hu_person_date_days_away(10, 1, now), 0);
    HU_ASSERT_EQ(hu_person_date_days_away(10, 2, now), 1);
    HU_ASSERT_EQ(hu_person_date_days_away(9, 30, now), 364);
    HU_ASSERT_EQ(hu_person_date_days_away(2, 29, now), 150); /* Feb 28, 2027 */
    HU_ASSERT_EQ(hu_person_date_days_away(2, 30, now), -1);
    /* Across the Nov 1 DST change, still whole days. */
    HU_ASSERT_EQ(hu_person_date_days_away(11, 2, local_at(2026, 10, 31, 23)), 2);
    tz_end();
}

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>

#define OWNER  "+15550000009"
#define SISTER "+15550000001"
#define MOTHER "+15550000002"

typedef struct rec_channel {
    int sends;
    char text[2048];
} rec_channel_t;

static hu_error_t rec_send(void *ctx, const char *target, size_t target_len, const char *msg,
                           size_t msg_len, const char *const *media, size_t media_count) {
    (void)target, (void)target_len, (void)media, (void)media_count;
    rec_channel_t *r = ctx;
    r->sends++;
    snprintf(r->text, sizeof(r->text), "%.*s", (int)msg_len, msg);
    return HU_OK;
}

static const char *rec_name(void *ctx) {
    (void)ctx;
    return "imessage";
}

static const hu_channel_vtable_t k_rec_vtable = {.send = rec_send, .name = rec_name};

typedef struct fixture {
    hu_contact_profile_t contacts[3];
    hu_persona_t persona;
    hu_allocator_t alloc;
    hu_memory_t mem;
    hu_agent_t agent;
    rec_channel_t rec;
    hu_channel_t channel;
    hu_service_channel_t svc;
    char ab_dir[64];
} fixture_t;

static void make_addressbook(const char *path, const char *phone, long long birthday) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(path, &db), SQLITE_OK);
    char sql[512];
    snprintf(sql, sizeof(sql),
             "CREATE TABLE ZABCDRECORD (Z_PK INTEGER PRIMARY KEY, ZBIRTHDAY REAL);"
             "CREATE TABLE ZABCDPHONENUMBER (ZOWNER INTEGER, ZFULLNUMBER TEXT);"
             "INSERT INTO ZABCDRECORD VALUES (1, %lld);"
             "INSERT INTO ZABCDPHONENUMBER VALUES (1, '%s');",
             birthday, phone);
    HU_ASSERT_EQ(sqlite3_exec(db, sql, NULL, NULL, NULL), SQLITE_OK);
    sqlite3_close(db);
}

static void fixture_begin(fixture_t *f, const char *gate) {
    memset(f, 0, sizeof(*f));
    tz_begin();
    setenv("HU_DATES", gate, 1);
    f->contacts[0].contact_id = SISTER;
    f->contacts[0].name = "Mindy Ford";
    f->contacts[0].relationship = "sister";
    f->contacts[1].contact_id = MOTHER;
    f->contacts[1].name = "Betty Ford";
    f->contacts[1].relationship = "mother";
    f->contacts[2].contact_id = OWNER;
    f->contacts[2].name = "Seth";
    f->contacts[2].relationship = "test";
    f->persona.contacts = f->contacts;
    f->persona.contacts_count = 3;
    f->alloc = hu_system_allocator();
    f->mem = hu_sqlite_memory_create(&f->alloc, ":memory:");
    f->agent.persona = &f->persona;
    f->agent.memory = &f->mem;
    f->channel.ctx = &f->rec;
    f->channel.vtable = &k_rec_vtable;
    f->svc.channel = &f->channel;
    f->svc.channel_ctx = &f->rec;

    /* Contacts: Mindy's birthday is Mar 2 (1985), Betty's Mar 4 (1990). */
    snprintf(f->ab_dir, sizeof(f->ab_dir), "/tmp/hu_pd_ab_XXXXXX");
    HU_ASSERT_NOT_NULL(mkdtemp(f->ab_dir));
    char p[160];
    snprintf(p, sizeof(p), "%s/Sources", f->ab_dir);
    mkdir(p, 0700);
    snprintf(p, sizeof(p), "%s/Sources/S1", f->ab_dir);
    mkdir(p, 0700);
    snprintf(p, sizeof(p), "%s/Sources/S1/AddressBook-v22.abcddb", f->ab_dir);
    make_addressbook(p, "(555) 000-0001", -499694400LL);
    snprintf(p, sizeof(p), "%s/Sources/S2", f->ab_dir);
    mkdir(p, 0700);
    snprintf(p, sizeof(p), "%s/Sources/S2/AddressBook-v22.abcddb", f->ab_dir);
    make_addressbook(p, "+1 555 000 0002", -341755200LL);
    setenv("HU_ADDRESSBOOK_DIR", f->ab_dir, 1);
}

static void fixture_end(fixture_t *f) {
    char p[160];
    for (int s = 1; s <= 2; s++) {
        snprintf(p, sizeof(p), "%s/Sources/S%d/AddressBook-v22.abcddb", f->ab_dir, s);
        unlink(p);
        snprintf(p, sizeof(p), "%s/Sources/S%d", f->ab_dir, s);
        rmdir(p);
    }
    snprintf(p, sizeof(p), "%s/Sources", f->ab_dir);
    rmdir(p);
    rmdir(f->ab_dir);
    f->mem.vtable->deinit(f->mem.ctx);
    unsetenv("HU_DATES");
    unsetenv("HU_ADDRESSBOOK_DIR");
    unsetenv("HU_BRIEFING");
    tz_end();
}

static bool say(fixture_t *f, const char *from, const char *text, char *reply, size_t cap) {
    return hu_person_dates_handle_owner_message(&f->agent, from, strlen(from), text, strlen(text),
                                                local_at(2026, 2, 20, 10), reply, cap);
}

static void person_dates_owner_statements_are_stored_and_acked(void) {
    fixture_t f;
    fixture_begin(&f, "live");
    char reply[256];
    HU_ASSERT_FALSE(say(&f, SISTER, "mom's birthday is march 3", reply, sizeof(reply)));
    HU_ASSERT_TRUE(say(&f, OWNER, "mom's birthday is march 3", reply, sizeof(reply)));
    HU_ASSERT_STR_EQ(reply, "got it: Betty's birthday, March 3");
    HU_ASSERT_TRUE(say(&f, OWNER, "our anniversary is june 12", reply, sizeof(reply)));
    HU_ASSERT_STR_EQ(reply, "got it: your anniversary, June 12");
    /* A clear date statement about someone unknown is answered, not guessed. */
    HU_ASSERT_TRUE(say(&f, OWNER, "zelda's birthday is march 3", reply, sizeof(reply)));
    HU_ASSERT_STR_EQ(reply,
                     "who's zelda? I couldn't match that to exactly one person in your contacts");
    HU_ASSERT_FALSE(say(&f, OWNER, "how was your day", reply, sizeof(reply)));
    fixture_end(&f);
}

static void person_dates_do_nothing_unless_live(void) {
    const char *gates[] = {"off", "shadow"};
    for (size_t i = 0; i < 2; i++) {
        fixture_t f;
        fixture_begin(&f, gates[i]);
        char reply[256];
        HU_ASSERT_FALSE(say(&f, OWNER, "mom's birthday is march 3", reply, sizeof(reply)));
        hu_briefing_date_t d[4];
        HU_ASSERT_EQ(hu_person_dates_upcoming(&f.agent, local_at(2026, 2, 28, 8), 7, d, 4), 0);
        fixture_end(&f);
    }
}

static void person_dates_merge_owner_and_contacts_soonest_first(void) {
    fixture_t f;
    fixture_begin(&f, "live");
    char reply[256];
    /* The owner says Betty's birthday is Mar 3; Contacts says Mar 4 (also in the window). The owner
     * wins. */
    HU_ASSERT_TRUE(say(&f, OWNER, "mom's birthday is march 3", reply, sizeof(reply)));
    hu_briefing_date_t d[4];
    size_t n = hu_person_dates_upcoming(&f.agent, local_at(2026, 2, 28, 8), 7, d, 4);
    HU_ASSERT_EQ(n, 2);
    HU_ASSERT_STR_EQ(d[0].label, "Mindy's birthday"); /* from Contacts, Mar 2 */
    HU_ASSERT_EQ(d[0].days_away, 2);
    HU_ASSERT_STR_EQ(d[1].label, "Betty's birthday"); /* from the owner, Mar 3 */
    HU_ASSERT_EQ(d[1].days_away, 3);
    /* Outside the window: nothing. */
    HU_ASSERT_EQ(hu_person_dates_upcoming(&f.agent, local_at(2026, 2, 20, 8), 7, d, 4), 0);
    fixture_end(&f);
}

static void person_dates_reach_the_morning_briefing_by_name(void) {
    fixture_t f;
    fixture_begin(&f, "live");
    setenv("HU_BRIEFING", "live", 1);
    char reply[256];
    HU_ASSERT_TRUE(say(&f, OWNER, "mom's birthday is march 3", reply, sizeof(reply)));
    hu_briefing_tick(&f.agent, &f.svc, 1, local_at(2026, 2, 28, 8));
    HU_ASSERT_EQ(f.rec.sends, 1);
    HU_ASSERT_STR_EQ(f.rec.text, "morning. here's today:\n"
                                 "- Monday: Mindy's birthday\n"
                                 "- Tuesday: Betty's birthday");
    fixture_end(&f);
}
#endif /* HU_ENABLE_SQLITE */

void run_daemon_person_dates_tests(void) {
    HU_TEST_SUITE("daemon person dates");
    HU_RUN_TEST(person_date_parse_accepts_the_ways_people_say_it);
    HU_RUN_TEST(person_date_parse_refuses_what_is_not_a_date_statement);
    HU_RUN_TEST(person_date_days_away_wraps_the_year_and_handles_feb_29);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(person_dates_owner_statements_are_stored_and_acked);
    HU_RUN_TEST(person_dates_do_nothing_unless_live);
    HU_RUN_TEST(person_dates_merge_owner_and_contacts_soonest_first);
    HU_RUN_TEST(person_dates_reach_the_morning_briefing_by_name);
#endif
}
