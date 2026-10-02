#ifndef HU_DAEMON_OUTBOUND_SANITIZE_H
#define HU_DAEMON_OUTBOUND_SANITIZE_H

#include "human/core/allocator.h"
#include "human/observer.h"
#include <stdbool.h>
#include <stddef.h>

/* Last text cleanup before the reactive reply is split into bubbles, moved
 * out of daemon.c's send block so the real-turn replay harness
 * (src/daemon/replay_turn.c) runs the same bytes production sends.
 *
 *   1. Strip invalid UTF-8 and surrogate-encoded garbage. Keeps ASCII
 *      printable, '\n', '\t' and valid multi-byte UTF-8 (emoji included).
 *   2. llm_decides only: a reply that opens with '(' loses everything up to
 *      and including its last ')' (local models sometimes emit parenthetical
 *      analysis instead of the message). When nothing is left, the reply
 *      becomes "hey whats up" — written only when `cap` (allocated bytes,
 *      NUL included) can hold it; otherwise the reply is emptied.
 *
 * Edits `response` in place and updates *response_len. NULL or empty input is
 * a no-op. */
void hu_daemon_outbound_sanitize(char *response, size_t *response_len, size_t cap, bool llm_decides,
                                 hu_observer_t *observer);

/* The missed-message acknowledgement joined to the reply: "ack\n\nreply", or
 * just "ack" when the reply is empty (e.g. an all-reasoning reply the
 * sanitizer emptied) — never "ack" plus a dangling blank line. Returns an
 * allocation of *out_len + 1 bytes on `alloc`, or NULL. */
char *hu_daemon_join_ack(hu_allocator_t *alloc, const char *ack, const char *reply,
                         size_t reply_len, size_t *out_len);

#endif /* HU_DAEMON_OUTBOUND_SANITIZE_H */
