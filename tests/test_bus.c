/* Event bus pub/sub tests */
#include "human/bus.h"
#include "human/core/allocator.h"
#include "test_framework.h"
#include <string.h>

static int bus_received_count;
static hu_bus_event_type_t bus_last_type;

static bool bus_subscriber_fn(hu_bus_event_type_t type, const hu_bus_event_t *ev, void *user_ctx) {
    (void)user_ctx;
    (void)ev;
    bus_received_count++;
    bus_last_type = type;
    return true; /* stay subscribed */
}

static void test_bus_init(void) {
    hu_bus_t bus;
    hu_bus_init(&bus);
    HU_ASSERT_EQ(bus.count, 0);
}

static void test_bus_subscribe_publish(void) {
    hu_bus_t bus;
    hu_bus_init(&bus);
    bus_received_count = 0;

    hu_error_t err = hu_bus_subscribe(&bus, bus_subscriber_fn, NULL, HU_BUS_EVENT_COUNT);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(bus.count, 1);

    hu_bus_event_t ev = {0};
    ev.type = HU_BUS_MESSAGE_RECEIVED;
    hu_bus_publish(&bus, &ev);
    HU_ASSERT_EQ(bus_received_count, 1);
    HU_ASSERT_EQ(bus_last_type, HU_BUS_MESSAGE_RECEIVED);

    hu_bus_publish_simple(&bus, HU_BUS_TOOL_CALL, "cli", "sess1", "run shell");
    HU_ASSERT_EQ(bus_received_count, 2);
    HU_ASSERT_EQ(bus_last_type, HU_BUS_TOOL_CALL);
}

static void test_bus_filter(void) {
    hu_bus_t bus;
    hu_bus_init(&bus);
    bus_received_count = 0;

    /* Subscribe only to MESSAGE_SENT */
    hu_bus_subscribe(&bus, bus_subscriber_fn, NULL, HU_BUS_MESSAGE_SENT);
    hu_bus_publish_simple(&bus, HU_BUS_MESSAGE_RECEIVED, "x", "y", "");
    HU_ASSERT_EQ(bus_received_count, 0);
    hu_bus_publish_simple(&bus, HU_BUS_MESSAGE_SENT, "x", "y", "");
    HU_ASSERT_EQ(bus_received_count, 1);
}

static void test_bus_unsubscribe(void) {
    hu_bus_t bus;
    hu_bus_init(&bus);
    bus_received_count = 0;
    hu_bus_subscribe(&bus, bus_subscriber_fn, NULL, HU_BUS_EVENT_COUNT);
    hu_bus_unsubscribe(&bus, bus_subscriber_fn, NULL);
    HU_ASSERT_EQ(bus.count, 0);
    hu_bus_publish_simple(&bus, HU_BUS_ERROR, "a", "b", "err");
    HU_ASSERT_EQ(bus_received_count, 0);
}

static void test_bus_publish_full_event(void) {
    hu_bus_t bus;
    hu_bus_init(&bus);
    bus_received_count = 0;
    bus_last_type = HU_BUS_EVENT_COUNT;
    hu_bus_subscribe(&bus, bus_subscriber_fn, NULL, HU_BUS_EVENT_COUNT);

    hu_bus_event_t ev = {0};
    ev.type = HU_BUS_HEALTH_CHANGE;
    memcpy(ev.channel, "telegram", 8);
    memcpy(ev.id, "sess_xyz", 8);
    memcpy(ev.message, "online", 6);
    hu_bus_publish(&bus, &ev);

    HU_ASSERT_EQ(bus_received_count, 1);
    HU_ASSERT_EQ(bus_last_type, HU_BUS_HEALTH_CHANGE);
}

static int bus_sub1_count;
static int bus_sub2_count;

static bool bus_sub1_fn(hu_bus_event_type_t type, const hu_bus_event_t *ev, void *ctx) {
    (void)type;
    (void)ev;
    (void)ctx;
    bus_sub1_count++;
    return true;
}

static bool bus_sub2_fn(hu_bus_event_type_t type, const hu_bus_event_t *ev, void *ctx) {
    (void)type;
    (void)ev;
    (void)ctx;
    bus_sub2_count++;
    return true;
}

