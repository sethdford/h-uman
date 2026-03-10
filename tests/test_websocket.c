/* WebSocket frame encoding/decoding tests */
#include "human/websocket/websocket.h"
#include "test_framework.h"
#include <string.h>

static void test_ws_build_frame_empty_text(void) {
    char buf[32];
    unsigned char mask[] = {0x01, 0x02, 0x03, 0x04};
    size_t n = hu_ws_build_frame(buf, sizeof(buf), HU_WS_OP_TEXT, "", 0, mask);
    HU_ASSERT_EQ(n, 6u);
    HU_ASSERT_EQ((unsigned char)buf[0], 0x81u); /* FIN + text */
    HU_ASSERT_EQ((unsigned char)buf[1], 0x80u); /* MASK + len 0 */
}

static void test_ws_build_frame_short_payload(void) {
    char buf[32];
    unsigned char mask[] = {0xAA, 0xBB, 0xCC, 0xDD};
    const char *payload = "Hi";
    size_t n = hu_ws_build_frame(buf, sizeof(buf), HU_WS_OP_TEXT, payload, 2, mask);
    HU_ASSERT_EQ(n, 8u);
    HU_ASSERT_EQ((unsigned char)buf[0], 0x81u);
    HU_ASSERT_EQ((unsigned char)buf[1], 0x82u); /* MASK + len 2 */
    HU_ASSERT_EQ((unsigned char)buf[6], (unsigned char)('H' ^ 0xAA));
    HU_ASSERT_EQ((unsigned char)buf[7], (unsigned char)('i' ^ 0xBB));
}

static void test_ws_parse_header_short_text(void) {
    unsigned char bytes[] = {0x81, 0x05}; /* FIN, text, len 5 */
    hu_ws_parsed_header_t h = {0};
    int r = hu_ws_parse_header((const char *)bytes, 2, &h);
    HU_ASSERT_EQ(r, 0);
    HU_ASSERT_TRUE(h.fin);
    HU_ASSERT_EQ(h.opcode, HU_WS_OP_TEXT);
    HU_ASSERT_FALSE(h.masked);
    HU_ASSERT_EQ(h.payload_len, 5ull);
    HU_ASSERT_EQ(h.header_bytes, 2u);
}

static void test_ws_parse_header_126_extended(void) {
    unsigned char bytes[] = {0x82, 0x7E, 0x00, 0x80}; /* binary, len 128 */
    hu_ws_parsed_header_t h = {0};
    int r = hu_ws_parse_header((const char *)bytes, 4, &h);
    HU_ASSERT_EQ(r, 0);
    HU_ASSERT_EQ(h.opcode, HU_WS_OP_BINARY);
    HU_ASSERT_EQ(h.payload_len, 128ull);
    HU_ASSERT_EQ(h.header_bytes, 4u);
}

static void test_ws_parse_header_masked(void) {
    unsigned char bytes[] = {0x81, 0x85, 0xAA, 0xBB, 0xCC, 0xDD};
    hu_ws_parsed_header_t h = {0};
    int r = hu_ws_parse_header((const char *)bytes, 6, &h);
    HU_ASSERT_EQ(r, 0);
    HU_ASSERT_TRUE(h.masked);
    HU_ASSERT_EQ(h.payload_len, 5ull);
    HU_ASSERT_EQ(h.header_bytes, 6u);
}

static void test_ws_apply_mask(void) {
    char payload[] = "Hello";
    unsigned char mask[] = {0x37, 0xFA, 0x21, 0x3D};
    hu_ws_apply_mask(payload, 5, mask);
    HU_ASSERT_EQ((unsigned char)payload[0], (unsigned char)('H' ^ 0x37));
    hu_ws_apply_mask(payload, 5, mask); /* double mask restores */
    HU_ASSERT_STR_EQ(payload, "Hello");
}

static void test_ws_connect_stub(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_ws_client_t *ws = NULL;
    hu_error_t err = hu_ws_connect(&alloc, "wss://example.com/ws", &ws);
    HU_ASSERT_NEQ(err, HU_OK);
    HU_ASSERT_NULL(ws);
}

static void test_ws_build_frame_binary(void) {
    char buf[64];
    unsigned char mask[] = {0x11, 0x22, 0x33, 0x44};
    const char *payload = "bin";
    size_t n = hu_ws_build_frame(buf, sizeof(buf), HU_WS_OP_BINARY, payload, 3, mask);
    HU_ASSERT_EQ(n, 9u);
    HU_ASSERT_EQ((unsigned char)buf[0], 0x82u);
    HU_ASSERT_EQ((unsigned char)buf[1], 0x83u);
}

