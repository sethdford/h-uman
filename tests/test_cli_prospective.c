/* tests/test_cli_prospective.c
 *
 * `human prospective init|probe|backfill` (src/app/cli_prospective.c): the
 * probe the harness drives (spec 2026-09-30 §4.5). Parsing is pure; the run
 * path is exercised against an in-memory store with a scripted judge, and its
 * output lines are the contract scripts/pm_bench_local.py parses. */
#include "test_framework.h"

#include "human/cli_prospective.h"
#include <stdio.h>
#include <string.h>

static void cli_prospective_parse_accepts_the_documented_forms(void) {
    hu_cli_prospective_args_t a;
    char *in[] = {"human",     "prospective",  "probe",     "--full",     "--db",      "/tmp/pm.db",
                  "--contact", "+15550000001", "--now",     "1790000000", "--inbound", "taco place",
                  "--judge",   "fire",         "--history", "/tmp/h.txt"};
    HU_ASSERT_TRUE(hu_cli_prospective_parse(16, in, &a));
    HU_ASSERT_EQ(a.op, HU_CLI_PM_INBOUND);
    HU_ASSERT_TRUE(a.full);
    HU_ASSERT_STR_EQ(a.db, "/tmp/pm.db");
    HU_ASSERT_STR_EQ(a.contact, "+15550000001");
    HU_ASSERT_STR_EQ(a.text, "taco place");
    HU_ASSERT_STR_EQ(a.judge, "fire");
    HU_ASSERT_STR_EQ(a.history_path, "/tmp/h.txt");
    HU_ASSERT_EQ(a.now, 1790000000LL);
    char *tick[] = {"human", "prospective", "probe",    "--db",    "d",     "--contact",
                    "c",     "--tick",      "--shadow", "--group", "--self"};
    HU_ASSERT_TRUE(hu_cli_prospective_parse(11, tick, &a));
    HU_ASSERT_EQ(a.op, HU_CLI_PM_TICK);
    HU_ASSERT_TRUE(a.shadow && a.group && a.self);
    HU_ASSERT_STR_EQ(a.judge, "not_now"); /* default: silence */
    char *dl[] = {"human",     "prospective", "probe",     "--db",      "d",
                  "--contact", "c",           "--deliver", "how was it"};
    HU_ASSERT_TRUE(hu_cli_prospective_parse(9, dl, &a));
    HU_ASSERT_EQ(a.op, HU_CLI_PM_DELIVER);
    char *init[] = {"human", "prospective", "init", "--db", "d"};
    HU_ASSERT_TRUE(hu_cli_prospective_parse(5, init, &a));
    HU_ASSERT_EQ(a.op, HU_CLI_PM_INIT);
}

static void cli_prospective_parse_refuses_unsafe_or_ambiguous_input(void) {
    hu_cli_prospective_args_t a;
    char *no_db[] = {"human", "prospective", "probe", "--contact", "c", "--tick"};
    HU_ASSERT_FALSE(hu_cli_prospective_parse(6, no_db, &a)); /* never a default DB */
    char *two_ops[] = {"human",     "prospective", "probe",  "--db",      "d",
                       "--contact", "c",           "--tick", "--inbound", "x"};
    HU_ASSERT_FALSE(hu_cli_prospective_parse(10, two_ops, &a));
    char *no_op[] = {"human", "prospective", "probe", "--db", "d", "--contact", "c"};
    HU_ASSERT_FALSE(hu_cli_prospective_parse(7, no_op, &a));
    char *bad_judge[] = {"human",     "prospective", "probe",  "--db",    "d",
                         "--contact", "c",           "--tick", "--judge", "yes"};
    HU_ASSERT_FALSE(hu_cli_prospective_parse(10, bad_judge, &a));
    char *bad_now[] = {"human",     "prospective", "probe",  "--db",  "d",
                       "--contact", "c",           "--tick", "--now", "12x"};
    HU_ASSERT_FALSE(hu_cli_prospective_parse(10, bad_now, &a));
    char *no_contact[] = {"human", "prospective", "probe", "--db", "d", "--tick"};
    HU_ASSERT_FALSE(hu_cli_prospective_parse(6, no_contact, &a));
    char *unknown[] = {"human", "prospective", "frobnicate", "--db", "d"};
    HU_ASSERT_FALSE(hu_cli_prospective_parse(5, unknown, &a));
    char *dangling[] = {"human", "prospective", "probe", "--db"};
    HU_ASSERT_FALSE(hu_cli_prospective_parse(4, dangling, &a));
    /* fix round 1 minor: strtoll overflow must be rejected, not silently
     * clamped to LLONG_MAX with errno left dangling. */
    char *overflow_now[] = {
        "human",     "prospective", "probe",  "--db",  "d",
        "--contact", "c",           "--tick", "--now", "999999999999999999999999999999999999"};
    HU_ASSERT_FALSE(hu_cli_prospective_parse(10, overflow_now, &a));
}