static void test_bus_multi_subscriber(void) {
    bus_sub1_count = 0;
    bus_sub2_count = 0;

    hu_bus_t bus;
    hu_bus_init(&bus);
    hu_bus_subscribe(&bus, bus_sub1_fn, NULL, HU_BUS_EVENT_COUNT);
    hu_bus_subscribe(&bus, bus_sub2_fn, NULL, HU_BUS_EVENT_COUNT);

    hu_bus_publish_simple(&bus, HU_BUS_MESSAGE_RECEIVED, "ch", "id", "msg");
    HU_ASSERT_EQ(bus_sub1_count, 1);
    HU_ASSERT_EQ(bus_sub2_count, 1);

    hu_bus_publish_simple(&bus, HU_BUS_TOOL_CALL, "cli", "s1", "run");
    HU_ASSERT_EQ(bus_sub1_count, 2);
    HU_ASSERT_EQ(bus_sub2_count, 2);
}

static bool bus_only_tool_fn(hu_bus_event_type_t type, const hu_bus_event_t *ev, void *ctx) {
    (void)ev;
    (void)ctx;
    if (type == HU_BUS_TOOL_CALL)
        bus_received_count++;
    return true;
}

static void test_bus_subscribe_filtered_multiple(void) {
    bus_received_count = 0;
    hu_bus_t bus;
    hu_bus_init(&bus);
    hu_bus_subscribe(&bus, bus_only_tool_fn, NULL, HU_BUS_TOOL_CALL);
    hu_bus_publish_simple(&bus, HU_BUS_MESSAGE_RECEIVED, "x", "y", "");
    hu_bus_publish_simple(&bus, HU_BUS_TOOL_CALL, "x", "y", "shell");
    hu_bus_publish_simple(&bus, HU_BUS_ERROR, "x", "y", "");

    HU_ASSERT_EQ(bus_received_count, 1);
}

static char bus_received_channel[HU_BUS_CHANNEL_LEN];
static char bus_received_id[HU_BUS_ID_LEN];
static char bus_received_msg[HU_BUS_MSG_LEN];

static bool bus_capture_fn(hu_bus_event_type_t type, const hu_bus_event_t *ev, void *ctx) {
    (void)type;
    (void)ctx;
    if (ev) {
        memcpy(bus_received_channel, ev->channel, HU_BUS_CHANNEL_LEN - 1);
        bus_received_channel[HU_BUS_CHANNEL_LEN - 1] = '\0';
        memcpy(bus_received_id, ev->id, HU_BUS_ID_LEN - 1);
        bus_received_id[HU_BUS_ID_LEN - 1] = '\0';
        memcpy(bus_received_msg, ev->message, HU_BUS_MSG_LEN - 1);
        bus_received_msg[HU_BUS_MSG_LEN - 1] = '\0';
    }
    return true;
}

static void test_bus_event_payload_fields(void) {
    bus_received_channel[0] = bus_received_id[0] = bus_received_msg[0] = '\0';
    hu_bus_t bus;
    hu_bus_init(&bus);
    hu_bus_subscribe(&bus, bus_capture_fn, NULL, HU_BUS_EVENT_COUNT);
    hu_bus_publish_simple(&bus, HU_BUS_MESSAGE_SENT, "discord", "user_99", "Hello world");

    HU_ASSERT_TRUE(strstr(bus_received_channel, "discord") != NULL);
    HU_ASSERT_TRUE(strstr(bus_received_id, "user_99") != NULL);
    HU_ASSERT_TRUE(strstr(bus_received_msg, "Hello world") != NULL);
}

static void test_bus_all_event_types(void) {
    bus_received_count = 0;
    hu_bus_t bus;
    hu_bus_init(&bus);
    hu_bus_subscribe(&bus, bus_subscriber_fn, NULL, HU_BUS_EVENT_COUNT);

    hu_bus_publish_simple(&bus, HU_BUS_MESSAGE_RECEIVED, "a", "b", "");
    hu_bus_publish_simple(&bus, HU_BUS_MESSAGE_SENT, "a", "b", "");
    hu_bus_publish_simple(&bus, HU_BUS_TOOL_CALL, "a", "b", "");
    hu_bus_publish_simple(&bus, HU_BUS_ERROR, "a", "b", "");
    hu_bus_publish_simple(&bus, HU_BUS_HEALTH_CHANGE, "a", "b", "");

    HU_ASSERT_EQ(bus_received_count, 5);
}

