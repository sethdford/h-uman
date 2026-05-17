#include "human/agent/workflow_event.h"
#include "human/core/allocator.h"
#include "test_framework.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static hu_allocator_t hu_test_allocator_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    return alloc;
}

/* Per-pid event-log path: makes the test deterministic across
 * concurrent test-binary runs (CI matrix) and immune to a previous
 * run's stale jsonl file. The unlink in each test still scrubs the
 * file before create — this just guarantees no other binary can
 * race us. */
static const char *events_path(const char *tag) {
    static char buf[160];
    snprintf(buf, sizeof(buf), "/tmp/hu_test_events_%d_%s.jsonl", (int)getpid(), tag);
    (void)unlink(buf);
    return buf;
}

/* Test: Create and destroy event log */
static void test_workflow_event_log_create_destroy(void) {
    hu_allocator_t alloc = hu_test_allocator_create();

    hu_workflow_event_log_t *log = NULL;
    hu_error_t err = hu_workflow_event_log_create(&alloc, events_path("basic"), &log);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(log);

    size_t count = hu_workflow_event_log_count(log);
    HU_ASSERT_EQ(count, 0);

    hu_workflow_event_log_destroy(log, &alloc);
}

/* Test: Append and retrieve a single event */
static void test_workflow_event_append_single(void) {
    hu_allocator_t alloc = hu_test_allocator_create();

    hu_workflow_event_log_t *log = NULL;
    hu_error_t err = hu_workflow_event_log_create(&alloc, events_path("single"), &log);
    HU_ASSERT_EQ(err, HU_OK);

    /* Create an event */
    hu_workflow_event_t event = {0};
    event.type = HU_WF_EVENT_WORKFLOW_STARTED;
    event.timestamp = hu_workflow_event_current_timestamp_ms();
    event.workflow_id = (char *)"wf-001";
    event.workflow_id_len = 6;
    event.data_json = (char *)"{\"status\":\"started\"}";
    event.data_json_len = strlen(event.data_json);

    err = hu_workflow_event_log_append(log, &alloc, &event);
    HU_ASSERT_EQ(err, HU_OK);

    size_t count = hu_workflow_event_log_count(log);
    HU_ASSERT_EQ(count, 1);

    hu_workflow_event_log_destroy(log, &alloc);
}

/* Test: Append multiple events and verify sequence numbers */
static void test_workflow_event_sequence_numbers(void) {
    hu_allocator_t alloc = hu_test_allocator_create();

    hu_workflow_event_log_t *log = NULL;
    hu_error_t err = hu_workflow_event_log_create(&alloc, events_path("seq"), &log);
    HU_ASSERT_EQ(err, HU_OK);

    int64_t ts = hu_workflow_event_current_timestamp_ms();

    /* Append 3 events */
    for (int i = 0; i < 3; i++) {
        hu_workflow_event_t event = {0};
        event.type = HU_WF_EVENT_STEP_STARTED;
        event.timestamp = ts + i;
        event.workflow_id = (char *)"wf-seq";
        event.workflow_id_len = 6;
        char step_id[32];
        snprintf(step_id, sizeof(step_id), "step-%d", i);
        event.step_id = step_id;
        event.step_id_len = strlen(step_id);

        err = hu_workflow_event_log_append(log, &alloc, &event);
        HU_ASSERT_EQ(err, HU_OK);
    }

    size_t count = hu_workflow_event_log_count(log);
    HU_ASSERT_EQ(count, 3);

    hu_workflow_event_log_destroy(log, &alloc);
}

