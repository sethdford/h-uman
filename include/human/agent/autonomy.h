#ifndef HU_AGENT_AUTONOMY_H
#define HU_AGENT_AUTONOMY_H

#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HU_AUTONOMY_MAX_GOALS 16
#define HU_AUTONOMY_DEFAULT_BUDGET 8192
#define HU_AUTONOMY_CONSOLIDATION_INTERVAL_MS (6 * 60 * 60 * 1000) /* 6 hours */

typedef struct hu_autonomy_goal {
    char description[512];
    size_t description_len;
    double priority; /* 0.0-1.0 */
    bool completed;
    int64_t created_at;
    int64_t deadline; /* 0 = no deadline */
} hu_autonomy_goal_t;

typedef struct hu_autonomy_state {
    hu_autonomy_goal_t goals[HU_AUTONOMY_MAX_GOALS];
    size_t goal_count;
    int64_t last_consolidation;
    int64_t session_start;
    size_t context_tokens_used;
    size_t context_budget;
} hu_autonomy_state_t;

hu_error_t hu_autonomy_init(hu_autonomy_state_t *state, size_t context_budget);
hu_error_t hu_autonomy_add_goal(hu_autonomy_state_t *state, const char *desc, size_t desc_len,
                                 double priority);
hu_error_t hu_autonomy_get_next_goal(const hu_autonomy_state_t *state, hu_autonomy_goal_t *out);
hu_error_t hu_autonomy_mark_complete(hu_autonomy_state_t *state, size_t goal_index);
bool hu_autonomy_needs_consolidation(const hu_autonomy_state_t *state, int64_t now_ms);
hu_error_t hu_autonomy_consolidate(hu_autonomy_state_t *state);

hu_error_t hu_autonomy_generate_intrinsic_goal(hu_autonomy_state_t *state,
                                                size_t completed_count, size_t failed_count);

/* Self-initiated agenda: when `engine`'s persisted agenda for `contact_id` is
 * EMPTY, generate one intrinsic goal (via hu_autonomy_generate_intrinsic_goal)
 * and seed it into the persisted goal engine the world model reads into the
 * prompt — wiring the otherwise-dormant generator into the live path.
 *
 * mode: 0 = off (no-op), 1 = shadow (compute + log, do NOT persist),
 *       2 = on (persist via hu_goal_create). No-op (out_id = 0) when the
 * contact already has ≥1 active goal, so it never piles up. Forward-declared
 * engine type keeps SQLite out of this widely-included header. */
struct hu_goal_engine;
hu_error_t hu_autonomy_seed_intrinsic_goal(struct hu_goal_engine *engine, const char *contact_id,
                                           size_t contact_id_len, int mode, int64_t now_ts,
                                           int64_t *out_id);

hu_error_t hu_autonomy_externalize_state(const hu_autonomy_state_t *state,
                                          char *buf, size_t buf_size, size_t *out_len);

hu_error_t hu_autonomy_restore_state(hu_autonomy_state_t *state,
                                      const char *buf, size_t buf_len);

#endif
