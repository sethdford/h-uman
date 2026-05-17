#ifndef HU_AGENT_GOALS_H
#define HU_AGENT_GOALS_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Goal Autonomy — persistent goal hierarchy with decomposition and
 * autonomous selection. The agent can set, track, decompose, and
 * prioritize its own goals across sessions.
 */

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

typedef enum hu_auto_goal_status {
    HU_AUTO_GOAL_PENDING = 0,
    HU_AUTO_GOAL_ACTIVE,
    HU_AUTO_GOAL_COMPLETED,
    HU_AUTO_GOAL_BLOCKED,
    HU_AUTO_GOAL_ABANDONED,
} hu_auto_goal_status_t;

typedef struct hu_goal {
    int64_t id;
    char description[1024];
    size_t description_len;
    hu_auto_goal_status_t status;
    double priority;   /* 0.0–1.0, higher = more important */
    double progress;   /* 0.0–1.0, completion ratio */
    int64_t parent_id; /* 0 = root goal */
    int64_t created_at;
    int64_t updated_at;
    int64_t deadline; /* 0 = no deadline */
    /* P3-1 (2026-05-16) — goals are now scoped per-contact. Empty string
     * means "no contact" (legacy rows from older schemas default here). */
    char contact_id[128];
    size_t contact_id_len;
} hu_goal_t;

typedef struct hu_goal_engine {
    hu_allocator_t *alloc;
    sqlite3 *db;
} hu_goal_engine_t;

hu_error_t hu_goal_engine_create(hu_allocator_t *alloc, sqlite3 *db, hu_goal_engine_t *out);
void hu_goal_engine_deinit(hu_goal_engine_t *engine);

hu_error_t hu_goal_init_tables(hu_goal_engine_t *engine);

/* P3-1 (2026-05-16) — all read/write APIs are scoped per-contact. A
 * NULL or empty contact_id is permitted (treated as ""): legacy rows
 * with an empty contact_id will be visible only to callers passing
 * the same empty contact_id, matching the conservative migration
 * stance of "old rows are off-contact by default".
 */

/* Create a new goal. Returns id in *out_id. */
hu_error_t hu_goal_create(hu_goal_engine_t *engine, const char *contact_id, size_t contact_id_len,
                          const char *description, size_t desc_len, double priority,
                          int64_t parent_id, int64_t deadline, int64_t now_ts, int64_t *out_id);

/* Update goal status. Scoped: WHERE id = ? AND contact_id = ?. */
hu_error_t hu_goal_update_status(hu_goal_engine_t *engine, const char *contact_id,
                                 size_t contact_id_len, int64_t goal_id,
                                 hu_auto_goal_status_t status, int64_t now_ts);

/* Update goal progress (0.0–1.0). Auto-completes at 1.0. Scoped. */
hu_error_t hu_goal_update_progress(hu_goal_engine_t *engine, const char *contact_id,
                                   size_t contact_id_len, int64_t goal_id, double progress,
                                   int64_t now_ts);

/* Decompose a goal into N subgoals. Returns ids in *out_ids. */
hu_error_t hu_goal_decompose(hu_goal_engine_t *engine, const char *contact_id,
                             size_t contact_id_len, int64_t parent_id, const char **descriptions,
                             const size_t *desc_lens, size_t count, int64_t now_ts,
                             int64_t *out_ids);

/* Select the next goal to work on (highest priority active/pending) for this contact. */
hu_error_t hu_goal_select_next(hu_goal_engine_t *engine, const char *contact_id,
                               size_t contact_id_len, hu_goal_t *out, bool *found);

/* List active goals (status PENDING or ACTIVE) for this contact. Caller must free *out. */
hu_error_t hu_goal_list_active(hu_goal_engine_t *engine, const char *contact_id,
                               size_t contact_id_len, hu_goal_t **out, size_t *out_count);

/* Get goal by id, scoped to contact. */
hu_error_t hu_goal_get(hu_goal_engine_t *engine, const char *contact_id, size_t contact_id_len,
                       int64_t goal_id, hu_goal_t *out, bool *found);

/* Count total goals for this contact. */
hu_error_t hu_goal_count(hu_goal_engine_t *engine, const char *contact_id, size_t contact_id_len,
                         size_t *out);

/* Build context string for agent prompt (scoped to contact). Caller must free *out. */
hu_error_t hu_goal_build_context(hu_goal_engine_t *engine, const char *contact_id,
                                 size_t contact_id_len, char **out, size_t *out_len);

const char *hu_auto_goal_status_str(hu_auto_goal_status_t status);

void hu_goal_free(hu_allocator_t *alloc, hu_goal_t *goals, size_t count);

/* Advance the highest-priority active goal's progress based on tool usage.
 * Selects the next active goal for this contact, computes a delta from
 * tool_results_count, and persists the updated progress (clamped to 1.0). */
void hu_goal_engine_record_turn_progress(hu_goal_engine_t *engine, const char *contact_id,
                                         size_t contact_id_len, size_t tool_results_count);

#endif /* HU_ENABLE_SQLITE */
#endif /* HU_AGENT_GOALS_H */
