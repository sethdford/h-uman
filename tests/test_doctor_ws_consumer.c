/* Tests for src/doctor/ws_consumer.c — T1 pure helpers.
 *
 * Spec: docs/plans/2026-05-27-doctor-ws-consumer/
 *
 * Covers:
 *  - hu_doctor_ws_event_matches_filter — NULL, empty, single, multi,
 *    whitespace-trim, non-match
 *  - hu_doctor_ws_format_event_line — happy path, NULL inputs, deterministic
 *    timestamp via fixed epoch, summary truncation
 *  - hu_doctor_ws_config_default — sane defaults
 *  - hu_doctor_ws_watch — stub returns HU_ERR_NOT_SUPPORTED until T2-T6 ship
 */

#include "human/doctor/ws_consumer.h"
#include "test_framework.h"

#include <string.h>
#include <time.h>

static void test_event_matches_filter_null_matches_all(void) {
    HU_ASSERT_TRUE(hu_doctor_ws_event_matches_filter("chat", NULL));
    HU_ASSERT_TRUE(hu_doctor_ws_event_matches_filter("agent.tool", NULL));
    HU_ASSERT_TRUE(hu_doctor_ws_event_matches_filter("anything", NULL));
}

static void test_event_matches_filter_empty_matches_all(void) {
    HU_ASSERT_TRUE(hu_doctor_ws_event_matches_filter("chat", ""));
}

static void test_event_matches_filter_single_token_match(void) {
    HU_ASSERT_TRUE(hu_doctor_ws_event_matches_filter("chat", "chat"));
}

static void test_event_matches_filter_single_token_no_match(void) {
    HU_ASSERT_TRUE(!hu_doctor_ws_event_matches_filter("error", "chat"));
}

static void test_event_matches_filter_csv_match_first(void) {
    HU_ASSERT_TRUE(hu_doctor_ws_event_matches_filter("chat", "chat,error,health"));
}

static void test_event_matches_filter_csv_match_middle(void) {
    HU_ASSERT_TRUE(hu_doctor_ws_event_matches_filter("error", "chat,error,health"));
}

static void test_event_matches_filter_csv_match_last(void) {
    HU_ASSERT_TRUE(hu_doctor_ws_event_matches_filter("health", "chat,error,health"));
}

static void test_event_matches_filter_csv_no_match(void) {
    HU_ASSERT_TRUE(!hu_doctor_ws_event_matches_filter("cron.job", "chat,error,health"));
}

static void test_event_matches_filter_trims_whitespace(void) {
    HU_ASSERT_TRUE(hu_doctor_ws_event_matches_filter("chat", " chat , error "));
    HU_ASSERT_TRUE(hu_doctor_ws_event_matches_filter("error", " chat , error "));
}

static void test_event_matches_filter_empty_tokens_ignored(void) {
    HU_ASSERT_TRUE(hu_doctor_ws_event_matches_filter("chat", ",,chat,,"));
    HU_ASSERT_TRUE(!hu_doctor_ws_event_matches_filter("foo", ",,chat,,"));
}

static void test_event_matches_filter_null_event_returns_false(void) {
    HU_ASSERT_TRUE(!hu_doctor_ws_event_matches_filter(NULL, "chat"));
}

static void test_event_matches_filter_exact_match_no_prefix(void) {
    /* "chat" filter must NOT match "chat.send" — no glob in v1. */
    HU_ASSERT_TRUE(!hu_doctor_ws_event_matches_filter("chat.send", "chat"));
}

static void test_format_event_line_includes_name_and_seq(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* 2026-05-27T12:34:56 UTC for deterministic timestamp (localtime will
     * still vary by TZ, but seq + name are deterministic). */
    char *line = hu_doctor_ws_format_event_line(&alloc, "chat", "{}", 42, 1748781296);
    HU_ASSERT_NOT_NULL(line);
    HU_ASSERT_TRUE(strstr(line, "chat") != NULL);
    HU_ASSERT_TRUE(strstr(line, "seq=42") != NULL);
    /* Format is "[HH:MM:SS] ... " — starts with '[' and contains a ']'. */
    HU_ASSERT_TRUE(line[0] == '[');
    HU_ASSERT_TRUE(strchr(line, ']') != NULL);
    alloc.free(alloc.ctx, line, strlen(line) + 1);
}

static void test_format_event_line_null_alloc_returns_null(void) {
    HU_ASSERT_TRUE(hu_doctor_ws_format_event_line(NULL, "chat", "{}", 0, 0) == NULL);
}

