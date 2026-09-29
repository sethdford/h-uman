#ifndef HU_MEMORY_PROSPECTIVE_H
#define HU_MEMORY_PROSPECTIVE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

#ifdef HU_ENABLE_SQLITE

#include <sqlite3.h>

/* ──────────────────────────────────────────────────────────────────────────
 * F75 Prospective Memory — store and trigger future actions
 * Table: prospective_memories (created by sqlite engine)
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_prospective_entry {
    int64_t id;
    char trigger_type[32];
    char trigger_value[256];
    char action[512];
    char contact_id[128];
    int64_t expires_at;
    int64_t created_at;
} hu_prospective_entry_t;

/* Open (fired=0, unexpired) triggers of `trigger_type` for `contact_id` whose
 * trigger_value occurs as a whole word or phrase, case-folded, in the inbound
 * text `trigger_value[0..tv_len)`. Newest intention first; one entry per
 * intention (action + contact) even when several of its keywords match, so
 * the caller's render cap is spent on distinct reminders. */
hu_error_t hu_prospective_check_triggers(hu_allocator_t *alloc, sqlite3 *db,
                                         const char *trigger_type, const char *trigger_value,
                                         size_t tv_len, const char *contact_id, size_t cid_len,
                                         int64_t now_ts, hu_prospective_entry_t **out,
                                         size_t *out_count);

/* Retire the intentions that were surfaced: every open row sharing an entry's
 * action + contact_id is set fired=1, so a reminder is rendered once and its
 * sibling keywords stop re-firing on the next text. `count == 0` is a no-op.
 * (fired=2 is reserved for rows retired by the extractor's keyword prune.) */
hu_error_t hu_prospective_mark_fired(sqlite3 *db, const hu_prospective_entry_t *entries,
                                     size_t count);

/* One-shot render of the open intentions cued by `text` for `contact_id`:
 *   "[PROSPECTIVE MEMORY: Remember to: <action> (triggered by: <cue>) | ...]"
 * (at most 3), and retire them (fired=1). Returns NULL when nothing is cued;
 * otherwise a NUL-terminated string the caller frees with
 * alloc->free(ctx, p, *out_len + 1).
 *
 * Lives here rather than in the daemon so every response mode can call it
 * (until 2026-09-20 it existed only inside the daemon's !llm_decides block,
 * and production — llm_decides on — never fired one of 461 open intentions)
 * and so tests can drive it against a real database: the daemon prompt
 * builder is compiled out under HU_IS_TEST. */
char *hu_prospective_directive_build(hu_allocator_t *alloc, sqlite3 *db, const char *text,
                                     size_t text_len, const char *contact_id, size_t cid_len,
                                     int64_t now_ts, size_t *out_len);

/* Retire open rows whose expires_at has passed: fired=3 ("expired, never
 * cued"), distinct from 1 (rendered) and 2 (extractor prune) so the fire rate
 * stays measurable. Returns the number of rows retired via *out_n (may be NULL). */
hu_error_t hu_prospective_expire_sweep(sqlite3 *db, int64_t now_ts, int64_t *out_n);

/* ──────────────────────────────────────────────────────────────────────────
 * Prospective tasks — time/event-triggered scheduled actions
 * Table: prospective_tasks (created by sqlite engine)
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_prospective_task {
    int64_t id;
    char description[512];
    char trigger_type[32];
    char trigger_value[256];
    double priority;
    int fired;
    int64_t created_at;
    int64_t fired_at;
} hu_prospective_task_t;

#endif /* HU_ENABLE_SQLITE */

#endif /* HU_MEMORY_PROSPECTIVE_H */
