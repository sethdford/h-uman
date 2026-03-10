/* Awareness module tests — bus event tracking + context string */
#include "human/agent/awareness.h"
#include "human/bus.h"
#include "human/core/allocator.h"
#include "test_framework.h"
#include <string.h>

static void test_awareness_init_null(void) {
    hu_awareness_t aw;
    HU_ASSERT_EQ(hu_awareness_init(NULL, NULL), HU_ERR_INVALID_ARGUMENT);
    hu_bus_t bus;
    hu_bus_init(&bus);
    HU_ASSERT_EQ(hu_awareness_init(&aw, NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_awareness_init(NULL, &bus), HU_ERR_INVALID_ARGUMENT);
}

static void test_awareness_init_deinit(void) {
    hu_bus_t bus;
    hu_bus_init(&bus);
    hu_awareness_t aw;
    hu_error_t err = hu_awareness_init(&aw, &bus);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(aw.state.messages_received, 0);
    HU_ASSERT_EQ(aw.state.messages_sent, 0);
    HU_ASSERT_EQ(aw.state.tool_calls, 0);
    HU_ASSERT_EQ(aw.state.total_errors, 0);
    HU_ASSERT_FALSE(aw.state.health_degraded);
    hu_awareness_deinit(&aw);
}

static void test_awareness_tracks_messages(void) {
    hu_bus_t bus;
    hu_bus_init(&bus);
    hu_awareness_t aw;
    hu_awareness_init(&aw, &bus);

    hu_bus_publish_simple(&bus, HU_BUS_MESSAGE_RECEIVED, "cli", "s1", "hello");
    hu_bus_publish_simple(&bus, HU_BUS_MESSAGE_RECEIVED, "cli", "s1", "world");
    hu_bus_publish_simple(&bus, HU_BUS_MESSAGE_SENT, "cli", "s1", "reply");
    HU_ASSERT_EQ(aw.state.messages_received, 2);
    HU_ASSERT_EQ(aw.state.messages_sent, 1);

    hu_awareness_deinit(&aw);
}

static void test_awareness_tracks_tool_calls(void) {
    hu_bus_t bus;
    hu_bus_init(&bus);
    hu_awareness_t aw;
    hu_awareness_init(&aw, &bus);

    hu_bus_publish_simple(&bus, HU_BUS_TOOL_CALL, "cli", "s1", "shell");
    hu_bus_publish_simple(&bus, HU_BUS_TOOL_CALL, "cli", "s1", "http");
    HU_ASSERT_EQ(aw.state.tool_calls, 2);

    hu_awareness_deinit(&aw);
}

static void test_awareness_tracks_errors(void) {
    hu_bus_t bus;
    hu_bus_init(&bus);
    hu_awareness_t aw;
    hu_awareness_init(&aw, &bus);

    hu_bus_publish_simple(&bus, HU_BUS_ERROR, "", "", "connection timeout");
    hu_bus_publish_simple(&bus, HU_BUS_ERROR, "", "", "rate limited");
    HU_ASSERT_EQ(aw.state.total_errors, 2);
    HU_ASSERT_EQ(aw.state.error_write_idx, 2);

    hu_awareness_deinit(&aw);
}

static void test_awareness_error_circular_buffer(void) {
    hu_bus_t bus;
    hu_bus_init(&bus);
    hu_awareness_t aw;
    hu_awareness_init(&aw, &bus);

    for (int i = 0; i < HU_AWARENESS_MAX_RECENT_ERRORS + 3; i++) {
        char msg[32];
        snprintf(msg, sizeof(msg), "error-%d", i);
        hu_bus_publish_simple(&bus, HU_BUS_ERROR, "", "", msg);
    }
    HU_ASSERT_EQ(aw.state.total_errors, (size_t)(HU_AWARENESS_MAX_RECENT_ERRORS + 3));
    HU_ASSERT_EQ(aw.state.error_write_idx, (size_t)(HU_AWARENESS_MAX_RECENT_ERRORS + 3));

    hu_awareness_deinit(&aw);
}

static void test_awareness_tracks_channels(void) {
    hu_bus_t bus;
    hu_bus_init(&bus);
    hu_awareness_t aw;
    hu_awareness_init(&aw, &bus);

    hu_bus_publish_simple(&bus, HU_BUS_MESSAGE_RECEIVED, "telegram", "u1", "hi");
    hu_bus_publish_simple(&bus, HU_BUS_MESSAGE_RECEIVED, "discord", "u2", "hey");
    hu_bus_publish_simple(&bus, HU_BUS_MESSAGE_RECEIVED, "telegram", "u3", "dup");
    HU_ASSERT_EQ(aw.state.active_channel_count, 2);
    HU_ASSERT_STR_EQ(aw.state.active_channels[0], "telegram");
    HU_ASSERT_STR_EQ(aw.state.active_channels[1], "discord");

    hu_awareness_deinit(&aw);
}

static void test_awareness_health_change(void) {
    hu_bus_t bus;
    hu_bus_init(&bus);
    hu_awareness_t aw;
    hu_awareness_init(&aw, &bus);

    hu_bus_publish_simple(&bus, HU_BUS_HEALTH_CHANGE, "", "", "degraded");
    HU_ASSERT_TRUE(aw.state.health_degraded);

    hu_bus_publish_simple(&bus, HU_BUS_HEALTH_CHANGE, "", "", "");
    HU_ASSERT_FALSE(aw.state.health_degraded);

    hu_awareness_deinit(&aw);
}

static void test_awareness_context_null_when_empty(void) {
    hu_bus_t bus;
    hu_bus_init(&bus);
    hu_awareness_t aw;
    hu_awareness_init(&aw, &bus);

    hu_allocator_t alloc = hu_system_allocator();
    size_t len = 0;
    char *ctx = hu_awareness_context(&aw, &alloc, &len);
    HU_ASSERT_NULL(ctx);
    HU_ASSERT_EQ(len, 0);

    hu_awareness_deinit(&aw);
}

static void test_awareness_context_with_activity(void) {
    hu_bus_t bus;
    hu_bus_init(&bus);
    hu_awareness_t aw;
    hu_awareness_init(&aw, &bus);

    hu_bus_publish_simple(&bus, HU_BUS_MESSAGE_RECEIVED, "cli", "s1", "hello");
    hu_bus_publish_simple(&bus, HU_BUS_TOOL_CALL, "cli", "s1", "shell");

    hu_allocator_t alloc = hu_system_allocator();
    size_t len = 0;
    char *ctx = hu_awareness_context(&aw, &alloc, &len);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(ctx, "Situational Awareness") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, "1 msgs received") != NULL);
    alloc.free(alloc.ctx, ctx, len + 1);

    hu_awareness_deinit(&aw);
}