static void test_format_event_line_null_name_returns_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_TRUE(hu_doctor_ws_format_event_line(&alloc, NULL, "{}", 0, 0) == NULL);
}

static void test_format_event_line_collapses_payload_whitespace(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *line = hu_doctor_ws_format_event_line(
        &alloc, "agent.tool", "{\n  \"name\": \"ping\",\n  \"args\": {}\n}", 7, 1748781296);
    HU_ASSERT_NOT_NULL(line);
    /* Newlines & tabs must be collapsed to single spaces — the line
     * must NOT contain '\n'. */
    HU_ASSERT_TRUE(strchr(line, '\n') == NULL);
    HU_ASSERT_TRUE(strstr(line, "agent.tool") != NULL);
    HU_ASSERT_TRUE(strstr(line, "seq=7") != NULL);
    alloc.free(alloc.ctx, line, strlen(line) + 1);
}

static void test_format_event_line_truncates_long_payload(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* Payload > 80 chars; summary should be truncated with "..." */
    char big_payload[256];
    for (int i = 0; i < 200; i++)
        big_payload[i] = 'x';
    big_payload[200] = '\0';
    char *line = hu_doctor_ws_format_event_line(&alloc, "error", big_payload, 1, 1748781296);
    HU_ASSERT_NOT_NULL(line);
    HU_ASSERT_TRUE(strstr(line, "...") != NULL);
    /* Sanity: shouldn't contain all 200 x's — must have been truncated */
    HU_ASSERT_TRUE(strlen(line) < 200);
    alloc.free(alloc.ctx, line, strlen(line) + 1);
}

static void test_config_default_has_sane_values(void) {
    hu_doctor_ws_config_t c = hu_doctor_ws_config_default();
    HU_ASSERT_TRUE(c.host != NULL);
    HU_ASSERT_TRUE(strcmp(c.host, "127.0.0.1") == 0);
    HU_ASSERT_EQ((int)c.port, 3006);
    HU_ASSERT_TRUE(c.path != NULL);
    HU_ASSERT_TRUE(strcmp(c.path, "/ws") == 0);
    HU_ASSERT_TRUE(c.event_filter == NULL);
    HU_ASSERT_EQ((int)c.max_reconnect_attempts, 3);
    HU_ASSERT_TRUE(c.quiet_stdout == false);
}

static void test_watch_returns_not_supported_until_t3_lands(void) {
    /* T1+T2 ship pure helpers; T3-T6 will implement the socket loop.
     * Until then, hu_doctor_ws_watch returns HU_ERR_NOT_SUPPORTED so
     * callers can detect the gap explicitly rather than hanging on a
     * half-built loop. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_doctor_ws_config_t c = hu_doctor_ws_config_default();
    HU_ASSERT_EQ(hu_doctor_ws_watch(&alloc, &c), HU_ERR_NOT_SUPPORTED);
}

/* ── T2: RFC 6455 handshake helpers ─────────────────────────────────── */

/* RFC 6455 §1.3 worked example: client_key "dGhlIHNhbXBsZSBub25jZQ==" must
 * produce Sec-WebSocket-Accept "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=". This is the
 * canonical reference vector — if our SHA-1 + base64 chain matches this,
 * the implementation is RFC-compliant. */
static void test_compute_accept_key_rfc6455_reference_vector(void) {
    char out[64];
    HU_ASSERT_TRUE(hu_doctor_ws__compute_accept_key("dGhlIHNhbXBsZSBub25jZQ==", out, sizeof(out)));
    HU_ASSERT_STR_EQ(out, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

static void test_compute_accept_key_rejects_small_output_buffer(void) {
    char out[28]; /* 28 chars not enough — need 29 (+NUL) */
    HU_ASSERT_TRUE(!hu_doctor_ws__compute_accept_key("dGhlIHNhbXBsZSBub25jZQ==", out, sizeof(out)));
}

static void test_compute_accept_key_rejects_null_key(void) {
    char out[64];
    HU_ASSERT_TRUE(!hu_doctor_ws__compute_accept_key(NULL, out, sizeof(out)));
}

static void test_generate_client_key_is_deterministic_under_test(void) {
    /* Under HU_IS_TEST the generator uses a fixed 16-byte seed
     * ("test-key-1234567"). Reproducible output lets test fixtures pin
     * the exact handshake bytes. */
    char a[32];
    char b[32];
    HU_ASSERT_TRUE(hu_doctor_ws__generate_client_key(a, sizeof(a)));
    HU_ASSERT_TRUE(hu_doctor_ws__generate_client_key(b, sizeof(b)));
    HU_ASSERT_STR_EQ(a, b);
    /* The base64 of "test-key-1234567" (16 bytes) is exactly 24 chars
     * ending in "==". Pin the exact value so any change to the test
     * seed surfaces. */
    HU_ASSERT_STR_EQ(a, "dGVzdC1rZXktMTIzNDU2Nw==");
}

static void test_generate_client_key_rejects_small_buffer(void) {
    char small[24]; /* 24 chars not enough — need 25 (+NUL) */
    HU_ASSERT_TRUE(!hu_doctor_ws__generate_client_key(small, sizeof(small)));
}

static void test_format_upgrade_request_emits_valid_http(void) {
    char buf[1024];
    size_t n = hu_doctor_ws__format_upgrade_request(buf, sizeof(buf), "127.0.0.1", 3006, "/ws",
                                                    "dGVzdC1rZXktMTIzNDU2Nw==");
    HU_ASSERT_TRUE(n > 0);
    HU_ASSERT_TRUE(strstr(buf, "GET /ws HTTP/1.1\r\n") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "Host: 127.0.0.1:3006\r\n") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "Upgrade: websocket\r\n") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "Connection: Upgrade\r\n") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "Sec-WebSocket-Key: dGVzdC1rZXktMTIzNDU2Nw==\r\n") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "Sec-WebSocket-Version: 13\r\n") != NULL);
    /* Must end with blank line (CRLF CRLF) to terminate the request. */
    HU_ASSERT_TRUE(strstr(buf, "\r\n\r\n") != NULL);
}