static void test_ws_build_frame_close(void) {
    char buf[32];
    unsigned char mask[] = {0, 0, 0, 0};
    size_t n = hu_ws_build_frame(buf, sizeof(buf), HU_WS_OP_CLOSE, "", 0, mask);
    HU_ASSERT_EQ(n, 6u);
    HU_ASSERT_EQ((unsigned char)buf[0], 0x88u);
}

static void test_ws_build_frame_ping(void) {
    char buf[32];
    unsigned char mask[] = {1, 2, 3, 4};
    size_t n = hu_ws_build_frame(buf, sizeof(buf), HU_WS_OP_PING, "x", 1, mask);
    HU_ASSERT_EQ(n, 7u);
    HU_ASSERT_EQ((unsigned char)buf[0], 0x89u);
}

static void test_ws_build_frame_pong(void) {
    char buf[32];
    unsigned char mask[] = {0, 0, 0, 0};
    size_t n = hu_ws_build_frame(buf, sizeof(buf), HU_WS_OP_PONG, "pong", 4, mask);
    HU_ASSERT_EQ(n, 10u);
    HU_ASSERT_EQ((unsigned char)buf[0], 0x8Au);
}

static void test_ws_parse_header_close(void) {
    unsigned char bytes[] = {0x88, 0x00};
    hu_ws_parsed_header_t h = {0};
    int r = hu_ws_parse_header((const char *)bytes, 2, &h);
    HU_ASSERT_EQ(r, 0);
    HU_ASSERT_EQ(h.opcode, HU_WS_OP_CLOSE);
    HU_ASSERT_EQ(h.payload_len, 0ull);
}

static void test_ws_parse_header_insufficient_bytes(void) {
    unsigned char bytes[] = {0x81};
    hu_ws_parsed_header_t h = {0};
    int r = hu_ws_parse_header((const char *)bytes, 1, &h);
    HU_ASSERT_NEQ(r, 0);
}

static void test_ws_parse_header_null_out(void) {
    unsigned char bytes[] = {0x81, 0x05};
    int r = hu_ws_parse_header((const char *)bytes, 2, NULL);
    HU_ASSERT_NEQ(r, 0);
}

static void test_ws_build_frame_buf_too_small_returns_zero(void) {
    char buf[5];
    unsigned char mask[] = {0, 0, 0, 0};
    size_t n = hu_ws_build_frame(buf, sizeof(buf), HU_WS_OP_TEXT, "", 0, mask);
    HU_ASSERT_EQ(n, 0u);
}

static void test_ws_build_frame_buf_insufficient_for_payload_returns_zero(void) {
    char buf[7];
    unsigned char mask[] = {0, 0, 0, 0};
    const char *payload = "Hi";
    size_t n = hu_ws_build_frame(buf, sizeof(buf), HU_WS_OP_TEXT, payload, 2, mask);
    HU_ASSERT_EQ(n, 0u);
}

static void test_ws_parse_header_bytes_len_zero_returns_error(void) {
    unsigned char bytes[] = {0x81, 0x05};
    hu_ws_parsed_header_t h = {0};
    int r = hu_ws_parse_header((const char *)bytes, 0, &h);
    HU_ASSERT_EQ(r, -1);
}

static void test_ws_parse_header_null_out_returns_negative_one(void) {
    unsigned char bytes[] = {0x81, 0x05};
    int r = hu_ws_parse_header((const char *)bytes, 2, NULL);
    HU_ASSERT_EQ(r, -1);
}

static void test_ws_parse_header_bytes_len_one_returns_error(void) {
    unsigned char bytes[] = {0x81};
    hu_ws_parsed_header_t h = {0};
    int r = hu_ws_parse_header((const char *)bytes, 1, &h);
    HU_ASSERT_EQ(r, -1);
}

static void test_ws_build_frame_payload_125_bytes_verify_size(void) {
    char buf[256];
    unsigned char mask[] = {0x11, 0x22, 0x33, 0x44};
    char payload[125];
    for (size_t i = 0; i < 125; i++)
        payload[i] = (char)(i & 0xFF);
    size_t n = hu_ws_build_frame(buf, sizeof(buf), HU_WS_OP_TEXT, payload, 125, mask);
    HU_ASSERT_EQ(n, 131u); /* 2 header + 4 mask + 125 payload */
}

