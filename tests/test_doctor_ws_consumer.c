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

/* ── T3: RFC 6455 frame parser ──────────────────────────────────────── */

/* Server-to-client text frame, payload "Hello" (5 bytes, unmasked):
 *   0x81 = FIN=1 + opcode=TEXT
 *   0x05 = MASK=0 + len=5
 *   "Hello"
 */
static void test_parse_frame_text_5_byte_payload(void) {
    const uint8_t frame[] = {0x81, 0x05, 'H', 'e', 'l', 'l', 'o'};
    hu_doctor_ws_opcode_t op;
    const uint8_t *payload;
    size_t payload_len, consumed;
    HU_ASSERT_EQ(
        hu_doctor_ws__parse_frame(frame, sizeof(frame), &op, &payload, &payload_len, &consumed),
        HU_OK);
    HU_ASSERT_EQ((int)op, (int)HU_DOCTOR_WS_OP_TEXT);
    HU_ASSERT_EQ((int)payload_len, 5);
    HU_ASSERT_EQ((int)consumed, 7);
    HU_ASSERT_TRUE(payload != NULL);
    HU_ASSERT_TRUE(memcmp(payload, "Hello", 5) == 0);
}

/* Extended length 126 (16-bit len). Payload = 200 bytes of 'x'.
 *   0x81 0x7E 0x00 0xC8 + 200 bytes. Total = 204 bytes. */
static void test_parse_frame_extended_16bit_length(void) {
    uint8_t frame[204];
    frame[0] = 0x81;
    frame[1] = 0x7E; /* 126 */
    frame[2] = 0x00;
    frame[3] = 0xC8; /* 200 */
    memset(frame + 4, 'x', 200);
    hu_doctor_ws_opcode_t op;
    const uint8_t *payload;
    size_t payload_len, consumed;
    HU_ASSERT_EQ(
        hu_doctor_ws__parse_frame(frame, sizeof(frame), &op, &payload, &payload_len, &consumed),
        HU_OK);
    HU_ASSERT_EQ((int)op, (int)HU_DOCTOR_WS_OP_TEXT);
    HU_ASSERT_EQ((int)payload_len, 200);
    HU_ASSERT_EQ((int)consumed, 204);
}

/* Incomplete header: only 1 byte received. Parser returns HU_OK with
 * consumed=0, signalling caller to read more. */
static void test_parse_frame_incomplete_returns_ok_with_zero_consumed(void) {
    const uint8_t frame[] = {0x81};
    hu_doctor_ws_opcode_t op = (hu_doctor_ws_opcode_t)0xFF;
    const uint8_t *payload = (const uint8_t *)0xDEAD;
    size_t payload_len = 999, consumed = 999;
    HU_ASSERT_EQ(hu_doctor_ws__parse_frame(frame, 1, &op, &payload, &payload_len, &consumed),
                 HU_OK);
    HU_ASSERT_EQ((int)consumed, 0);
    HU_ASSERT_EQ((int)payload_len, 0);
    HU_ASSERT_TRUE(payload == NULL);
}

/* Incomplete payload: header says 100 bytes but only 50 in buffer. */
static void test_parse_frame_incomplete_payload(void) {
    uint8_t frame[52];
    frame[0] = 0x81;
    frame[1] = 100;
    memset(frame + 2, 'x', 50);
    size_t consumed = 999;
    HU_ASSERT_EQ(hu_doctor_ws__parse_frame(frame, sizeof(frame), NULL, NULL, NULL, &consumed),
                 HU_OK);
    HU_ASSERT_EQ((int)consumed, 0);
}

/* Server-to-client frame MUST NOT be masked. If it is, reject. */
static void test_parse_frame_rejects_masked_inbound(void) {
    const uint8_t frame[] = {0x81,       0x85,       0xAA,       0xBB,       0xCC,      0xDD,
                             'H' ^ 0xAA, 'e' ^ 0xBB, 'l' ^ 0xCC, 'l' ^ 0xDD, 'o' ^ 0xAA};
    HU_ASSERT_EQ(hu_doctor_ws__parse_frame(frame, sizeof(frame), NULL, NULL, NULL, NULL),
                 HU_ERR_PARSE);
}

/* RSV bits set without negotiated extensions = malformed. */
static void test_parse_frame_rejects_reserved_bits(void) {
    const uint8_t frame[] = {0x91, 0x00}; /* FIN + RSV1 + opcode 1 */
    HU_ASSERT_EQ(hu_doctor_ws__parse_frame(frame, sizeof(frame), NULL, NULL, NULL, NULL),
                 HU_ERR_PARSE);
}

/* Unknown opcode (e.g. 0x3 reserved) = malformed. */
static void test_parse_frame_rejects_unknown_opcode(void) {
    const uint8_t frame[] = {0x83, 0x00}; /* FIN + opcode 3 (reserved) */
    HU_ASSERT_EQ(hu_doctor_ws__parse_frame(frame, sizeof(frame), NULL, NULL, NULL, NULL),
                 HU_ERR_PARSE);
}

/* CLOSE frame: opcode 0x8, payload may carry status_code + reason. */
static void test_parse_frame_close_with_status_code(void) {
    /* Close with status 1000 (0x03E8) and reason "bye" */
    const uint8_t frame[] = {0x88, 0x05, 0x03, 0xE8, 'b', 'y', 'e'};
    hu_doctor_ws_opcode_t op;
    const uint8_t *payload;
    size_t payload_len, consumed;
    HU_ASSERT_EQ(
        hu_doctor_ws__parse_frame(frame, sizeof(frame), &op, &payload, &payload_len, &consumed),
        HU_OK);
    HU_ASSERT_EQ((int)op, (int)HU_DOCTOR_WS_OP_CLOSE);
    HU_ASSERT_EQ((int)payload_len, 5);
    HU_ASSERT_EQ((int)consumed, 7);
}

