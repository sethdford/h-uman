/* WebSocket frame encoding/decoding tests */
#include "test_framework.h"
#include "seaclaw/websocket/websocket.h"
#include <string.h>

static void test_ws_build_frame_empty_text(void) {
    char buf[32];
    unsigned char mask[] = {0x01, 0x02, 0x03, 0x04};
    size_t n = sc_ws_build_frame(buf, sizeof(buf), SC_WS_OP_TEXT, "", 0, mask);
    SC_ASSERT_EQ(n, 6u);
    SC_ASSERT_EQ((unsigned char)buf[0], 0x81u);  /* FIN + text */
    SC_ASSERT_EQ((unsigned char)buf[1], 0x80u);  /* MASK + len 0 */
}

static void test_ws_build_frame_short_payload(void) {
    char buf[32];
    unsigned char mask[] = {0xAA, 0xBB, 0xCC, 0xDD};
    const char *payload = "Hi";
    size_t n = sc_ws_build_frame(buf, sizeof(buf), SC_WS_OP_TEXT, payload, 2, mask);
    SC_ASSERT_EQ(n, 8u);
    SC_ASSERT_EQ((unsigned char)buf[0], 0x81u);
    SC_ASSERT_EQ((unsigned char)buf[1], 0x82u);  /* MASK + len 2 */
    SC_ASSERT_EQ((unsigned char)buf[6], (unsigned char)('H' ^ 0xAA));
    SC_ASSERT_EQ((unsigned char)buf[7], (unsigned char)('i' ^ 0xBB));
}

static void test_ws_parse_header_short_text(void) {
    unsigned char bytes[] = {0x81, 0x05};  /* FIN, text, len 5 */
    sc_ws_parsed_header_t h = {0};
    int r = sc_ws_parse_header((const char *)bytes, 2, &h);
    SC_ASSERT_EQ(r, 0);
    SC_ASSERT_TRUE(h.fin);
    SC_ASSERT_EQ(h.opcode, SC_WS_OP_TEXT);
    SC_ASSERT_FALSE(h.masked);
    SC_ASSERT_EQ(h.payload_len, 5ull);
    SC_ASSERT_EQ(h.header_bytes, 2u);
}

static void test_ws_parse_header_126_extended(void) {
    unsigned char bytes[] = {0x82, 0x7E, 0x00, 0x80};  /* binary, len 128 */
    sc_ws_parsed_header_t h = {0};
    int r = sc_ws_parse_header((const char *)bytes, 4, &h);
    SC_ASSERT_EQ(r, 0);
    SC_ASSERT_EQ(h.opcode, SC_WS_OP_BINARY);
    SC_ASSERT_EQ(h.payload_len, 128ull);
    SC_ASSERT_EQ(h.header_bytes, 4u);
}

static void test_ws_parse_header_masked(void) {
    unsigned char bytes[] = {0x81, 0x85, 0xAA, 0xBB, 0xCC, 0xDD};
    sc_ws_parsed_header_t h = {0};
    int r = sc_ws_parse_header((const char *)bytes, 6, &h);
    SC_ASSERT_EQ(r, 0);
    SC_ASSERT_TRUE(h.masked);
    SC_ASSERT_EQ(h.payload_len, 5ull);
    SC_ASSERT_EQ(h.header_bytes, 6u);
}

static void test_ws_apply_mask(void) {
    char payload[] = "Hello";
    unsigned char mask[] = {0x37, 0xFA, 0x21, 0x3D};
    sc_ws_apply_mask(payload, 5, mask);
    SC_ASSERT_EQ((unsigned char)payload[0], (unsigned char)('H' ^ 0x37));
    sc_ws_apply_mask(payload, 5, mask);  /* double mask restores */
    SC_ASSERT_STR_EQ(payload, "Hello");
}

static void test_ws_connect_stub(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_ws_client_t *ws = NULL;
    sc_error_t err = sc_ws_connect(&alloc, "wss://example.com/ws", &ws);
    SC_ASSERT_NEQ(err, SC_OK);
    SC_ASSERT_NULL(ws);
}

static void test_ws_build_frame_binary(void) {
    char buf[64];
    unsigned char mask[] = {0x11, 0x22, 0x33, 0x44};
    const char *payload = "bin";
    size_t n = sc_ws_build_frame(buf, sizeof(buf), SC_WS_OP_BINARY, payload, 3, mask);
    SC_ASSERT_EQ(n, 9u);
    SC_ASSERT_EQ((unsigned char)buf[0], 0x82u);
    SC_ASSERT_EQ((unsigned char)buf[1], 0x83u);
}

static void test_ws_build_frame_close(void) {
    char buf[32];
    unsigned char mask[] = {0, 0, 0, 0};
    size_t n = sc_ws_build_frame(buf, sizeof(buf), SC_WS_OP_CLOSE, "", 0, mask);
    SC_ASSERT_EQ(n, 6u);
    SC_ASSERT_EQ((unsigned char)buf[0], 0x88u);
}

