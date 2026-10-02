/* Request history budget — see include/human/agent/history_budget.h. */
#include "human/agent/history_budget.h"
#include "human/context.h"
#include "human/core/log.h"
#include <stdatomic.h>
#include <stdlib.h>

hu_gate_mode_t hu_history_budget_mode(void) {
    return hu_gate_mode_from_env("HU_HISTORY_BUDGET", HU_GATE_OFF);
}

size_t hu_history_budget_max_total(void) {
    const char *v = getenv("HU_HISTORY_BUDGET_MAX_TOTAL_BYTES");
    if (!v || !v[0])
        return HU_HISTORY_BUDGET_MAX_TOTAL_DEFAULT;
    char *end = NULL;
    unsigned long long n = strtoull(v, &end, 10);
    if (end == v || *end != '\0')
        return HU_HISTORY_BUDGET_MAX_TOTAL_DEFAULT;
    if (n < HU_HISTORY_BUDGET_MAX_TOTAL_FLOOR)
        return HU_HISTORY_BUDGET_MAX_TOTAL_FLOOR;
    if (n > HU_HISTORY_BUDGET_MAX_TOTAL_CEIL)
        return HU_HISTORY_BUDGET_MAX_TOTAL_CEIL;
    return (size_t)n;
}

/* Drop from msgs[1] up while `bytes` exceeds `limit`, never the current. */
static size_t drop_oldest_until(const hu_chat_message_t *msgs, size_t count, size_t bytes,
                                size_t limit) {
    size_t keep_from = 1;
    while (bytes > limit && keep_from < count - 1)
        bytes -= hu_chat_message_estimate_bytes(&msgs[keep_from++]);
    return keep_from;
}

static size_t sum_bytes(const hu_chat_message_t *msgs, size_t from, size_t count) {
    size_t t = 0;
    for (size_t i = from; i < count; i++)
        t += hu_chat_message_estimate_bytes(&msgs[i]);
    return t;
}

size_t hu_history_budget_keep_from_legacy(const hu_chat_message_t *msgs, size_t count,
                                          size_t budget) {
    if (!msgs || count <= 2)
        return 1;
    /* A1b (2026-05-19): system prompt + history under one budget. */
    return drop_oldest_until(msgs, count, sum_bytes(msgs, 0, count), budget);
}

size_t hu_history_budget_keep_from_scoped(const hu_chat_message_t *msgs, size_t count,
                                          size_t hist_budget, size_t max_total) {
    if (!msgs || count <= 2)
        return 1;
    size_t sys = hu_chat_message_estimate_bytes(&msgs[0]);
    size_t room = max_total > sys ? max_total - sys : 0;
    size_t limit = hist_budget < room ? hist_budget : room;
    return drop_oldest_until(msgs, count, sum_bytes(msgs, 1, count), limit);
}

/* Keep msgs[0] and msgs[keep_from..count), sliding survivors down. */
static size_t apply_keep_from(hu_chat_message_t *msgs, size_t count, size_t keep_from) {
    size_t tail = count - keep_from;
    for (size_t i = 0; i < tail; i++)
        msgs[1 + i] = msgs[keep_from + i];
    return 1 + tail;
}

size_t hu_history_budget_fit(hu_chat_message_t *msgs, size_t count, hu_gate_mode_t mode,
                             size_t max_total, hu_history_budget_plan_t *plan) {
    hu_history_budget_plan_t p = {.msgs_before = count};
    if (!msgs || count < 2) {
        p.msgs_after_legacy = p.msgs_after_scoped = count;
        if (plan)
            *plan = p;
        return count;
    }
    size_t legacy_from = hu_history_budget_keep_from_legacy(msgs, count, HU_HISTORY_BUDGET_BYTES);
    size_t scoped_from =
        hu_history_budget_keep_from_scoped(msgs, count, HU_HISTORY_BUDGET_BYTES, max_total);
    p.sys_bytes = hu_chat_message_estimate_bytes(&msgs[0]);
    p.hist_bytes = sum_bytes(msgs, 1, count);
    p.msgs_after_legacy = 1 + (count - legacy_from);
    p.msgs_after_scoped = 1 + (count - scoped_from);
    if (plan)
        *plan = p;

    size_t keep_from = mode == HU_GATE_LIVE ? scoped_from : legacy_from;
    size_t out = keep_from > 1 ? apply_keep_from(msgs, count, keep_from) : count;

    if (mode == HU_GATE_SHADOW) {
        hu_log_info("agent_turn", NULL,
                    "[HU_HISTORY_BUDGET shadow] msgs_before=%zu msgs_after=%zu new_msgs_after=%zu "
                    "sys_bytes=%zu hist_bytes=%zu new_keeps_more=%d",
                    p.msgs_before, p.msgs_after_legacy, p.msgs_after_scoped, p.sys_bytes,
                    p.hist_bytes, p.msgs_after_scoped > p.msgs_after_legacy ? 1 : 0);
    } else if (mode == HU_GATE_LIVE) {
        hu_log_info("agent_turn", NULL,
                    "[HU_HISTORY_BUDGET live] msgs_before=%zu msgs_after=%zu legacy_msgs_after=%zu "
                    "sys_bytes=%zu total_bytes=%zu max_total=%zu",
                    p.msgs_before, out, p.msgs_after_legacy, p.sys_bytes, sum_bytes(msgs, 0, out),
                    max_total);
    }

    /* Replaces a log-once warning that hid how often history was cut: count
     * every truncation, log the 1st and every Nth. */
    if (keep_from > 1) {
        static atomic_size_t truncations = 0;
        size_t n = atomic_fetch_add(&truncations, 1) + 1;
        if (n == 1 || n % HU_HISTORY_BUDGET_LOG_EVERY == 0)
            hu_log_warn("agent_turn", NULL,
                        "history truncated (%zu so far, logged every %d): dropped %zu oldest "
                        "messages (msgs %zu -> %zu, sys_bytes %zu, policy %s)",
                        n, HU_HISTORY_BUDGET_LOG_EVERY, keep_from - 1, count, out, p.sys_bytes,
                        mode == HU_GATE_LIVE ? "history-only" : "legacy");
    }
    return out;
}