#ifdef HU_ENABLE_SQLITE
#include "human/memory.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <unistd.h>

static void read_all(FILE *f, char *buf, size_t cap) {
    rewind(f);
    size_t n = fread(buf, 1, cap - 1, f);
    buf[n] = '\0';
}

static hu_error_t fire_judge(void *ctx, hu_allocator_t *alloc, const char *system,
                             size_t system_len, const char *user, size_t user_len, char **out,
                             size_t *out_len) {
    (void)ctx;
    (void)system;
    (void)system_len;
    (void)user;
    (void)user_len;
    char *b = (char *)alloc->alloc(alloc->ctx, 5);
    memcpy(b, "fire", 5);
    *out = b;
    *out_len = 4;
    return HU_OK;
}

static void cli_prospective_run_prints_the_probe_contract(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_EQ(sqlite3_exec(db,
                              "INSERT INTO prospective_memories(trigger_type,trigger_value,action,"
                              "contact_id,expires_at,fired,created_at) VALUES('keyword',"
                              "'taco place','ask how the new taco place was','+15550000001',0,0,"
                              "100)",
                              NULL, NULL, NULL),
                 SQLITE_OK);
    hu_prospective_judge_t j = {.fn = fire_judge, .ctx = NULL};
    hu_cli_prospective_args_t a;
    char *in[] = {"human", "prospective", "probe",     "--full",
                  "--db",  ":memory:",    "--contact", "+15550000001",
                  "--now", "1790000000",  "--inbound", "the taco place!"};
    HU_ASSERT_TRUE(hu_cli_prospective_parse(12, in, &a));
    FILE *f = tmpfile();
    HU_ASSERT_NOT_NULL(f);
    HU_ASSERT_EQ(hu_cli_prospective_run(&alloc, &mem, &a, NULL, 0, &j, f), HU_OK);
    char buf[1024];
    read_all(f, buf, sizeof(buf));
    fclose(f);
    HU_ASSERT_STR_CONTAINS(buf, "candidates=1 fire=1 resolved=0 cancel=0 not_now=0 "
                                "parse_fail=0 judge_err=0 expired=0 capped=0 bytes=");
    HU_ASSERT_STR_CONTAINS(buf, "item id=1 verdict=fire\n");
    HU_ASSERT_STR_CONTAINS(buf, "[PROSPECTIVE MEMORY: If it fits naturally, you could bring up: "
                                "ask how the new taco place was]\n");

    char *dl[] = {"human",
                  "prospective",
                  "probe",
                  "--db",
                  ":memory:",
                  "--contact",
                  "+15550000001",
                  "--now",
                  "1790000100",
                  "--deliver",
                  "how was the taco place"};
    HU_ASSERT_TRUE(hu_cli_prospective_parse(11, dl, &a));
    f = tmpfile();
    HU_ASSERT_EQ(hu_cli_prospective_run(&alloc, &mem, &a, NULL, 0, &j, f), HU_OK);
    read_all(f, buf, sizeof(buf));
    fclose(f);
    HU_ASSERT_STR_EQ(buf, "surfaced=1 used=1 ignored=0 expired=0\n");

    char *init[] = {"human", "prospective", "init", "--db", ":memory:"};
    HU_ASSERT_TRUE(hu_cli_prospective_parse(5, init, &a));
    f = tmpfile();
    HU_ASSERT_EQ(hu_cli_prospective_run(&alloc, &mem, &a, NULL, 0, &j, f), HU_OK);
    read_all(f, buf, sizeof(buf));
    fclose(f);
    HU_ASSERT_STR_EQ(buf, "ok\n");
    mem.vtable->deinit(mem.ctx);
}