static void test_ws_build_frame_ping(void) {
    char buf[32];
    unsigned char mask[] = {1, 2, 3, 4};
    size_t n = sc_ws_build_frame(buf, sizeof(buf), SC_WS_OP_PING, "x", 1, mask);
    SC_ASSERT_EQ(n, 7u);
    SC_ASSERT_EQ((unsigned char)buf[0], 0x89u);
}

static void test_ws_build_frame_pong(void) {
    char buf[32];
    unsigned char mask[] = {0, 0, 0, 0};
    size_t n = sc_ws_build_frame(buf, sizeof(buf), SC_WS_OP_PONG, "pong", 4, mask);
    SC_ASSERT_EQ(n, 10u);
    SC_ASSERT_EQ((unsigned char)buf[0], 0x8Au);
}

static void test_ws_parse_header_close(void) {
    unsigned char bytes[] = {0x88, 0x00};
    sc_ws_parsed_header_t h = {0};
    int r = sc_ws_parse_header((const char *)bytes, 2, &h);
    SC_ASSERT_EQ(r, 0);
    SC_ASSERT_EQ(h.opcode, SC_WS_OP_CLOSE);
    SC_ASSERT_EQ(h.payload_len, 0ull);
}

static void test_ws_parse_header_insufficient_bytes(void) {
    unsigned char bytes[] = {0x81};
    sc_ws_parsed_header_t h = {0};
    int r = sc_ws_parse_header((const char *)bytes, 1, &h);
    SC_ASSERT_NEQ(r, 0);
}

static void test_ws_parse_header_null_out(void) {
    unsigned char bytes[] = {0x81, 0x05};
    int r = sc_ws_parse_header((const char *)bytes, 2, NULL);
    SC_ASSERT_NEQ(r, 0);
}

static void test_ws_apply_mask_empty(void) {
    char payload[] = "";
    unsigned char mask[] = {1, 2, 3, 4};
    sc_ws_apply_mask(payload, 0, mask);
    SC_ASSERT_EQ(payload[0], '\0');
}

static void test_ws_connect_invalid_host_returns_error(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_ws_client_t *ws = NULL;
    sc_error_t err = sc_ws_connect(&alloc, "ws://invalid-host-that-does-not-resolve.example/ws", &ws);
    SC_ASSERT_NEQ(err, SC_OK);
    SC_ASSERT_NULL(ws);
}

static void test_ws_send_null_client_fails(void) {
    sc_error_t err = sc_ws_send(NULL, "x", 1);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_ws_recv_null_client_fails(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *data = NULL;
    size_t len = 0;
    sc_error_t err = sc_ws_recv(NULL, &alloc, &data, &len);
    SC_ASSERT_NEQ(err, SC_OK);
    SC_ASSERT_NULL(data);
    SC_ASSERT_EQ(len, 0u);
}

static void test_ws_close_null_safe(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_ws_close(NULL, &alloc);
}

static void test_ws_connect_null_args_fails(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_ws_client_t *ws = NULL;
    sc_error_t err = sc_ws_connect(NULL, "ws://example.com/ws", &ws);
    SC_ASSERT_NEQ(err, SC_OK);
    err = sc_ws_connect(&alloc, NULL, &ws);
    SC_ASSERT_NEQ(err, SC_OK);
    err = sc_ws_connect(&alloc, "ws://example.com/ws", NULL);
    SC_ASSERT_NEQ(err, SC_OK);
}

void run_websocket_tests(void) {
    SC_TEST_SUITE("WebSocket");
    SC_RUN_TEST(test_ws_build_frame_empty_text);
    SC_RUN_TEST(test_ws_build_frame_short_payload);
    SC_RUN_TEST(test_ws_build_frame_binary);
    SC_RUN_TEST(test_ws_build_frame_close);
    SC_RUN_TEST(test_ws_build_frame_ping);
    SC_RUN_TEST(test_ws_build_frame_pong);
    SC_RUN_TEST(test_ws_parse_header_short_text);
    SC_RUN_TEST(test_ws_parse_header_126_extended);
    SC_RUN_TEST(test_ws_parse_header_masked);
    SC_RUN_TEST(test_ws_parse_header_close);
    SC_RUN_TEST(test_ws_parse_header_insufficient_bytes);
    SC_RUN_TEST(test_ws_parse_header_null_out);
    SC_RUN_TEST(test_ws_apply_mask);
    SC_RUN_TEST(test_ws_apply_mask_empty);
    SC_RUN_TEST(test_ws_connect_stub);
    SC_RUN_TEST(test_ws_connect_invalid_host_returns_error);
    SC_RUN_TEST(test_ws_send_null_client_fails);
    SC_RUN_TEST(test_ws_recv_null_client_fails);
    SC_RUN_TEST(test_ws_close_null_safe);
    SC_RUN_TEST(test_ws_connect_null_args_fails);
}