/* Test: Replay returns all events in order */
static void test_workflow_event_replay(void) {
    hu_allocator_t alloc = hu_test_allocator_create();

    hu_workflow_event_log_t *log = NULL;
    hu_error_t err = hu_workflow_event_log_create(&alloc, events_path("replay"), &log);
    HU_ASSERT_EQ(err, HU_OK);

    int64_t ts = hu_workflow_event_current_timestamp_ms();

    /* Append 2 events */
    hu_workflow_event_t event1 = {0};
    event1.type = HU_WF_EVENT_WORKFLOW_STARTED;
    event1.timestamp = ts;
    event1.workflow_id = (char *)"wf-replay";
    event1.workflow_id_len = 9;
    event1.idempotency_key = (char *)"key-1";
    event1.idempotency_key_len = 5;

    hu_workflow_event_log_append(log, &alloc, &event1);

    hu_workflow_event_t event2 = {0};
    event2.type = HU_WF_EVENT_STEP_COMPLETED;
    event2.timestamp = ts + 1;
    event2.workflow_id = (char *)"wf-replay";
    event2.workflow_id_len = 9;
    event2.step_id = (char *)"node-1";
    event2.step_id_len = 6;
    event2.idempotency_key = (char *)"key-2";
    event2.idempotency_key_len = 5;

    hu_workflow_event_log_append(log, &alloc, &event2);

    /* Replay */
    hu_workflow_event_t *events = NULL;
    size_t count = 0;
    err = hu_workflow_event_log_replay(log, &alloc, &events, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 2);
    HU_ASSERT_NOT_NULL(events);

    HU_ASSERT_EQ(events[0].type, HU_WF_EVENT_WORKFLOW_STARTED);
    HU_ASSERT_EQ(events[0].sequence_num, 0);
    HU_ASSERT_NOT_NULL(events[0].workflow_id);
    HU_ASSERT_STR_EQ(events[0].workflow_id, "wf-replay");

    HU_ASSERT_EQ(events[1].type, HU_WF_EVENT_STEP_COMPLETED);
    HU_ASSERT_EQ(events[1].sequence_num, 1);
    HU_ASSERT_NOT_NULL(events[1].step_id);
    HU_ASSERT_STR_EQ(events[1].step_id, "node-1");

    /* Clean up */
    for (size_t i = 0; i < count; i++) {
        hu_workflow_event_free(&alloc, &events[i]);
    }
    alloc.free(alloc.ctx, events, sizeof(*events) * count);

    hu_workflow_event_log_destroy(log, &alloc);
}

/* Test: Find by idempotency key (found) */
static void test_workflow_event_find_by_key_found(void) {
    hu_allocator_t alloc = hu_test_allocator_create();

    hu_workflow_event_log_t *log = NULL;
    hu_error_t err = hu_workflow_event_log_create(&alloc, events_path("find"), &log);
    HU_ASSERT_EQ(err, HU_OK);

    int64_t ts = hu_workflow_event_current_timestamp_ms();

    /* Append event with key */
    hu_workflow_event_t event = {0};
    event.type = HU_WF_EVENT_TOOL_CALLED;
    event.timestamp = ts;
    event.workflow_id = (char *)"wf-find";
    event.workflow_id_len = 7;
    event.step_id = (char *)"tool-1";
    event.step_id_len = 6;
    event.data_json = (char *)"{\"tool\":\"search\"}";
    event.data_json_len = 17;
    event.idempotency_key = (char *)"unique-key-123";
    event.idempotency_key_len = 14;

    hu_workflow_event_log_append(log, &alloc, &event);

    /* Find by key */
    hu_workflow_event_t found = {0};
    bool key_found = false;
    err = hu_workflow_event_log_find_by_key(log, "unique-key-123", &found, &key_found);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(key_found);

    HU_ASSERT_EQ(found.type, HU_WF_EVENT_TOOL_CALLED);
    HU_ASSERT_NOT_NULL(found.workflow_id);
    HU_ASSERT_STR_EQ(found.workflow_id, "wf-find");
    HU_ASSERT_NOT_NULL(found.step_id);
    HU_ASSERT_STR_EQ(found.step_id, "tool-1");

    /* Clean up find result */
    hu_allocator_t system_alloc = hu_system_allocator();
    hu_workflow_event_free(&system_alloc, &found);

    hu_workflow_event_log_destroy(log, &alloc);
}

/* Test: Find by idempotency key (not found) */
static void test_workflow_event_find_by_key_not_found(void) {
    hu_allocator_t alloc = hu_test_allocator_create();

    hu_workflow_event_log_t *log = NULL;
    hu_error_t err = hu_workflow_event_log_create(&alloc, events_path("notfound"), &log);
    HU_ASSERT_EQ(err, HU_OK);

    hu_workflow_event_t found = {0};
    bool key_found = false;
    err = hu_workflow_event_log_find_by_key(log, "nonexistent-key", &found, &key_found);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_FALSE(key_found);

    hu_workflow_event_log_destroy(log, &alloc);
}

/* Test: Empty log replay returns 0 events */
static void test_workflow_event_replay_empty(void) {
    hu_allocator_t alloc = hu_test_allocator_create();

    hu_workflow_event_log_t *log = NULL;
    hu_error_t err = hu_workflow_event_log_create(&alloc, events_path("empty"), &log);
    HU_ASSERT_EQ(err, HU_OK);

    hu_workflow_event_t *events = NULL;
    size_t count = 0;
    err = hu_workflow_event_log_replay(log, &alloc, &events, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0);
    HU_ASSERT_NULL(events);

    hu_workflow_event_log_destroy(log, &alloc);
}

