#ifndef HU_AGENT_REASK_H
#define HU_AGENT_REASK_H

#include "human/agent.h"
#include <stdbool.h>
#include <stddef.h>

/* Re-ask detection for the value-learning signal in hu_agent_turn: true when
 * `msg` repeats an EARLIER user message in the last 8 history entries.
 *
 * Called after the turn's own user message (and usually its reply) are in
 * `history`, so the most recent USER entry whose content equals `msg`
 * exactly is the current message and is skipped; only older user entries
 * count. Similarity: both sides > 10 chars and more than 70% of the first
 * min(len, 100) characters match position by position. Requires at least 4
 * history entries and msg_len > 10. */
bool hu_agent_history_is_reask(const hu_owned_message_t *history, size_t history_count,
                               const char *msg, size_t msg_len);

#endif /* HU_AGENT_REASK_H */
