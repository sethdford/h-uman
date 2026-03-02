/* Event bus pub/sub tests */
#include "test_framework.h"
#include "seaclaw/bus.h"
#include "seaclaw/core/allocator.h"
#include <string.h>

static int bus_received_count;
static sc_bus_event_type_t bus_last_type;

static bool bus_subscriber_fn(sc_bus_event_type_t type,
    const sc_bus_event_t *ev, void *user_ctx)
{
    (void)user_ctx;
    (void)ev;
    bus_received_count++;
    bus_last_type = type;
    return true; /* stay subscribed */
}

static void test_bus_init(void) {
    sc_bus_t bus;
    sc_bus_init(&bus);
    SC_ASSERT_EQ(bus.count, 0);
}

static void test_bus_subscribe_publish(void) {
    sc_bus_t bus;
    sc_bus_init(&bus);
    bus_received_count = 0;

    sc_error_t err = sc_bus_subscribe(&bus, bus_subscriber_fn, NULL, SC_BUS_EVENT_COUNT);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(bus.count, 1);

    sc_bus_event_t ev = {0};
    ev.type = SC_BUS_MESSAGE_RECEIVED;
    sc_bus_publish(&bus, &ev);
    SC_ASSERT_EQ(bus_received_count, 1);
    SC_ASSERT_EQ(bus_last_type, SC_BUS_MESSAGE_RECEIVED);

    sc_bus_publish_simple(&bus, SC_BUS_TOOL_CALL, "cli", "sess1", "run shell");
    SC_ASSERT_EQ(bus_received_count, 2);
    SC_ASSERT_EQ(bus_last_type, SC_BUS_TOOL_CALL);
}

static void test_bus_filter(void) {
    sc_bus_t bus;
    sc_bus_init(&bus);
    bus_received_count = 0;

    /* Subscribe only to MESSAGE_SENT */
    sc_bus_subscribe(&bus, bus_subscriber_fn, NULL, SC_BUS_MESSAGE_SENT);
    sc_bus_publish_simple(&bus, SC_BUS_MESSAGE_RECEIVED, "x", "y", "");
    SC_ASSERT_EQ(bus_received_count, 0);
    sc_bus_publish_simple(&bus, SC_BUS_MESSAGE_SENT, "x", "y", "");
    SC_ASSERT_EQ(bus_received_count, 1);
}

static void test_bus_unsubscribe(void) {
    sc_bus_t bus;
    sc_bus_init(&bus);
    bus_received_count = 0;
    sc_bus_subscribe(&bus, bus_subscriber_fn, NULL, SC_BUS_EVENT_COUNT);
    sc_bus_unsubscribe(&bus, bus_subscriber_fn, NULL);
    SC_ASSERT_EQ(bus.count, 0);
    sc_bus_publish_simple(&bus, SC_BUS_ERROR, "a", "b", "err");
    SC_ASSERT_EQ(bus_received_count, 0);
}

static void test_bus_publish_full_event(void) {
    sc_bus_t bus;
    sc_bus_init(&bus);
    bus_received_count = 0;
    bus_last_type = SC_BUS_EVENT_COUNT;
    sc_bus_subscribe(&bus, bus_subscriber_fn, NULL, SC_BUS_EVENT_COUNT);

    sc_bus_event_t ev = {0};
    ev.type = SC_BUS_HEALTH_CHANGE;
    memcpy(ev.channel, "telegram", 8);
    memcpy(ev.id, "sess_xyz", 8);
    memcpy(ev.message, "online", 6);
    sc_bus_publish(&bus, &ev);

    SC_ASSERT_EQ(bus_received_count, 1);
    SC_ASSERT_EQ(bus_last_type, SC_BUS_HEALTH_CHANGE);
}

static int bus_sub1_count;
static int bus_sub2_count;

static bool bus_sub1_fn(sc_bus_event_type_t type, const sc_bus_event_t *ev, void *ctx) {
    (void)type; (void)ev; (void)ctx;
    bus_sub1_count++;
    return true;
}

static bool bus_sub2_fn(sc_bus_event_type_t type, const sc_bus_event_t *ev, void *ctx) {
    (void)type; (void)ev; (void)ctx;
    bus_sub2_count++;
    return true;
}

