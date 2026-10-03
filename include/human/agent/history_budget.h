#ifndef HU_AGENT_HISTORY_BUDGET_H
#define HU_AGENT_HISTORY_BUDGET_H

#include "human/core/gate_mode.h"
#include "human/provider.h"
#include <stdbool.h>
#include <stddef.h>

/* Request history budget (hu_agent_internal_fit_history, A1b 2026-05-19).
 *
 * The legacy policy drops the oldest history until system prompt + history
 * fits 20 KB. The system prompt is capped at 24 KB (HU_PROMPT_TRIM_BUDGET_BYTES)
 * and runs ~17-22 KB in prod, so on most turns that budget is already spent
 * before any history is counted: every prior message is dropped and the model
 * sees msgs=2 (350 of 944 prod calls, 2026-10-01 audit).
 *
 * Gate HU_HISTORY_BUDGET (hu_gate_mode_parse, unset -> OFF):
 *   OFF    legacy policy, byte-identical messages.
 *   SHADOW legacy policy applied; one line per call compares the two:
 *          `[HU_HISTORY_BUDGET shadow] msgs_before= msgs_after= new_msgs_after=
 *          sys_bytes= hist_bytes= new_keeps_more=`.
 *   LIVE   history-only budget: the 20 KB counts the non-system messages
 *          only, and system + history is capped at max_total
 *          (HU_HISTORY_BUDGET_MAX_TOTAL_BYTES, default 40 KB). GLM-4.5-Air
 *          serves a 131,072-token window (max_position_embeddings; the mlx
 *          server sets no max_kv_size), so even the 96 KB clamp ceiling stays
 *          well inside it at any bytes-per-token ratio >= 1.
 * Activation to LIVE gated on the n=40 blind A/B; see
 * docs/guides/thread-context.md. */

#define HU_HISTORY_BUDGET_BYTES             (20 * 1024)
#define HU_HISTORY_BUDGET_MAX_TOTAL_DEFAULT (40 * 1024)
#define HU_HISTORY_BUDGET_MAX_TOTAL_FLOOR   HU_HISTORY_BUDGET_BYTES
#define HU_HISTORY_BUDGET_MAX_TOTAL_CEIL    (96 * 1024)
#define HU_HISTORY_BUDGET_LOG_EVERY         25 /* truncation counter log cadence */

typedef struct hu_history_budget_plan {
    size_t msgs_before;
    size_t msgs_after_legacy; /* what OFF/SHADOW keep */
    size_t msgs_after_scoped; /* what LIVE keeps */
    size_t sys_bytes;
    size_t hist_bytes; /* every non-system message, before fitting */
} hu_history_budget_plan_t;

/* HU_HISTORY_BUDGET per hu_gate_mode_parse; unset -> OFF. */
hu_gate_mode_t hu_history_budget_mode(void);

/* HU_HISTORY_BUDGET_MAX_TOTAL_BYTES clamped to [FLOOR, CEIL]; unset or
 * unparsable -> DEFAULT. */
size_t hu_history_budget_max_total(void);

/* Pure planners over msgs[0] = system, msgs[count-1] = current. Each returns
 * the index of the first history message to keep (1 = keep all); the current
 * message is never dropped. */
size_t hu_history_budget_keep_from_legacy(const hu_chat_message_t *msgs, size_t count,
                                          size_t budget);
size_t hu_history_budget_keep_from_scoped(const hu_chat_message_t *msgs, size_t count,
                                          size_t hist_budget, size_t max_total);

/* Plan both policies, apply the one `mode` selects (slides survivors down to
 * msgs[1..]), log per the gate contract, and return the new count. `plan`
 * may be NULL. */
size_t hu_history_budget_fit(hu_chat_message_t *msgs, size_t count, hu_gate_mode_t mode,
                             size_t max_total, hu_history_budget_plan_t *plan);

#endif /* HU_AGENT_HISTORY_BUDGET_H */