static void test_format_upgrade_request_returns_zero_on_overflow(void) {
    char tiny[16];
    size_t n =
        hu_doctor_ws__format_upgrade_request(tiny, sizeof(tiny), "127.0.0.1", 3006, "/ws", "key");
    HU_ASSERT_EQ((int)n, 0);
}

static void test_format_upgrade_request_returns_zero_on_null_inputs(void) {
    char buf[256];
    HU_ASSERT_EQ(
        (int)hu_doctor_ws__format_upgrade_request(buf, sizeof(buf), NULL, 3006, "/ws", "key"), 0);
    HU_ASSERT_EQ(
        (int)hu_doctor_ws__format_upgrade_request(buf, sizeof(buf), "127.0.0.1", 3006, NULL, "key"),
        0);
    HU_ASSERT_EQ(
        (int)hu_doctor_ws__format_upgrade_request(buf, sizeof(buf), "127.0.0.1", 3006, "/ws", NULL),
        0);
}

static void test_verify_handshake_response_accepts_valid(void) {
    const char *resp = "HTTP/1.1 101 Switching Protocols\r\n"
                       "Upgrade: websocket\r\n"
                       "Connection: Upgrade\r\n"
                       "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
                       "\r\n";
    HU_ASSERT_TRUE(hu_doctor_ws__verify_handshake_response(resp, strlen(resp),
                                                           "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
}

static void test_verify_handshake_response_rejects_wrong_status_line(void) {
    const char *resp = "HTTP/1.1 200 OK\r\n"
                       "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
                       "\r\n";
    HU_ASSERT_TRUE(!hu_doctor_ws__verify_handshake_response(resp, strlen(resp),
                                                            "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
}

static void test_verify_handshake_response_rejects_wrong_accept(void) {
    const char *resp = "HTTP/1.1 101 Switching Protocols\r\n"
                       "Sec-WebSocket-Accept: WRONGWRONGWRONGWRONGWRONGWRO=\r\n"
                       "\r\n";
    HU_ASSERT_TRUE(!hu_doctor_ws__verify_handshake_response(resp, strlen(resp),
                                                            "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
}

static void test_verify_handshake_response_rejects_missing_accept(void) {
    const char *resp = "HTTP/1.1 101 Switching Protocols\r\n"
                       "Upgrade: websocket\r\n"
                       "\r\n";
    HU_ASSERT_TRUE(!hu_doctor_ws__verify_handshake_response(resp, strlen(resp),
                                                            "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
}

static void test_verify_handshake_response_case_insensitive_header_name(void) {
    /* RFC 7230 §3.2 — header names are case-insensitive. */
    const char *resp = "HTTP/1.1 101 Switching Protocols\r\n"
                       "SEC-WEBSOCKET-ACCEPT: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
                       "\r\n";
    HU_ASSERT_TRUE(hu_doctor_ws__verify_handshake_response(resp, strlen(resp),
                                                           "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
}

/* End-to-end contract: client generates key → computes expected accept →
 * server (simulated) would echo same accept → verify passes. This is the
 * full handshake roundtrip without sockets. */
static void test_handshake_end_to_end_roundtrip_without_sockets(void) {
    char client_key[32];
    HU_ASSERT_TRUE(hu_doctor_ws__generate_client_key(client_key, sizeof(client_key)));

    char expected_accept[64];
    HU_ASSERT_TRUE(
        hu_doctor_ws__compute_accept_key(client_key, expected_accept, sizeof(expected_accept)));

    /* Simulate the server using the same compute (server-side helpers in
     * ws_server.c use the identical algorithm — pinned by T7 contract
     * test). */
    char server_accept[64];
    HU_ASSERT_TRUE(
        hu_doctor_ws__compute_accept_key(client_key, server_accept, sizeof(server_accept)));
    HU_ASSERT_STR_EQ(expected_accept, server_accept);

    /* Format the server's response as it would arrive on the wire */
    char resp[512];
    int n = snprintf(resp, sizeof(resp),
                     "HTTP/1.1 101 Switching Protocols\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: %s\r\n"
                     "\r\n",
                     server_accept);
    HU_ASSERT_TRUE(n > 0 && (size_t)n < sizeof(resp));

    HU_ASSERT_TRUE(hu_doctor_ws__verify_handshake_response(resp, (size_t)n, expected_accept));
}

void run_doctor_ws_consumer_tests(void) {
    HU_TEST_SUITE("doctor-ws-consumer");
    HU_RUN_TEST(test_event_matches_filter_null_matches_all);
    HU_RUN_TEST(test_event_matches_filter_empty_matches_all);
    HU_RUN_TEST(test_event_matches_filter_single_token_match);
    HU_RUN_TEST(test_event_matches_filter_single_token_no_match);
    HU_RUN_TEST(test_event_matches_filter_csv_match_first);
    HU_RUN_TEST(test_event_matches_filter_csv_match_middle);
    HU_RUN_TEST(test_event_matches_filter_csv_match_last);
    HU_RUN_TEST(test_event_matches_filter_csv_no_match);
    HU_RUN_TEST(test_event_matches_filter_trims_whitespace);
    HU_RUN_TEST(test_event_matches_filter_empty_tokens_ignored);
    HU_RUN_TEST(test_event_matches_filter_null_event_returns_false);
    HU_RUN_TEST(test_event_matches_filter_exact_match_no_prefix);
    HU_RUN_TEST(test_format_event_line_includes_name_and_seq);
    HU_RUN_TEST(test_format_event_line_null_alloc_returns_null);
    HU_RUN_TEST(test_format_event_line_null_name_returns_null);
    HU_RUN_TEST(test_format_event_line_collapses_payload_whitespace);
    HU_RUN_TEST(test_format_event_line_truncates_long_payload);
    HU_RUN_TEST(test_config_default_has_sane_values);
    HU_RUN_TEST(test_watch_returns_not_supported_until_t3_lands);
    /* T2 — handshake helpers */
    HU_RUN_TEST(test_compute_accept_key_rfc6455_reference_vector);
    HU_RUN_TEST(test_compute_accept_key_rejects_small_output_buffer);
    HU_RUN_TEST(test_compute_accept_key_rejects_null_key);
    HU_RUN_TEST(test_generate_client_key_is_deterministic_under_test);
    HU_RUN_TEST(test_generate_client_key_rejects_small_buffer);
    HU_RUN_TEST(test_format_upgrade_request_emits_valid_http);
    HU_RUN_TEST(test_format_upgrade_request_returns_zero_on_overflow);
    HU_RUN_TEST(test_format_upgrade_request_returns_zero_on_null_inputs);
    HU_RUN_TEST(test_verify_handshake_response_accepts_valid);
    HU_RUN_TEST(test_verify_handshake_response_rejects_wrong_status_line);
    HU_RUN_TEST(test_verify_handshake_response_rejects_wrong_accept);
    HU_RUN_TEST(test_verify_handshake_response_rejects_missing_accept);
    HU_RUN_TEST(test_verify_handshake_response_case_insensitive_header_name);
    HU_RUN_TEST(test_handshake_end_to_end_roundtrip_without_sockets);
}