static void test_bus_multi_subscriber(void) {
    bus_sub1_count = 0;
    bus_sub2_count = 0;

    sc_bus_t bus;
    sc_bus_init(&bus);
    sc_bus_subscribe(&bus, bus_sub1_fn, NULL, SC_BUS_EVENT_COUNT);
    sc_bus_subscribe(&bus, bus_sub2_fn, NULL, SC_BUS_EVENT_COUNT);

    sc_bus_publish_simple(&bus, SC_BUS_MESSAGE_RECEIVED, "ch", "id", "msg");
    SC_ASSERT_EQ(bus_sub1_count, 1);
    SC_ASSERT_EQ(bus_sub2_count, 1);

    sc_bus_publish_simple(&bus, SC_BUS_TOOL_CALL, "cli", "s1", "run");
    SC_ASSERT_EQ(bus_sub1_count, 2);
    SC_ASSERT_EQ(bus_sub2_count, 2);
}

static bool bus_only_tool_fn(sc_bus_event_type_t type, const sc_bus_event_t *ev, void *ctx) {
    (void)ev; (void)ctx;
    if (type == SC_BUS_TOOL_CALL) bus_received_count++;
    return true;
}

static void test_bus_subscribe_filtered_multiple(void) {
    bus_received_count = 0;
    sc_bus_t bus;
    sc_bus_init(&bus);
    sc_bus_subscribe(&bus, bus_only_tool_fn, NULL, SC_BUS_TOOL_CALL);
    sc_bus_publish_simple(&bus, SC_BUS_MESSAGE_RECEIVED, "x", "y", "");
    sc_bus_publish_simple(&bus, SC_BUS_TOOL_CALL, "x", "y", "shell");
    sc_bus_publish_simple(&bus, SC_BUS_ERROR, "x", "y", "");

    SC_ASSERT_EQ(bus_received_count, 1);
}

static char bus_received_channel[SC_BUS_CHANNEL_LEN];
static char bus_received_id[SC_BUS_ID_LEN];
static char bus_received_msg[SC_BUS_MSG_LEN];

static bool bus_capture_fn(sc_bus_event_type_t type, const sc_bus_event_t *ev, void *ctx) {
    (void)type; (void)ctx;
    if (ev) {
        memcpy(bus_received_channel, ev->channel, SC_BUS_CHANNEL_LEN - 1);
        bus_received_channel[SC_BUS_CHANNEL_LEN - 1] = '\0';
        memcpy(bus_received_id, ev->id, SC_BUS_ID_LEN - 1);
        bus_received_id[SC_BUS_ID_LEN - 1] = '\0';
        memcpy(bus_received_msg, ev->message, SC_BUS_MSG_LEN - 1);
        bus_received_msg[SC_BUS_MSG_LEN - 1] = '\0';
    }
    return true;
}

static void test_bus_event_payload_fields(void) {
    bus_received_channel[0] = bus_received_id[0] = bus_received_msg[0] = '\0';
    sc_bus_t bus;
    sc_bus_init(&bus);
    sc_bus_subscribe(&bus, bus_capture_fn, NULL, SC_BUS_EVENT_COUNT);
    sc_bus_publish_simple(&bus, SC_BUS_MESSAGE_SENT, "discord", "user_99", "Hello world");

    SC_ASSERT_TRUE(strstr(bus_received_channel, "discord") != NULL);
    SC_ASSERT_TRUE(strstr(bus_received_id, "user_99") != NULL);
    SC_ASSERT_TRUE(strstr(bus_received_msg, "Hello world") != NULL);
}

static void test_bus_all_event_types(void) {
    bus_received_count = 0;
    sc_bus_t bus;
    sc_bus_init(&bus);
    sc_bus_subscribe(&bus, bus_subscriber_fn, NULL, SC_BUS_EVENT_COUNT);

    sc_bus_publish_simple(&bus, SC_BUS_MESSAGE_RECEIVED, "a", "b", "");
    sc_bus_publish_simple(&bus, SC_BUS_MESSAGE_SENT, "a", "b", "");
    sc_bus_publish_simple(&bus, SC_BUS_TOOL_CALL, "a", "b", "");
    sc_bus_publish_simple(&bus, SC_BUS_ERROR, "a", "b", "");
    sc_bus_publish_simple(&bus, SC_BUS_HEALTH_CHANGE, "a", "b", "");

    SC_ASSERT_EQ(bus_received_count, 5);
}

