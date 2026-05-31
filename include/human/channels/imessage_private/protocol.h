#ifndef HUMAN_CHANNELS_IMESSAGE_PRIVATE_PROTOCOL_H
#define HUMAN_CHANNELS_IMESSAGE_PRIVATE_PROTOCOL_H

/* iMessage private-API transport protocol — pure helpers shared by the daemon
 * TCP server (Phase 3) and exercised directly by unit tests (Phase 1).
 *
 * This is the C side of the contract documented in
 * docs/investigations/imessage-private-api-mechanism.md. It speaks the same
 * wire protocol as the injected Swift helper dylib (jesec/imessage-rs's
 * TCPClient): a per-user localhost port and newline-delimited JSON.
 *
 * Everything here is PURE (no sockets, no I/O) so the framing + port math can
 * be pinned by tests on any platform without SIP, Messages.app, or a network. */

#include <stdbool.h>
#include <stddef.h>

/* Port range, matching the Swift TCPClient: base 45670, computed per-uid, then
 * clamped into [45670, 65535]. */
#define HU_IMESSAGE_PRIVATE_PORT_BASE 45670
#define HU_IMESSAGE_PRIVATE_PORT_MAX  65535
/* The reference uid the base port is anchored to (the first console user). */
#define HU_IMESSAGE_PRIVATE_PORT_UID_ANCHOR 501

/* Compute the localhost port the helper dylib will connect to for a given uid.
 *   port = clamp(BASE + (uid - 501), BASE, 65535)
 * Pure; deterministic. Matches TCPClient.init() in the Swift helper. */
int hu_imessage_private_port_for_uid(long uid);

/* OFF -> SHADOW -> LIVE activation states (see feature-gate-requires-measurement).
 * Kept here (not in config.h) so the protocol module owns its own vocabulary;
 * config.h includes this header to carry the mode on the channel config. */
typedef enum {
    HU_IMESSAGE_PRIVATE_MODE_OFF = 0,    /* default: do nothing, zero cost */
    HU_IMESSAGE_PRIVATE_MODE_SHADOW = 1, /* run + log what it WOULD do; output unchanged */
    HU_IMESSAGE_PRIVATE_MODE_LIVE = 2,   /* actually drive IMCore */
} hu_imessage_private_mode_t;

/* Parse a mode string (case-insensitive "off"/"shadow"/"live"); anything else
 * (including NULL/empty) returns OFF — the safe default. */
hu_imessage_private_mode_t hu_imessage_private_mode_from_string(const char *s);

/* Stable lowercase name for a mode ("off"/"shadow"/"live"). Never NULL. */
const char *hu_imessage_private_mode_name(hu_imessage_private_mode_t mode);

/* ── Newline-delimited line framing ──────────────────────────────────────
 * Accumulates raw socket bytes and yields complete '\n'-delimited lines, with
 * a trailing '\r' stripped and empty lines skipped — matching the Swift
 * TCPClient.processBuffer() behavior (it sends JSON + "\r\n", parses on '\n').
 *
 * Allocation uses malloc/realloc/free directly (daemon-side control-plane, not
 * a hot path); every allocation is freed, ASan-clean. */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} hu_imsg_line_buf_t;

/* Initialize an empty buffer (no allocation until the first append). */
void hu_imsg_line_buf_init(hu_imsg_line_buf_t *buf);

/* Free the backing storage and reset to empty. Safe on a zeroed/already-freed
 * buffer (idempotent). */
void hu_imsg_line_buf_free(hu_imsg_line_buf_t *buf);

/* Append n raw bytes. Returns false on allocation failure (buffer unchanged). */
bool hu_imsg_line_buf_append(hu_imsg_line_buf_t *buf, const char *bytes, size_t n);

/* Pop the next complete line, or NULL if no full line is buffered yet.
 * The returned string is heap-allocated, NUL-terminated, with any trailing
 * '\r' removed; the caller frees it with free(). Empty lines are skipped (the
 * function advances past them and returns the next non-empty line, or NULL).
 * On success, *out_len (if non-NULL) is set to the line length. */
char *hu_imsg_line_buf_next(hu_imsg_line_buf_t *buf, size_t *out_len);

#endif /* HUMAN_CHANNELS_IMESSAGE_PRIVATE_PROTOCOL_H */
