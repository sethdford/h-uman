#ifndef HU_DAEMON_OUTBOUND_SANITIZE_H
#define HU_DAEMON_OUTBOUND_SANITIZE_H

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

#endif /* HU_DAEMON_OUTBOUND_SANITIZE_H */