static int bus_order[8];
static int bus_order_idx;

static bool bus_order_fn(sc_bus_event_type_t type, const sc_bus_event_t *ev, void *ctx) {
    (void)ev; (void)ctx;
    bus_order[bus_order_idx++] = (int)type;
    return true;
}

static void test_bus_dispatch_ordering(void) {
    bus_order_idx = 0;
    sc_bus_t bus;
    sc_bus_init(&bus);
    sc_bus_subscribe(&bus, bus_order_fn, NULL, SC_BUS_EVENT_COUNT);
    sc_bus_publish_simple(&bus, SC_BUS_MESSAGE_RECEIVED, "x", "y", "");
    sc_bus_publish_simple(&bus, SC_BUS_TOOL_CALL, "x", "y", "");
    sc_bus_publish_simple(&bus, SC_BUS_ERROR, "x", "y", "");
    SC_ASSERT_EQ(bus_order_idx, 3);
    SC_ASSERT_EQ(bus_order[0], (int)SC_BUS_MESSAGE_RECEIVED);
    SC_ASSERT_EQ(bus_order[1], (int)SC_BUS_TOOL_CALL);
    SC_ASSERT_EQ(bus_order[2], (int)SC_BUS_ERROR);
}

static int bus_priority_count;
static bool bus_high_fn(sc_bus_event_type_t t, const sc_bus_event_t *e, void *c) {
    (void)t;(void)e;(void)c; bus_priority_count++; return true;
}

static void test_bus_subscriber_priority_order(void) {
    bus_priority_count = 0;
    sc_bus_t bus;
    sc_bus_init(&bus);
    sc_bus_subscribe(&bus, bus_high_fn, NULL, SC_BUS_MESSAGE_RECEIVED);
    sc_bus_subscribe(&bus, bus_high_fn, (void *)1, SC_BUS_MESSAGE_RECEIVED);
    sc_bus_publish_simple(&bus, SC_BUS_MESSAGE_RECEIVED, "a", "b", "");
    SC_ASSERT_EQ(bus_priority_count, 2);
}

static void test_bus_unsubscribe_nonexistent_safe(void) {
    sc_bus_t bus;
    sc_bus_init(&bus);
    sc_bus_unsubscribe(&bus, bus_subscriber_fn, NULL);
    SC_ASSERT_EQ(bus.count, 0);
}

static void test_bus_publish_zeroed_event(void) {
    bus_received_count = 0;
    sc_bus_t bus;
    sc_bus_init(&bus);
    sc_bus_subscribe(&bus, bus_subscriber_fn, NULL, SC_BUS_EVENT_COUNT);
    sc_bus_event_t ev = {0};
    ev.type = SC_BUS_MESSAGE_SENT;
    sc_bus_publish(&bus, &ev);
    SC_ASSERT_EQ(bus_received_count, 1);
}

void run_bus_tests(void) {
    SC_TEST_SUITE("Bus");
    SC_RUN_TEST(test_bus_init);
    SC_RUN_TEST(test_bus_subscribe_publish);
    SC_RUN_TEST(test_bus_filter);
    SC_RUN_TEST(test_bus_unsubscribe);
    SC_RUN_TEST(test_bus_publish_full_event);
    SC_RUN_TEST(test_bus_multi_subscriber);
    SC_RUN_TEST(test_bus_subscribe_filtered_multiple);
    SC_RUN_TEST(test_bus_event_payload_fields);
    SC_RUN_TEST(test_bus_all_event_types);
    SC_RUN_TEST(test_bus_dispatch_ordering);
    SC_RUN_TEST(test_bus_subscriber_priority_order);
    SC_RUN_TEST(test_bus_unsubscribe_nonexistent_safe);
    SC_RUN_TEST(test_bus_publish_zeroed_event);
}
