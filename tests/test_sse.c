/* SSE client tests */
#include "human/core/allocator.h"
#include "human/sse/sse_client.h"
#include "test_framework.h"
#include <string.h>

static int sse_cb_count;
static char sse_last_event_type[64];
static char sse_last_data[256];

static void sse_callback(void *ctx, const hu_sse_event_t *event) {
    (void)ctx;
    sse_cb_count++;
    if (event->event_type) {
        size_t n = event->event_type_len < sizeof(sse_last_event_type) - 1
                       ? event->event_type_len
                       : sizeof(sse_last_event_type) - 1;
        memcpy(sse_last_event_type, event->event_type, n);
        sse_last_event_type[n] = '\0';
    } else {
        sse_last_event_type[0] = '\0';
    }
    if (event->data) {
        size_t n = event->data_len < sizeof(sse_last_data) - 1 ? event->data_len
                                                               : sizeof(sse_last_data) - 1;
        memcpy(sse_last_data, event->data, n);
        sse_last_data[n] = '\0';
    } else {
        sse_last_data[0] = '\0';
    }
}

static void test_sse_connect_mock(void) {
#if HU_IS_TEST
    sse_cb_count = 0;
    sse_last_event_type[0] = '\0';
    sse_last_data[0] = '\0';

    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err =
        hu_sse_connect(&alloc, "https://example.com/sse", NULL, NULL, sse_callback, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(sse_cb_count, 1);
    HU_ASSERT_STR_EQ(sse_last_event_type, "message");
    HU_ASSERT_TRUE(strstr(sse_last_data, "sse_connect") != NULL);
#endif
}

static void test_sse_connect_null_alloc(void) {
    hu_error_t err = hu_sse_connect(NULL, "https://x.com", NULL, NULL, sse_callback, NULL);
    HU_ASSERT_NEQ(err, HU_OK);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_sse_connect_null_url(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_sse_connect(&alloc, NULL, NULL, NULL, sse_callback, NULL);
    HU_ASSERT_NEQ(err, HU_OK);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_sse_connect_null_callback(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_sse_connect(&alloc, "https://x.com", NULL, NULL, NULL, NULL);
    HU_ASSERT_NEQ(err, HU_OK);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_sse_connect_mock_event_structure(void) {
#if HU_IS_TEST
    sse_cb_count = 0;
    sse_last_event_type[0] = '\0';
    sse_last_data[0] = '\0';

    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err =
        hu_sse_connect(&alloc, "https://example.com/sse", NULL, NULL, sse_callback, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(sse_cb_count, 1);
    HU_ASSERT_STR_EQ(sse_last_event_type, "message");
    HU_ASSERT_TRUE(strstr(sse_last_data, "sse_connect") != NULL);
    HU_ASSERT_TRUE(strlen(sse_last_event_type) == 7);
    HU_ASSERT_TRUE(strlen(sse_last_data) > 0);
#endif
}

void run_sse_tests(void) {
    HU_TEST_SUITE("SSE client");
    HU_RUN_TEST(test_sse_connect_mock);
    HU_RUN_TEST(test_sse_connect_null_alloc);
    HU_RUN_TEST(test_sse_connect_null_url);
    HU_RUN_TEST(test_sse_connect_null_callback);
    HU_RUN_TEST(test_sse_connect_mock_event_structure);
}