static void test_ws_apply_mask_roundtrip_restores_data(void) {
    char data[] = "roundtrip test data";
    size_t len = strlen(data);
    unsigned char mask[] = {0xAB, 0xCD, 0xEF, 0x12};
    hu_ws_apply_mask(data, len, mask);
    HU_ASSERT(strcmp(data, "roundtrip test data") != 0);
    hu_ws_apply_mask(data, len, mask);
    HU_ASSERT_STR_EQ(data, "roundtrip test data");
}

static void test_ws_apply_mask_empty(void) {
    char payload[] = "";
    unsigned char mask[] = {1, 2, 3, 4};
    hu_ws_apply_mask(payload, 0, mask);
    HU_ASSERT_EQ(payload[0], '\0');
}

static void test_ws_connect_invalid_host_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_ws_client_t *ws = NULL;
    hu_error_t err =
        hu_ws_connect(&alloc, "ws://invalid-host-that-does-not-resolve.example/ws", &ws);
    HU_ASSERT_NEQ(err, HU_OK);
    HU_ASSERT_NULL(ws);
}

static void test_ws_send_null_client_fails(void) {
    hu_error_t err = hu_ws_send(NULL, "x", 1);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_ws_recv_null_client_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *data = NULL;
    size_t len = 0;
    hu_error_t err = hu_ws_recv(NULL, &alloc, &data, &len);
    HU_ASSERT_NEQ(err, HU_OK);
    HU_ASSERT_NULL(data);
    HU_ASSERT_EQ(len, 0u);
}

static void test_ws_close_null_safe(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_ws_close(NULL, &alloc);
}

static void test_ws_connect_null_args_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_ws_client_t *ws = NULL;
    hu_error_t err = hu_ws_connect(NULL, "ws://example.com/ws", &ws);
    HU_ASSERT_NEQ(err, HU_OK);
    err = hu_ws_connect(&alloc, NULL, &ws);
    HU_ASSERT_NEQ(err, HU_OK);
    err = hu_ws_connect(&alloc, "ws://example.com/ws", NULL);
    HU_ASSERT_NEQ(err, HU_OK);
}

void run_websocket_tests(void) {
    HU_TEST_SUITE("WebSocket");
    HU_RUN_TEST(test_ws_build_frame_empty_text);
    HU_RUN_TEST(test_ws_build_frame_short_payload);
    HU_RUN_TEST(test_ws_build_frame_binary);
    HU_RUN_TEST(test_ws_build_frame_close);
    HU_RUN_TEST(test_ws_build_frame_ping);
    HU_RUN_TEST(test_ws_build_frame_pong);
    HU_RUN_TEST(test_ws_parse_header_short_text);
    HU_RUN_TEST(test_ws_parse_header_126_extended);
    HU_RUN_TEST(test_ws_parse_header_masked);
    HU_RUN_TEST(test_ws_parse_header_close);
    HU_RUN_TEST(test_ws_parse_header_insufficient_bytes);
    HU_RUN_TEST(test_ws_parse_header_null_out);
    HU_RUN_TEST(test_ws_build_frame_buf_too_small_returns_zero);
    HU_RUN_TEST(test_ws_build_frame_buf_insufficient_for_payload_returns_zero);
    HU_RUN_TEST(test_ws_parse_header_bytes_len_zero_returns_error);
    HU_RUN_TEST(test_ws_parse_header_null_out_returns_negative_one);
    HU_RUN_TEST(test_ws_parse_header_bytes_len_one_returns_error);
    HU_RUN_TEST(test_ws_build_frame_payload_125_bytes_verify_size);
    HU_RUN_TEST(test_ws_apply_mask_roundtrip_restores_data);
    HU_RUN_TEST(test_ws_apply_mask);
    HU_RUN_TEST(test_ws_apply_mask_empty);
    HU_RUN_TEST(test_ws_connect_stub);
    HU_RUN_TEST(test_ws_connect_invalid_host_returns_error);
    HU_RUN_TEST(test_ws_send_null_client_fails);
    HU_RUN_TEST(test_ws_recv_null_client_fails);
    HU_RUN_TEST(test_ws_close_null_safe);
    HU_RUN_TEST(test_ws_connect_null_args_fails);
}
