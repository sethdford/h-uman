/* SSE client tests */
#include "test_framework.h"
#include "seaclaw/sse/sse_client.h"
#include "seaclaw/core/allocator.h"
#include <string.h>

static int sse_cb_count;
static char sse_last_event_type[64];
static char sse_last_data[256];

static void sse_callback(void *ctx, const sc_sse_event_t *event) {
    (void)ctx;
    sse_cb_count++;
    if (event->event_type) {
        size_t n = event->event_type_len < sizeof(sse_last_event_type) - 1 ?
            event->event_type_len : sizeof(sse_last_event_type) - 1;
        memcpy(sse_last_event_type, event->event_type, n);
        sse_last_event_type[n] = '\0';
    } else {
        sse_last_event_type[0] = '\0';
    }
    if (event->data) {
        size_t n = event->data_len < sizeof(sse_last_data) - 1 ?
            event->data_len : sizeof(sse_last_data) - 1;
        memcpy(sse_last_data, event->data, n);
        sse_last_data[n] = '\0';
    } else {
        sse_last_data[0] = '\0';
    }
}

static void test_sse_connect_mock(void) {
#if SC_IS_TEST
    sse_cb_count = 0;
    sse_last_event_type[0] = '\0';
    sse_last_data[0] = '\0';

    sc_allocator_t alloc = sc_system_allocator();
    sc_error_t err = sc_sse_connect(&alloc, "https://example.com/sse",
        NULL, NULL, sse_callback, NULL);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(sse_cb_count, 1);
    SC_ASSERT_STR_EQ(sse_last_event_type, "message");
    SC_ASSERT_TRUE(strstr(sse_last_data, "sse_connect") != NULL);
#endif
}

static void test_sse_connect_null_args(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_error_t err = sc_sse_connect(NULL, "https://x.com", NULL, NULL, sse_callback, NULL);
    SC_ASSERT_NEQ(err, SC_OK);
    err = sc_sse_connect(&alloc, "https://x.com", NULL, NULL, NULL, NULL);
    SC_ASSERT_NEQ(err, SC_OK);
}

void run_sse_tests(void) {
    SC_TEST_SUITE("SSE client");
    SC_RUN_TEST(test_sse_connect_mock);
    SC_RUN_TEST(test_sse_connect_null_args);
}