static void test_awareness_context_with_errors(void) {
    hu_bus_t bus;
    hu_bus_init(&bus);
    hu_awareness_t aw;
    hu_awareness_init(&aw, &bus);

    hu_bus_publish_simple(&bus, HU_BUS_ERROR, "", "", "timeout");

    hu_allocator_t alloc = hu_system_allocator();
    size_t len = 0;
    char *ctx = hu_awareness_context(&aw, &alloc, &len);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(strstr(ctx, "Recent errors") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, "timeout") != NULL);
    alloc.free(alloc.ctx, ctx, len + 1);

    hu_awareness_deinit(&aw);
}

static void test_awareness_context_health_degraded(void) {
    hu_bus_t bus;
    hu_bus_init(&bus);
    hu_awareness_t aw;
    hu_awareness_init(&aw, &bus);

    hu_bus_publish_simple(&bus, HU_BUS_HEALTH_CHANGE, "", "", "degraded");

    hu_allocator_t alloc = hu_system_allocator();
    size_t len = 0;
    char *ctx = hu_awareness_context(&aw, &alloc, &len);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(strstr(ctx, "health is degraded") != NULL);
    alloc.free(alloc.ctx, ctx, len + 1);

    hu_awareness_deinit(&aw);
}

static void test_awareness_deinit_null_safe(void) {
    hu_awareness_deinit(NULL);
    hu_awareness_t aw;
    memset(&aw, 0, sizeof(aw));
    hu_awareness_deinit(&aw);
}

static void test_awareness_context_null_args(void) {
    HU_ASSERT_NULL(hu_awareness_context(NULL, NULL, NULL));
}

void run_awareness_tests(void) {
    HU_TEST_SUITE("Awareness");
    HU_RUN_TEST(test_awareness_init_null);
    HU_RUN_TEST(test_awareness_init_deinit);
    HU_RUN_TEST(test_awareness_tracks_messages);
    HU_RUN_TEST(test_awareness_tracks_tool_calls);
    HU_RUN_TEST(test_awareness_tracks_errors);
    HU_RUN_TEST(test_awareness_error_circular_buffer);
    HU_RUN_TEST(test_awareness_tracks_channels);
    HU_RUN_TEST(test_awareness_health_change);
    HU_RUN_TEST(test_awareness_context_null_when_empty);
    HU_RUN_TEST(test_awareness_context_with_activity);
    HU_RUN_TEST(test_awareness_context_with_errors);
    HU_RUN_TEST(test_awareness_context_health_degraded);
    HU_RUN_TEST(test_awareness_deinit_null_safe);
    HU_RUN_TEST(test_awareness_context_null_args);
}
