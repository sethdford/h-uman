/* tests/test_daemon_contact_optout.c — October roadmap O5 (contestability).
 *
 * Pins src/daemon/daemon_contact_optout.c: the pure phrase detector (positive
 * phrases, negation, word boundaries), the HU_CONTACT_OPTOUT escape hatch,
 * and the DB-level pre/post contract the daemon relies on:
 *   not suppressed → observe(opt-out text) → suppressed → proactive skip,
 * plus the doctor card count (src/doctor/check_contact_optout.c).
 */
#include "human/daemon_contact_optout.h"
#include "human/doctor/check_ops.h"
#include "human/memory.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef HU_ENABLE_SQLITE
#include "human/memory/contact_optout_repo.h"
#endif

static bool detect(const char *s) {
    return hu_contact_optout_detect(s, s ? strlen(s) : 0);
}

static void test_optout_detect_positive_phrases(void) {
    HU_ASSERT_TRUE(detect("please stop texting me"));
    HU_ASSERT_TRUE(detect("STOP TEXTING ME."));
    HU_ASSERT_TRUE(detect("ok just leave me alone"));
    HU_ASSERT_TRUE(detect("unsubscribe"));
    HU_ASSERT_TRUE(detect("don't text me again"));
    HU_ASSERT_TRUE(detect("Do not message me, thanks"));
    HU_ASSERT_TRUE(detect("lose my number"));
    HU_ASSERT_TRUE(detect("can you quit texting me"));
}

static void test_optout_detect_negation_boundaries_and_empties(void) {
    /* A negation right before the phrase flips it. */
    HU_ASSERT_TRUE(!detect("don't stop texting me"));
    HU_ASSERT_TRUE(!detect("never stop texting me lol"));
    HU_ASSERT_TRUE(!detect("you won't stop texting me haha"));
    /* Word boundaries: "unsubscribed" is not "unsubscribe". */
    HU_ASSERT_TRUE(!detect("i unsubscribed from that newsletter"));
    /* Ordinary texts. */
    HU_ASSERT_TRUE(!detect("what time is the game"));
    HU_ASSERT_TRUE(!detect("text me when you land"));
    HU_ASSERT_TRUE(!detect(""));
    HU_ASSERT_TRUE(!hu_contact_optout_detect(NULL, 0));
    /* Length-bounded: the phrase must be inside `len`. */
    HU_ASSERT_TRUE(!hu_contact_optout_detect("stop texting me", 4));
}

static void test_optout_env_default_on_only_exact_off_disables(void) {
    const char *prev = getenv("HU_CONTACT_OPTOUT");
    char *saved = prev ? strdup(prev) : NULL;
    unsetenv("HU_CONTACT_OPTOUT");
    HU_ASSERT_TRUE(hu_contact_optout_enabled());
    setenv("HU_CONTACT_OPTOUT", "off", 1);
    HU_ASSERT_TRUE(!hu_contact_optout_enabled());
    setenv("HU_CONTACT_OPTOUT", "on", 1);
    HU_ASSERT_TRUE(hu_contact_optout_enabled());
    setenv("HU_CONTACT_OPTOUT", "Off", 1); /* only the exact escape hatch disables */
    HU_ASSERT_TRUE(hu_contact_optout_enabled());
    if (saved) {
        setenv("HU_CONTACT_OPTOUT", saved, 1);
        free(saved);
    } else {
        unsetenv("HU_CONTACT_OPTOUT");
    }
}

#ifdef HU_ENABLE_SQLITE
static void test_optout_observe_then_proactive_skip(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    struct sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    const char *c = "+15551230000";
    /* Pre. */
    HU_ASSERT_TRUE(!hu_contact_optout_is_suppressed_db(db, c));
    /* An ordinary inbound text must not suppress. */
    const char *ok = "sounds good see you sat";
    HU_ASSERT_TRUE(!hu_contact_optout_observe_db(db, c, strlen(c), ok, strlen(ok), 1000));
    HU_ASSERT_TRUE(!hu_contact_optout_is_suppressed_db(db, c));
    /* The opt-out does, within the same call. */
    const char *stop = "please stop texting me";
    HU_ASSERT_TRUE(hu_contact_optout_observe_db(db, c, strlen(c), stop, strlen(stop), 1000));
    HU_ASSERT_TRUE(hu_contact_optout_is_suppressed_db(db, c));
    /* Other contacts are untouched; the reversal works. */
    HU_ASSERT_TRUE(!hu_contact_optout_is_suppressed_db(db, "+15559990000"));
    HU_ASSERT_EQ(hu_contact_optout_repo_clear(db, c), HU_OK);
    HU_ASSERT_TRUE(!hu_contact_optout_is_suppressed_db(db, c));
    mem.vtable->deinit(mem.ctx);
}

static void test_optout_doctor_check_counts_suppressed_contacts(void) {
    char path[256];
    snprintf(path, sizeof(path), "/tmp/hu_optout_doctor_%d.db", (int)getpid());
    unlink(path);
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, path);
    HU_ASSERT_NOT_NULL(mem.ctx);
    struct sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_EQ(hu_contact_optout_repo_record(db, 1, "+15551230000", NULL, NULL), HU_OK);
    HU_ASSERT_EQ(hu_contact_optout_repo_record(db, 2, "+15559990000", NULL, NULL), HU_OK);
    mem.vtable->deinit(mem.ctx);

    hu_doctor_contact_optout_ctx_t ctx = {.memory_db = path};
    hu_doctor_check_result_t r = hu_doctor_check_contact_optout.run(NULL, &ctx);
    HU_ASSERT_EQ((int)r.verdict, (int)HU_DOCTOR_PASS);
    HU_ASSERT_NOT_NULL(r.detail_json);
    HU_ASSERT_TRUE(strstr(r.detail_json, "\"suppressed_contacts\":2") != NULL);
    HU_ASSERT_TRUE(strstr(r.reason, "2 contact(s)") != NULL);
    unlink(path);

    hu_doctor_contact_optout_ctx_t missing = {.memory_db = "/nonexistent/hu_optout.db"};
    r = hu_doctor_check_contact_optout.run(NULL, &missing);
    HU_ASSERT_EQ((int)r.verdict, (int)HU_DOCTOR_FAIL);
}
#endif

void run_daemon_contact_optout_tests(void) {
    HU_RUN_TEST(test_optout_detect_positive_phrases);
    HU_RUN_TEST(test_optout_detect_negation_boundaries_and_empties);
    HU_RUN_TEST(test_optout_env_default_on_only_exact_off_disables);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_optout_observe_then_proactive_skip);
    HU_RUN_TEST(test_optout_doctor_check_counts_suppressed_contacts);
#endif
}