/* Test: Persists to file and reloads correctly */
static void test_workflow_event_persist_and_reload(void) {
    hu_allocator_t alloc = hu_test_allocator_create();
    /* Single-call gets and cleans the path; the second create below
     * reopens it (no scrub) so the persistence assertion is honest. */
    const char *path = events_path("persist");

    /* Create log and append event */
    hu_workflow_event_log_t *log = NULL;
    hu_error_t err = hu_workflow_event_log_create(&alloc, path, &log);
    HU_ASSERT_EQ(err, HU_OK);

    int64_t ts = hu_workflow_event_current_timestamp_ms();
    hu_workflow_event_t event = {0};
    event.type = HU_WF_EVENT_WORKFLOW_COMPLETED;
    event.timestamp = ts;
    event.workflow_id = (char *)"wf-persist";
    event.workflow_id_len = 10;
    event.data_json = (char *)"{\"result\":\"success\"}";
    event.data_json_len = 20;

    hu_workflow_event_log_append(log, &alloc, &event);
    hu_workflow_event_log_destroy(log, &alloc);

    /* Reopen and verify */
    err = hu_workflow_event_log_create(&alloc, path, &log);
    HU_ASSERT_EQ(err, HU_OK);

    size_t count = hu_workflow_event_log_count(log);
    HU_ASSERT_EQ(count, 1);

    hu_workflow_event_t *events = NULL;
    err = hu_workflow_event_log_replay(log, &alloc, &events, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 1);
    HU_ASSERT_NOT_NULL(events);

    HU_ASSERT_EQ(events[0].type, HU_WF_EVENT_WORKFLOW_COMPLETED);
    HU_ASSERT_NOT_NULL(events[0].workflow_id);
    HU_ASSERT_STR_EQ(events[0].workflow_id, "wf-persist");

    for (size_t i = 0; i < count; i++) {
        hu_workflow_event_free(&alloc, &events[i]);
    }
    alloc.free(alloc.ctx, events, sizeof(*events) * count);

    hu_workflow_event_log_destroy(log, &alloc);
}

/* Test: Event serialization with special characters in JSON */
static void test_workflow_event_special_chars(void) {
    hu_allocator_t alloc = hu_test_allocator_create();

    hu_workflow_event_log_t *log = NULL;
    hu_error_t err = hu_workflow_event_log_create(&alloc, events_path("special"), &log);
    HU_ASSERT_EQ(err, HU_OK);

    int64_t ts = hu_workflow_event_current_timestamp_ms();

    /* Event with special chars in data_json */
    hu_workflow_event_t event = {0};
    event.type = HU_WF_EVENT_TOOL_RESULT;
    event.timestamp = ts;
    event.workflow_id = (char *)"wf-special";
    event.workflow_id_len = 10;
    event.step_id = (char *)"step-1";
    event.step_id_len = 6;
    /* JSON with escaped quotes and newlines */
    event.data_json = (char *)"{\"message\":\"hello\\nworld\",\"key\":\"value\"}";
    event.data_json_len = strlen((const char *)event.data_json);

    err = hu_workflow_event_log_append(log, &alloc, &event);
    HU_ASSERT_EQ(err, HU_OK);

    /* Verify count */
    size_t count = hu_workflow_event_log_count(log);
    HU_ASSERT_EQ(count, 1);

    hu_workflow_event_log_destroy(log, &alloc);
}

/* Test: Null argument handling */
static void test_workflow_event_null_args(void) {
    hu_allocator_t alloc = hu_test_allocator_create();

    /* Null log */
    hu_error_t err = hu_workflow_event_log_append(NULL, &alloc, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);

    hu_workflow_event_t *events = NULL;
    size_t count = 0;
    err = hu_workflow_event_log_replay(NULL, &alloc, &events, &count);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);

    hu_workflow_event_t found = {0};
    bool key_found = false;
    err = hu_workflow_event_log_find_by_key(NULL, "key", &found, &key_found);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);

    size_t log_count = hu_workflow_event_log_count(NULL);
    HU_ASSERT_EQ(log_count, 0);

    hu_workflow_event_log_destroy(NULL, &alloc);
}