static int bus_order[8];
static int bus_order_idx;

static bool bus_order_fn(hu_bus_event_type_t type, const hu_bus_event_t *ev, void *ctx) {
    (void)ev;
    (void)ctx;
    bus_order[bus_order_idx++] = (int)type;
    return true;
}

static void test_bus_dispatch_ordering(void) {
    bus_order_idx = 0;
    hu_bus_t bus;
    hu_bus_init(&bus);
    hu_bus_subscribe(&bus, bus_order_fn, NULL, HU_BUS_EVENT_COUNT);
    hu_bus_publish_simple(&bus, HU_BUS_MESSAGE_RECEIVED, "x", "y", "");
    hu_bus_publish_simple(&bus, HU_BUS_TOOL_CALL, "x", "y", "");
    hu_bus_publish_simple(&bus, HU_BUS_ERROR, "x", "y", "");
    HU_ASSERT_EQ(bus_order_idx, 3);
    HU_ASSERT_EQ(bus_order[0], (int)HU_BUS_MESSAGE_RECEIVED);
    HU_ASSERT_EQ(bus_order[1], (int)HU_BUS_TOOL_CALL);
    HU_ASSERT_EQ(bus_order[2], (int)HU_BUS_ERROR);
}

static int bus_priority_count;
static bool bus_high_fn(hu_bus_event_type_t t, const hu_bus_event_t *e, void *c) {
    (void)t;
    (void)e;
    (void)c;
    bus_priority_count++;
    return true;
}

static void test_bus_subscriber_priority_order(void) {
    bus_priority_count = 0;
    hu_bus_t bus;
    hu_bus_init(&bus);
    hu_bus_subscribe(&bus, bus_high_fn, NULL, HU_BUS_MESSAGE_RECEIVED);
    hu_bus_subscribe(&bus, bus_high_fn, (void *)1, HU_BUS_MESSAGE_RECEIVED);
    hu_bus_publish_simple(&bus, HU_BUS_MESSAGE_RECEIVED, "a", "b", "");
    HU_ASSERT_EQ(bus_priority_count, 2);
}

static void test_bus_unsubscribe_nonexistent_safe(void) {
    hu_bus_t bus;
    hu_bus_init(&bus);
    hu_bus_unsubscribe(&bus, bus_subscriber_fn, NULL);
    HU_ASSERT_EQ(bus.count, 0);
}

static void test_bus_publish_zeroed_event(void) {
    bus_received_count = 0;
    hu_bus_t bus;
    hu_bus_init(&bus);
    hu_bus_subscribe(&bus, bus_subscriber_fn, NULL, HU_BUS_EVENT_COUNT);
    hu_bus_event_t ev = {0};
    ev.type = HU_BUS_MESSAGE_SENT;
    hu_bus_publish(&bus, &ev);
    HU_ASSERT_EQ(bus_received_count, 1);
}

void run_bus_tests(void) {
    HU_TEST_SUITE("Bus");
    HU_RUN_TEST(test_bus_init);
    HU_RUN_TEST(test_bus_subscribe_publish);
    HU_RUN_TEST(test_bus_filter);
    HU_RUN_TEST(test_bus_unsubscribe);
    HU_RUN_TEST(test_bus_publish_full_event);
    HU_RUN_TEST(test_bus_multi_subscriber);
    HU_RUN_TEST(test_bus_subscribe_filtered_multiple);
    HU_RUN_TEST(test_bus_event_payload_fields);
    HU_RUN_TEST(test_bus_all_event_types);
    HU_RUN_TEST(test_bus_dispatch_ordering);
    HU_RUN_TEST(test_bus_subscriber_priority_order);
    HU_RUN_TEST(test_bus_unsubscribe_nonexistent_safe);
    HU_RUN_TEST(test_bus_publish_zeroed_event);
}
