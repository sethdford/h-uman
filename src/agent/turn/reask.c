/* src/agent/turn/reask.c — the re-ask predicate behind hu_agent_turn's
 * value-learning signal ("user repeats a similar message = previous answer
 * was wrong"), extracted from agent_turn_run so it can be tested directly.
 *
 * The inline version had two bugs that cancelled out in short conversations
 * and fired constantly in long ones: its window floor `history_count - 8`
 * wrapped on size_t below 8 entries (so the scan never ran), and it started at
 * history[history_count - 2], which by then is the current user message, so
 * from 8 entries on every message over 10 chars "re-asked" itself. */
#include "human/agent/reask.h"
#include "../agent_internal.h"
#include <string.h>

#define HU_REASK_WINDOW    8
#define HU_REASK_MIN_LEN   10
#define HU_REASK_CMP_CHARS 100
#define HU_REASK_MATCH_PCT 70

static bool reask_similar(const char *a, size_t a_len, const char *b, size_t b_len) {
    size_t min_l = a_len < b_len ? a_len : b_len;
    size_t match = 0;
    for (size_t ci = 0; ci < min_l && ci < HU_REASK_CMP_CHARS; ci++) {
        if (a[ci] == b[ci])
            match++;
    }
    return min_l > 0 && match * 100 / min_l > HU_REASK_MATCH_PCT;
}

bool hu_agent_history_is_reask(const hu_owned_message_t *history, size_t history_count,
                               const char *msg, size_t msg_len) {
    if (!history || !msg || history_count < 4 || msg_len <= HU_REASK_MIN_LEN)
        return false;
    /* The current message is located by content, not by index: tool results,
     * tool-call assistant entries and self-critique notes can follow it, so
     * history_count - 2 is only its position on a plain reply. */
    bool skipped_current = false;
    size_t floor_hi = hu_agent_history_floor(history_count, HU_REASK_WINDOW);
    for (size_t hi = history_count; hi > floor_hi; hi--) {
        const hu_owned_message_t *h = &history[hi - 1];
        if (h->role != HU_ROLE_USER || !h->content || h->content_len <= HU_REASK_MIN_LEN)
            continue;
        if (!skipped_current && h->content_len == msg_len &&
            memcmp(h->content, msg, msg_len) == 0) {
            skipped_current = true;
            continue;
        }
        if (reask_similar(msg, msg_len, h->content, h->content_len))
            return true;
    }
    return false;
}
