/* h-uman doctor WebSocket consumer — subscribes to the gateway's
 * event broadcast and streams formatted events to stdout + JSONL log.
 *
 * Spec: docs/plans/2026-05-27-doctor-ws-consumer/
 *
 * Two halves of the API:
 *  - PURE helpers (format_event_line, event_matches_filter): no I/O,
 *    fully unit-testable, no HU_IS_TEST gate needed.
 *  - hu_doctor_ws_watch: full TCP+WS loop, gated under HU_IS_TEST so
 *    unit tests don't actually open sockets.
 */

#ifndef HU_DOCTOR_WS_CONSUMER_H
#define HU_DOCTOR_WS_CONSUMER_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hu_doctor_ws_config {
    const char *host;                /* default "127.0.0.1" */
    uint16_t port;                   /* default 3006 */
    const char *path;                /* default "/ws" */
    const char *event_filter;        /* NULL = all events; else comma-separated */
    const char *log_path;            /* NULL = auto-generate under ~/.human/logs/ */
    uint32_t max_reconnect_attempts; /* default 3 */
    bool quiet_stdout;               /* default false; true suppresses stdout (tests) */
} hu_doctor_ws_config_t;

/* Initialize a default config — caller can override individual fields. */
hu_doctor_ws_config_t hu_doctor_ws_config_default(void);

/* PURE: format one event into a one-line printable string. The line includes
 *   - [HH:MM:SS] local timestamp
 *   - event name
 *   - seq=<N>
 *   - a one-line summary of the payload (event-specific)
 *
 * Returns a heap string the CALLER owns (free via alloc->free). NULL on
 * format error.
 *
 * payload_json must be a complete JSON object string ("{...}"). Pass "{}"
 * if the source emitted no payload.
 *
 * Pure: no I/O, no globals, no time-of-day side effects beyond the
 * embedded timestamp (which uses local clock_gettime; pass now_epoch=0 to
 * accept current time, else use the fixed epoch for deterministic tests).
 */
char *hu_doctor_ws_format_event_line(hu_allocator_t *alloc, const char *event_name,
                                     const char *payload_json, uint64_t seq, int64_t now_epoch);

/* PURE: returns true iff `event_name` should be printed under `filter_csv`.
 *
 * filter_csv conventions:
 *   NULL or empty  -> match all events
 *   "chat,error"   -> exact match either "chat" or "error"
 *   "chat, error " -> whitespace trimmed around each token
 *
 * No glob, no negation in v1 (see design.md DECISION-3).
 */
bool hu_doctor_ws_event_matches_filter(const char *event_name, const char *filter_csv);

/* Opens a WebSocket connection to ws://<host>:<port><path>, subscribes
 * to events (the gateway broadcasts all events to every connection by
 * default — no client-side subscribe message needed), prints each
 * matching event to stdout + JSONL log, reconnects on disconnect up to
 * cfg->max_reconnect_attempts, returns HU_OK on clean exit (Ctrl+C) or
 * HU_ERR_IO on reconnect-failed.
 *
 * Not implemented yet — T3-T6. T1 + T2 ship the pure helpers and the
 * stub returns HU_ERR_NOT_SUPPORTED.
 */
hu_error_t hu_doctor_ws_watch(hu_allocator_t *alloc, const hu_doctor_ws_config_t *cfg);

/* ── T2 RFC 6455 handshake helpers (testable, pure where possible) ──── */

/* Compute the Sec-WebSocket-Accept value the SERVER would return for a
 * given client_key, per RFC 6455 §4.2.2. SHA-1(client_key + WS_MAGIC)
 * then base64. Used to verify the server's handshake response.
 *
 * `out` must be at least 29 bytes (28 base64 chars + NUL). */
bool hu_doctor_ws__compute_accept_key(const char *client_key, char *out, size_t out_size);