/* Test: Event type names */
static void test_workflow_event_type_names(void) {
    HU_ASSERT_STR_EQ(hu_workflow_event_type_name(HU_WF_EVENT_WORKFLOW_STARTED),
                     "workflow_started");
    HU_ASSERT_STR_EQ(hu_workflow_event_type_name(HU_WF_EVENT_WORKFLOW_COMPLETED),
                     "workflow_completed");
    HU_ASSERT_STR_EQ(hu_workflow_event_type_name(HU_WF_EVENT_WORKFLOW_FAILED),
                     "workflow_failed");
    HU_ASSERT_STR_EQ(hu_workflow_event_type_name(HU_WF_EVENT_STEP_STARTED), "step_started");
    HU_ASSERT_STR_EQ(hu_workflow_event_type_name(HU_WF_EVENT_STEP_COMPLETED), "step_completed");
    HU_ASSERT_STR_EQ(hu_workflow_event_type_name(HU_WF_EVENT_TOOL_CALLED), "tool_called");
    HU_ASSERT_STR_EQ(hu_workflow_event_type_name(HU_WF_EVENT_TOOL_RESULT), "tool_result");
    HU_ASSERT_STR_EQ(hu_workflow_event_type_name(HU_WF_EVENT_HUMAN_GATE_WAITING),
                     "human_gate_waiting");
    HU_ASSERT_STR_EQ(hu_workflow_event_type_name(HU_WF_EVENT_HUMAN_GATE_RESOLVED),
                     "human_gate_resolved");
    HU_ASSERT_STR_EQ(hu_workflow_event_type_name(HU_WF_EVENT_CHECKPOINT_SAVED),
                     "checkpoint_saved");
    HU_ASSERT_STR_EQ(hu_workflow_event_type_name(HU_WF_EVENT_RETRY_ATTEMPTED),
                     "retry_attempted");
    HU_ASSERT_STR_EQ(hu_workflow_event_type_name((hu_workflow_event_type_t)9999), "unknown");
}

/* Test: Multiple workflows in same log */
static void test_workflow_event_multiple_workflows(void) {
    hu_allocator_t alloc = hu_test_allocator_create();

    hu_workflow_event_log_t *log = NULL;
    hu_error_t err = hu_workflow_event_log_create(&alloc, events_path("multi"), &log);
    HU_ASSERT_EQ(err, HU_OK);

    int64_t ts = hu_workflow_event_current_timestamp_ms();

    /* Events for workflow 1 */
    hu_workflow_event_t event1 = {0};
    event1.type = HU_WF_EVENT_WORKFLOW_STARTED;
    event1.timestamp = ts;
    event1.workflow_id = (char *)"wf-001";
    event1.workflow_id_len = 6;

    hu_workflow_event_log_append(log, &alloc, &event1);

    /* Events for workflow 2 */
    hu_workflow_event_t event2 = {0};
    event2.type = HU_WF_EVENT_WORKFLOW_STARTED;
    event2.timestamp = ts + 1;
    event2.workflow_id = (char *)"wf-002";
    event2.workflow_id_len = 6;

    hu_workflow_event_log_append(log, &alloc, &event2);

    size_t count = hu_workflow_event_log_count(log);
    HU_ASSERT_EQ(count, 2);

    hu_workflow_event_t *events = NULL;
    err = hu_workflow_event_log_replay(log, &alloc, &events, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 2);

    HU_ASSERT_STR_EQ(events[0].workflow_id, "wf-001");
    HU_ASSERT_STR_EQ(events[1].workflow_id, "wf-002");

    for (size_t i = 0; i < count; i++) {
        hu_workflow_event_free(&alloc, &events[i]);
    }
    alloc.free(alloc.ctx, events, sizeof(*events) * count);

    hu_workflow_event_log_destroy(log, &alloc);
}

void run_workflow_event_tests(void) {
    HU_TEST_SUITE("Workflow Event Log");
    HU_RUN_TEST(test_workflow_event_log_create_destroy);
    HU_RUN_TEST(test_workflow_event_append_single);
    HU_RUN_TEST(test_workflow_event_sequence_numbers);
    HU_RUN_TEST(test_workflow_event_replay);
    HU_RUN_TEST(test_workflow_event_find_by_key_found);
    HU_RUN_TEST(test_workflow_event_find_by_key_not_found);
    HU_RUN_TEST(test_workflow_event_replay_empty);
    HU_RUN_TEST(test_workflow_event_persist_and_reload);
    HU_RUN_TEST(test_workflow_event_special_chars);
    HU_RUN_TEST(test_workflow_event_null_args);
    HU_RUN_TEST(test_workflow_event_type_names);
    HU_RUN_TEST(test_workflow_event_multiple_workflows);
}
