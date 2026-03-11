#ifndef HU_INTELLIGENCE_SKILL_SYSTEM_H
#define HU_INTELLIGENCE_SKILL_SYSTEM_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- F94–F101: Skills System + Meta-Learning --- */

typedef enum hu_skill_status {
    HU_SKILL_EMERGING = 0,
    HU_SKILL_DEVELOPING,
    HU_SKILL_PROFICIENT,
    HU_SKILL_MASTERED,
    HU_SKILL_RETIRED
} hu_skill_status_t;

typedef struct hu_learned_skill {
    int64_t id;
    char *name;
    size_t name_len;
    char *description;
    size_t description_len;
    char *trigger;
    size_t trigger_len; /* when to apply this skill */
    char *strategy;
    size_t strategy_len; /* how to execute it */
    hu_skill_status_t status;
    double success_rate;
    uint32_t usage_count;
    uint64_t learned_at;
    uint64_t last_used;
    int64_t parent_skill_id; /* for chaining: 0 if root */
} hu_learned_skill_t;

typedef struct hu_meta_learning_state {
    double learning_rate;  /* how fast new skills are acquired */
    double transfer_rate;   /* how well skills transfer between contexts */
    uint32_t total_skills;
    uint32_t mastered_skills;
} hu_meta_learning_state_t;

/* F94: Create learned_skills table */
hu_error_t hu_skills_create_table_sql(char *buf, size_t cap, size_t *out_len);

/* F94: Insert a learned skill */
hu_error_t hu_skills_insert_sql(const hu_learned_skill_t *skill, char *buf, size_t cap,
                                size_t *out_len);

/* F94: Update usage after skill application */
hu_error_t hu_skills_update_usage_sql(int64_t id, double new_success_rate, char *buf, size_t cap,
                                      size_t *out_len);

/* F94: Query active (non-retired) skills */
hu_error_t hu_skills_query_active_sql(char *buf, size_t cap, size_t *out_len);

/* F95: Query skills by trigger */
hu_error_t hu_skills_query_by_trigger_sql(const char *trigger, size_t trigger_len, char *buf,
                                          size_t cap, size_t *out_len);

/* F94: Retire a skill */
hu_error_t hu_skills_retire_sql(int64_t id, char *buf, size_t cap, size_t *out_len);

/* F96: Match incoming situation to learned skills (word overlap, 0–1) */
double hu_skill_trigger_match(const char *skill_trigger, size_t trigger_len,
                              const char *situation, size_t situation_len);

/* F97: Refine skill success rate after use (EMA, alpha = 1/(usage_count+1)) */
double hu_skill_refine_success(double current_rate, bool was_successful, uint32_t usage_count);

/* F98: Skill transfer — can this skill apply in a new context? */
double hu_skill_transfer_score(const char *source_context, size_t src_len,
                               const char *target_context, size_t tgt_len,
                               double source_proficiency);

/* F99: Should this skill be retired? */
bool hu_skill_should_retire(double success_rate, uint32_t usage_count, uint64_t last_used_ms,
                            uint64_t now_ms);

/* F100: Skill chaining — query child skills of a parent */
hu_error_t hu_skill_chain_query_sql(int64_t parent_id, char *buf, size_t cap, size_t *out_len);

/* F101: Meta-learning — compute learning state */
hu_meta_learning_state_t hu_meta_learning_compute(uint32_t total_skills, uint32_t mastered,
                                                   uint32_t total_attempts,
                                                   uint32_t successful_attempts);

const char *hu_skill_status_str(hu_skill_status_t status);

hu_error_t hu_skills_build_prompt(hu_allocator_t *alloc, const hu_learned_skill_t *skills,
                                  size_t count, char **out, size_t *out_len);

void hu_learned_skill_deinit(hu_allocator_t *alloc, hu_learned_skill_t *skill);

#endif /* HU_INTELLIGENCE_SKILL_SYSTEM_H */
