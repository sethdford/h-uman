#ifndef HU_INTELLIGENCE_SKILLS_H
#define HU_INTELLIGENCE_SKILLS_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

/* F94/F95: Learned behavioral skill (DB-backed). */
typedef struct hu_skill {
    int64_t id;
    char name[128];
    char type[32];
    char contact_id[128];
    char trigger_conditions[512];
    char strategy[2048];
    size_t strategy_len;
    double success_rate;
    int attempts;
    int successes;
    int version;
    char origin[32];
    int64_t parent_skill_id;
    int64_t created_at;
    int64_t updated_at;
    int retired;
} hu_skill_t;

/* Insert a skill. Returns id in out_id. */
hu_error_t hu_skill_insert(hu_allocator_t *alloc, sqlite3 *db,
                          const char *name, size_t name_len,
                          const char *type, size_t type_len,
                          const char *contact_id, size_t cid_len,
                          const char *trigger_conditions, size_t tc_len,
                          const char *strategy, size_t strat_len,
                          const char *origin, size_t origin_len,
                          int64_t parent_skill_id, int64_t now_ts,
                          int64_t *out_id);

/* Load active skills for contact (retired=0, contact_id=? OR contact_id IS NULL). */
hu_error_t hu_skill_load_active(hu_allocator_t *alloc, sqlite3 *db,
                                const char *contact_id, size_t cid_len,
                                hu_skill_t **out, size_t *out_count);

/* Get skill by name (retired=0). */
hu_error_t hu_skill_get_by_name(hu_allocator_t *alloc, sqlite3 *db,
                               const char *name, size_t name_len,
                               hu_skill_t *out);

/* Free array returned by hu_skill_load_active. */
void hu_skill_free(hu_allocator_t *alloc, hu_skill_t *skills, size_t count);

/* F96: Match skills when trigger conditions apply (emotion, topic, contact, confidence). */
hu_error_t hu_skill_match_triggers(hu_allocator_t *alloc, sqlite3 *db,
    const char *contact_id, size_t cid_len,
    const char *emotion, size_t emotion_len,
    const char *topic, size_t topic_len,
    double confidence,
    hu_skill_t **out, size_t *out_count);

/* Record a skill attempt. Returns attempt id in out_id. */
hu_error_t hu_skill_record_attempt(sqlite3 *db,
    int64_t skill_id, const char *contact_id, size_t cid_len,
    int64_t applied_at,
    const char *outcome_signal, size_t sig_len,
    const char *outcome_evidence, size_t ev_len,
    const char *context, size_t ctx_len,
    int64_t *out_id);

/* Update skill success rate from new totals. */
hu_error_t hu_skill_update_success_rate(sqlite3 *db,
    int64_t skill_id, int new_attempts, int new_successes);

/* F97: Evolve skill — record old strategy in skill_evolution, update strategy/version, reset stats. */
hu_error_t hu_skill_evolve(hu_allocator_t *alloc, sqlite3 *db,
    int64_t skill_id,
    const char *new_strategy, size_t strat_len,
    const char *reason, size_t reason_len,
    int64_t now_ts);

/* F99: Retire skill (set retired=1). */
hu_error_t hu_skill_retire(sqlite3 *db, int64_t skill_id);

/* F99: True if version >= 3 AND success_rate < 0.35. */
bool hu_skill_db_should_retire(int version, double success_rate);

/* F98: Transfer skill — create universal copy with new trigger, parent_skill_id=original. */
hu_error_t hu_skill_transfer(hu_allocator_t *alloc, sqlite3 *db,
    int64_t skill_id,
    const char *new_trigger, size_t trigger_len,
    double confidence_penalty,
    int64_t now_ts,
    int64_t *out_id);

/* F100: Resolve "skill:name" patterns in strategy, expand with referenced skill strategies (max depth 3). */
hu_error_t hu_skill_resolve_chain(hu_allocator_t *alloc, sqlite3 *db,
    const char *strategy, size_t strategy_len,
    char *out, size_t out_cap, size_t *out_len);

#endif /* HU_ENABLE_SQLITE */

#endif /* HU_INTELLIGENCE_SKILLS_H */