/* Generate a 24-char base64-encoded client_key from 16 random bytes.
 * Under HU_IS_TEST returns a DETERMINISTIC fixed key
 * ("dGVzdC1rZXktMTIzNDU2Nw==") so handshake bytes are reproducible.
 * Real runs use /dev/urandom.
 *
 * `out` must be at least 25 bytes. */
bool hu_doctor_ws__generate_client_key(char *out, size_t out_size);

/* Format the HTTP/1.1 GET upgrade request bytes into `buf`. Returns
 * number of bytes written, or 0 on overflow / NULL inputs. Pure. */
size_t hu_doctor_ws__format_upgrade_request(char *buf, size_t buf_size, const char *host,
                                            uint16_t port, const char *path,
                                            const char *client_key);

/* Parse the server's handshake response. Returns true iff status line
 * is "HTTP/1.1 101 ..." AND Sec-WebSocket-Accept matches expected_accept
 * exactly. Pure. */
bool hu_doctor_ws__verify_handshake_response(const char *resp, size_t resp_len,
                                             const char *expected_accept);

/* ── T3 RFC 6455 frame parser (pure) ─────────────────────────────────── */

typedef enum hu_doctor_ws_opcode {
    HU_DOCTOR_WS_OP_CONT = 0x0,
    HU_DOCTOR_WS_OP_TEXT = 0x1,
    HU_DOCTOR_WS_OP_BIN = 0x2,
    HU_DOCTOR_WS_OP_CLOSE = 0x8,
    HU_DOCTOR_WS_OP_PING = 0x9,
    HU_DOCTOR_WS_OP_PONG = 0xA
} hu_doctor_ws_opcode_t;

/* Maximum payload size we accept from server, to prevent OOM on hostile
 * input. 1 MB is plenty for event broadcasts; the gateway typically sends
 * frames < 8 KB. */
#define HU_DOCTOR_WS_MAX_PAYLOAD (1u * 1024u * 1024u)

/* Parse one WebSocket frame from `buf`. Returns:
 *   HU_OK + *out_consumed > 0   — full frame parsed; *out_consumed bytes
 *                                  processed. *out_payload points INTO buf
 *                                  (zero-copy) for *out_payload_len bytes.
 *                                  *out_opcode is the WebSocket opcode.
 *   HU_OK + *out_consumed == 0  — frame is INCOMPLETE; caller should read
 *                                  more bytes and call again with the
 *                                  extended buffer. All other outs are
 *                                  0/NULL — caller must NOT consume any
 *                                  bytes.
 *   HU_ERR_PARSE                — malformed frame: reserved bits set,
 *                                  mask bit set on inbound (server MUST
 *                                  NOT mask per RFC §5.1), unknown opcode,
 *                                  or payload exceeds HU_DOCTOR_WS_MAX_PAYLOAD.
 *
 * Pure; no I/O. Choosing HU_OK + consumed==0 over a separate error code
 * keeps the API single-return-value-checkable while still letting callers
 * distinguish "need more data" from "malformed". */
hu_error_t hu_doctor_ws__parse_frame(const uint8_t *buf, size_t buf_len,
                                     hu_doctor_ws_opcode_t *out_opcode, const uint8_t **out_payload,
                                     size_t *out_payload_len, size_t *out_consumed);

/* Format a PONG frame in response to PING. Pure. Returns number of
 * bytes written (always between 2 and 14 for client→server pongs of
 * length 0-125, since masking adds 4 bytes), or 0 on overflow. */
size_t hu_doctor_ws__format_pong(uint8_t *buf, size_t buf_size, const uint8_t *payload,
                                 size_t payload_len);

/* Format a CLOSE frame (clean shutdown). Pure. Same return semantics as
 * format_pong. */
size_t hu_doctor_ws__format_close(uint8_t *buf, size_t buf_size, uint16_t status_code);

#ifdef __cplusplus
}
#endif

#endif /* HU_DOCTOR_WS_CONSUMER_H */