/* fix round 1 minor: --history keeps the LAST cap-1 bytes of the file
 * (most-recent turns), not the first — pm_read_file used to fread() from
 * the start, silently discarding the tail of any history file over
 * PM_HISTORY_CAP. */
static void cli_prospective_read_tail_keeps_the_tail_not_the_head(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char tmpl[] = "/tmp/hu_pm_tail_testXXXXXX";
    int tfd = mkstemp(tmpl);
    HU_ASSERT(tfd >= 0);
    FILE *wf = fdopen(tfd, "wb");
    HU_ASSERT_NOT_NULL(wf);
    for (int i = 0; i < 17000; i++)
        HU_ASSERT_EQ(fputc('A', wf), 'A');
    static const char marker[] = "TAIL_MARKER_XYZ\n";
    HU_ASSERT_EQ(fwrite(marker, 1, sizeof(marker) - 1, wf), sizeof(marker) - 1);
    fclose(wf);

    size_t len = 0;
    char *buf = hu_cli_prospective_read_tail(&alloc, tmpl, 16384, &len);
    unlink(tmpl);
    HU_ASSERT_NOT_NULL(buf);
    HU_ASSERT_TRUE(len <= (size_t)16383);
    HU_ASSERT_TRUE(strstr(buf, "TAIL_MARKER_XYZ") != NULL); /* the tail survived */
    HU_ASSERT_TRUE(strstr(buf, "AAAA") != NULL);            /* still mostly filler */
    alloc.free(alloc.ctx, buf, 16384);

    /* a file at or under cap is read whole, unaffected by the tail logic */
    char tmpl2[] = "/tmp/hu_pm_tail_test2XXXXXX";
    int tfd2 = mkstemp(tmpl2);
    HU_ASSERT(tfd2 >= 0);
    FILE *wf2 = fdopen(tfd2, "wb");
    HU_ASSERT_NOT_NULL(wf2);
    static const char short_body[] = "them: hi\nme: hey\n";
    HU_ASSERT_EQ(fwrite(short_body, 1, sizeof(short_body) - 1, wf2), sizeof(short_body) - 1);
    fclose(wf2);
    size_t len2 = 0;
    char *buf2 = hu_cli_prospective_read_tail(&alloc, tmpl2, 16384, &len2);
    unlink(tmpl2);
    HU_ASSERT_NOT_NULL(buf2);
    HU_ASSERT_STR_EQ(buf2, short_body);
    HU_ASSERT_EQ(len2, sizeof(short_body) - 1);
    alloc.free(alloc.ctx, buf2, 16384);

    HU_ASSERT_NULL(hu_cli_prospective_read_tail(&alloc, "/no/such/path", 16384, &len2));
}
#endif /* HU_ENABLE_SQLITE */

void run_cli_prospective_tests(void) {
    HU_TEST_SUITE("cli prospective");
    HU_RUN_TEST(cli_prospective_parse_accepts_the_documented_forms);
    HU_RUN_TEST(cli_prospective_parse_refuses_unsafe_or_ambiguous_input);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(cli_prospective_run_prints_the_probe_contract);
    HU_RUN_TEST(cli_prospective_read_tail_keeps_the_tail_not_the_head);
#endif
}
