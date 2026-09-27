/* Dated-moment check-ins (src/daemon/daemon_dated_followup.c): a detected
 * dated situation ("interview Thursday") becomes ONE queued check-in for after
 * the event when HU_PROACTIVE_CONTEXTUAL is on; shadow only logs; off does
 * nothing. The queue is delayed_followups, which the proactive proposer lists
 * and marks sent on confirmed delivery. */
#include "test_framework.h"

#ifdef HU_ENABLE_SQLITE
#include "human/daemon/dated_followup.h"
#include "human/memory.h"
#include "human/memory/superhuman.h"
#include <string.h>

static const char CONTACT[] = "+15550001111";
static const char FRAME[] = "they have a job interview on Thursday";
#define NOW     1790000000
#define SEND_AT (NOW + 2 * 86400)

static size_t pending_count(hu_memory_t *mem, hu_allocator_t *a, int64_t now) {
    hu_delayed_followup_t *arr = NULL;
    size_t n = 0;
    if (hu_superhuman_delayed_followup_list_due(mem, a, now, &arr, &n) != HU_OK)
        return 0;
    hu_superhuman_delayed_followup_free(a, arr, n);
    return n;
}

static hu_dated_followup_outcome_t apply(hu_memory_t *mem, hu_allocator_t *a,
                                         hu_contextual_proactive_mode_t mode) {
    return hu_daemon_dated_followup_apply(mem, a, mode, CONTACT, sizeof(CONTACT) - 1, FRAME,
                                          sizeof(FRAME) - 1, SEND_AT, NOW);
}

static void dated_followup_off_queues_nothing(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&a, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    HU_ASSERT_EQ((int)apply(&mem, &a, HU_CONTEXTUAL_PROACTIVE_OFF), (int)HU_DATED_FOLLOWUP_NONE);
    HU_ASSERT_EQ(pending_count(&mem, &a, SEND_AT + 1), 0u);
    mem.vtable->deinit(mem.ctx);
}

static void dated_followup_shadow_logs_but_queues_nothing(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&a, ":memory:");
    HU_ASSERT_EQ((int)apply(&mem, &a, HU_CONTEXTUAL_PROACTIVE_SHADOW),
                 (int)HU_DATED_FOLLOWUP_WOULD_SCHEDULE);
    HU_ASSERT_EQ(pending_count(&mem, &a, SEND_AT + 1), 0u);
    mem.vtable->deinit(mem.ctx);
}

static void dated_followup_on_queues_one_check_in_due_after_the_event(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&a, ":memory:");
    HU_ASSERT_EQ((int)apply(&mem, &a, HU_CONTEXTUAL_PROACTIVE_ON),
                 (int)HU_DATED_FOLLOWUP_SCHEDULED);
    /* Not due before the event, due after it, carrying the situation. */
    HU_ASSERT_EQ(pending_count(&mem, &a, SEND_AT - 1), 0u);
    hu_delayed_followup_t *arr = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_list_due(&mem, &a, SEND_AT + 1, &arr, &n), HU_OK);
    HU_ASSERT_EQ(n, 1u);
    HU_ASSERT_STR_EQ(arr[0].contact_id, CONTACT);
    HU_ASSERT_STR_EQ(arr[0].topic, FRAME);
    HU_ASSERT_EQ(arr[0].scheduled_at, (int64_t)SEND_AT);
    hu_superhuman_delayed_followup_free(&a, arr, n);
    mem.vtable->deinit(mem.ctx);
}

static void dated_followup_same_event_twice_is_queued_once(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&a, ":memory:");
    HU_ASSERT_EQ((int)apply(&mem, &a, HU_CONTEXTUAL_PROACTIVE_ON),
                 (int)HU_DATED_FOLLOWUP_SCHEDULED);
    HU_ASSERT_EQ((int)apply(&mem, &a, HU_CONTEXTUAL_PROACTIVE_ON),
                 (int)HU_DATED_FOLLOWUP_ALREADY_PENDING);
    HU_ASSERT_EQ(pending_count(&mem, &a, SEND_AT + 1), 1u);
    mem.vtable->deinit(mem.ctx);
}

static void dated_followup_can_requeue_after_the_first_was_sent(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&a, ":memory:");
    HU_ASSERT_EQ((int)apply(&mem, &a, HU_CONTEXTUAL_PROACTIVE_ON),
                 (int)HU_DATED_FOLLOWUP_SCHEDULED);
    hu_delayed_followup_t *arr = NULL;
    size_t n = 0;
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_list_due(&mem, &a, SEND_AT + 1, &arr, &n), HU_OK);
    HU_ASSERT_EQ(n, 1u);
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_mark_sent(&mem, arr[0].id), HU_OK);
    hu_superhuman_delayed_followup_free(&a, arr, n);
    /* A later mention of the same event is a new moment, not a duplicate. */
    HU_ASSERT_EQ((int)apply(&mem, &a, HU_CONTEXTUAL_PROACTIVE_ON),
                 (int)HU_DATED_FOLLOWUP_SCHEDULED);
    mem.vtable->deinit(mem.ctx);
}

static void dated_followup_rejects_empty_inputs(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&a, ":memory:");
    HU_ASSERT_EQ((int)hu_daemon_dated_followup_apply(&mem, &a, HU_CONTEXTUAL_PROACTIVE_ON, CONTACT,
                                                     sizeof(CONTACT) - 1, "", 0, SEND_AT, NOW),
                 (int)HU_DATED_FOLLOWUP_NONE);
    HU_ASSERT_EQ((int)hu_daemon_dated_followup_apply(NULL, &a, HU_CONTEXTUAL_PROACTIVE_ON, CONTACT,
                                                     sizeof(CONTACT) - 1, FRAME, sizeof(FRAME) - 1,
                                                     SEND_AT, NOW),
                 (int)HU_DATED_FOLLOWUP_NONE);
    bool exists = true;
    HU_ASSERT_EQ(hu_superhuman_delayed_followup_pending_exists(&mem, CONTACT, sizeof(CONTACT) - 1,
                                                               FRAME, sizeof(FRAME) - 1, &exists),
                 HU_OK);
    HU_ASSERT_FALSE(exists);
    mem.vtable->deinit(mem.ctx);
}

void run_daemon_dated_followup_tests(void) {
    HU_TEST_SUITE("daemon_dated_followup");
    HU_RUN_TEST(dated_followup_off_queues_nothing);
    HU_RUN_TEST(dated_followup_shadow_logs_but_queues_nothing);
    HU_RUN_TEST(dated_followup_on_queues_one_check_in_due_after_the_event);
    HU_RUN_TEST(dated_followup_same_event_twice_is_queued_once);
    HU_RUN_TEST(dated_followup_can_requeue_after_the_first_was_sent);
    HU_RUN_TEST(dated_followup_rejects_empty_inputs);
}

#else

void run_daemon_dated_followup_tests(void) {
    (void)0;
}

#endif