/* PING frame: opcode 0x9. */
static void test_parse_frame_ping_empty_payload(void) {
    const uint8_t frame[] = {0x89, 0x00};
    hu_doctor_ws_opcode_t op;
    size_t consumed;
    HU_ASSERT_EQ(hu_doctor_ws__parse_frame(frame, sizeof(frame), &op, NULL, NULL, &consumed),
                 HU_OK);
    HU_ASSERT_EQ((int)op, (int)HU_DOCTOR_WS_OP_PING);
    HU_ASSERT_EQ((int)consumed, 2);
}

/* NULL buf: must not crash, must return HU_OK + consumed=0 (incomplete). */
static void test_parse_frame_null_buf_returns_incomplete(void) {
    size_t consumed = 999;
    HU_ASSERT_EQ(hu_doctor_ws__parse_frame(NULL, 0, NULL, NULL, NULL, &consumed), HU_OK);
    HU_ASSERT_EQ((int)consumed, 0);
}

/* Format a client pong with empty payload. Under HU_IS_TEST the mask key is
 * 0x00000000 (deterministic), so the payload appears unmasked on the wire. */
static void test_format_pong_empty_payload_correct_bytes(void) {
    uint8_t buf[16];
    size_t n = hu_doctor_ws__format_pong(buf, sizeof(buf), NULL, 0);
    HU_ASSERT_EQ((int)n, 6);         /* 2-byte hdr + 4-byte mask + 0 payload */
    HU_ASSERT_EQ((int)buf[0], 0x8A); /* FIN + opcode PONG */
    HU_ASSERT_EQ((int)buf[1], 0x80); /* MASK + len 0 */
    HU_ASSERT_EQ((int)buf[2], 0x00); /* mask key */
    HU_ASSERT_EQ((int)buf[3], 0x00);
    HU_ASSERT_EQ((int)buf[4], 0x00);
    HU_ASSERT_EQ((int)buf[5], 0x00);
}

/* Format a client pong with 4-byte payload. Verify the masked payload is
 * round-trippable: payload XOR mask must equal original. With test-mode
 * zero mask, the wire bytes equal the original payload directly. */
static void test_format_pong_with_payload_round_trips(void) {
    const uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
    uint8_t buf[16];
    size_t n = hu_doctor_ws__format_pong(buf, sizeof(buf), payload, 4);
    HU_ASSERT_EQ((int)n, 10);
    /* Header */
    HU_ASSERT_EQ((int)buf[0], 0x8A);
    HU_ASSERT_EQ((int)buf[1], 0x84); /* MASK + len 4 */
    /* Mask key (zero under HU_IS_TEST) */
    HU_ASSERT_EQ((int)buf[2], 0x00);
    /* Payload (XOR'd with zero mask = original) */
    HU_ASSERT_EQ((int)buf[6], 0xDE);
    HU_ASSERT_EQ((int)buf[7], 0xAD);
    HU_ASSERT_EQ((int)buf[8], 0xBE);
    HU_ASSERT_EQ((int)buf[9], 0xEF);
}

/* Format a client close with status code 1000 (normal closure). */
static void test_format_close_with_normal_status(void) {
    uint8_t buf[16];
    size_t n = hu_doctor_ws__format_close(buf, sizeof(buf), 1000);
    HU_ASSERT_EQ((int)n, 8);         /* hdr 2 + mask 4 + payload 2 */
    HU_ASSERT_EQ((int)buf[0], 0x88); /* FIN + opcode CLOSE */
    HU_ASSERT_EQ((int)buf[1], 0x82); /* MASK + len 2 */
    /* Payload: status code in network byte order, masked with zero key */
    HU_ASSERT_EQ((int)buf[6], 0x03);
    HU_ASSERT_EQ((int)buf[7], 0xE8); /* 1000 = 0x03E8 */
}

/* Pong overflow: tiny buffer, can't fit even the header. */
static void test_format_pong_returns_zero_on_overflow(void) {
    uint8_t buf[3];
    HU_ASSERT_EQ((int)hu_doctor_ws__format_pong(buf, sizeof(buf), NULL, 0), 0);
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
    /* T3 — frame parser + writer */
    HU_RUN_TEST(test_parse_frame_text_5_byte_payload);
    HU_RUN_TEST(test_parse_frame_extended_16bit_length);
    HU_RUN_TEST(test_parse_frame_incomplete_returns_ok_with_zero_consumed);
    HU_RUN_TEST(test_parse_frame_incomplete_payload);
    HU_RUN_TEST(test_parse_frame_rejects_masked_inbound);
    HU_RUN_TEST(test_parse_frame_rejects_reserved_bits);
    HU_RUN_TEST(test_parse_frame_rejects_unknown_opcode);
    HU_RUN_TEST(test_parse_frame_close_with_status_code);
    HU_RUN_TEST(test_parse_frame_ping_empty_payload);
    HU_RUN_TEST(test_parse_frame_null_buf_returns_incomplete);
    HU_RUN_TEST(test_format_pong_empty_payload_correct_bytes);
    HU_RUN_TEST(test_format_pong_with_payload_round_trips);
    HU_RUN_TEST(test_format_close_with_normal_status);
    HU_RUN_TEST(test_format_pong_returns_zero_on_overflow);
}
