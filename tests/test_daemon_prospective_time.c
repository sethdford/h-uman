/* tests/test_daemon_prospective_time.c
 *
 * The proactive tick's time-cued producers (src/daemon/daemon_prospective_time.c),
 * moved out of hu_service_run unchanged: the F20 commitment follow-up lines
 * and the proposer's due_followups section. Task 9 adds the
 * HU_PROSPECTIVE_TIME gate on top of these. */
#include "test_framework.h"

#include "human/agent.h"
#include "human/daemon/prospective_time.h"
#include <string.h>

static void time_producers_need_memory_and_a_contact(void) {
    hu_allocator_t alloc = hu_system_allocator();
    static hu_agent_t agent;
    memset(&agent, 0, sizeof(agent)); /* no memory */
    char *ctx = (char *)&agent;
    size_t len = 9;
    int64_t ids[3];
    size_t n = 9;
    hu_daemon_prospective_commitment_ctx(&alloc, &agent, "+15550000001", 100, &ctx, &len, ids, &n);
    HU_ASSERT_NULL(ctx);
    HU_ASSERT_EQ(len, (size_t)0);
    HU_ASSERT_EQ(n, (size_t)0);
    char buf[64] = "stale";
    int64_t listed = -1;
    HU_ASSERT_EQ(hu_daemon_prospective_due_followups(&alloc, &agent, NULL, NULL, 0, "+15550000001",
                                                     100, buf, sizeof(buf), &listed),
                 (size_t)0);
    HU_ASSERT_STR_EQ(buf, "");
    HU_ASSERT_EQ(listed, (int64_t)-1);
}

#ifdef HU_ENABLE_SQLITE
#include "human/memory.h"
#include "human/memory/superhuman.h"

static void commitment_ctx_lists_this_contacts_due_commitments(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    static hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.memory = &mem;
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, "+15550000001", 12,
                                                "call the dentist", 16, "me", 2, 1000),
                 HU_OK);
    HU_ASSERT_EQ(hu_superhuman_commitment_store(&mem, &alloc, "+15550000002", 12,
                                                "return the drill", 16, "me", 2, 1000),
                 HU_OK);
    char *ctx = NULL;
    size_t len = 0;
    int64_t ids[3];
    size_t n = 0;
    hu_daemon_prospective_commitment_ctx(&alloc, &agent, "+15550000001", 5000, &ctx, &len, ids, &n);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_STR_EQ(ctx, "COMMITMENT FOLLOW-UP: call the dentist was due. Ask if it happened: "
                          "'hey did you ever call the dentist?'\n");
    HU_ASSERT_EQ(len, strlen(ctx));
    HU_ASSERT_EQ(n, (size_t)1);
    HU_ASSERT_TRUE(ids[0] > 0);
    alloc.free(alloc.ctx, ctx, len + 1);
    mem.vtable->deinit(mem.ctx);
}

static void due_followups_lists_one_line_for_this_contact(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    static hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.memory = &mem;
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_schedule(&mem, &alloc, "+15550000002", 12,
                                                         "their trip", 10, 500),
                 HU_OK);
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_schedule(&mem, &alloc, "+15550000001", 12,
                                                         "the job interview", 17, 1000),
                 HU_OK);
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_schedule(&mem, &alloc, "+15550000001", 12,
                                                         "the move", 8, 2000),
                 HU_OK);
    char buf[640];
    int64_t listed = -1;
    size_t n = hu_daemon_prospective_due_followups(&alloc, &agent, NULL, NULL, 0, "+15550000001",
                                                   5000, buf, sizeof(buf), &listed);
    HU_ASSERT_STR_EQ(buf, "- the job interview (due 4000s ago)\n"); /* oldest due, one line */
    HU_ASSERT_EQ(n, strlen(buf));
    HU_ASSERT_TRUE(listed > 0);
    listed = -1;
    HU_ASSERT_EQ(hu_daemon_prospective_due_followups(&alloc, &agent, NULL, NULL, 0, "+15550000003",
                                                     5000, buf, sizeof(buf), &listed),
                 (size_t)0);
    HU_ASSERT_EQ(listed, (int64_t)-1);
    mem.vtable->deinit(mem.ctx);
}
#endif /* HU_ENABLE_SQLITE */

void run_daemon_prospective_time_tests(void) {
    HU_TEST_SUITE("daemon prospective time");
    HU_RUN_TEST(time_producers_need_memory_and_a_contact);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(commitment_ctx_lists_this_contacts_due_commitments);
    HU_RUN_TEST(due_followups_lists_one_line_for_this_contact);
#endif
}
