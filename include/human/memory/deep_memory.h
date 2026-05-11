#ifndef HU_MEMORY_DEEP_MEMORY_H
#define HU_MEMORY_DEEP_MEMORY_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- F70 Episodic Memory ---
 * Deep-memory consolidated episode (multi-day distilled memories with
 * emotional arc, impact, participants). Renamed from hu_episode_t to avoid
 * ODR conflict with the session-level episode struct in agent/episodic.h
 * (which now uses hu_session_episode_t). */
typedef struct hu_deep_episode {
    int64_t id;
    char *summary;
    size_t summary_len;
    char *emotional_arc;
    size_t emotional_arc_len; /* "started anxious, ended relieved" */
    double impact_score;      /* 0-1 */
    char *participants;
    size_t participants_len; /* JSON array */
    uint64_t occurred_at;
    char *source_tag;
    size_t source_tag_len; /* "conversation", "event", "memory" */
} hu_deep_episode_t;

hu_error_t hu_episodic_create_table_sql(char *buf, size_t cap, size_t *out_len);
hu_error_t hu_episodic_insert_sql(const hu_deep_episode_t *ep, char *buf, size_t cap,
                                  size_t *out_len);
hu_error_t hu_episodic_query_by_contact_sql(const char *contact_id, size_t len, uint32_t limit,
                                            char *buf, size_t cap, size_t *out_len);
hu_error_t hu_episodic_query_high_impact_sql(double min_impact, uint32_t limit, char *buf,
                                             size_t cap, size_t *out_len);

/* --- F71 Associative Recall --- */
double hu_episodic_relevance_score(const char *episode_summary, size_t summary_len,
                                   const char *trigger, size_t trigger_len);

/* --- F72 Consolidation Engine --- */
typedef struct hu_consolidation_result {
    size_t merged_count;
    size_t retained_count;
} hu_consolidation_result_t;

bool hu_consolidation_should_merge(const char *a, size_t a_len, const char *b, size_t b_len,
                                  double threshold);
hu_error_t hu_consolidation_merge_sql(int64_t keep_id, int64_t remove_id, char *buf, size_t cap,
                                      size_t *out_len);

/* --- F74 Source Tagging --- */
/* Handled by source_tag in hu_deep_episode_t */

/* --- F75 Prospective Memory --- */
typedef struct hu_prospective_item {
    int64_t id;
    char *description;
    size_t description_len;
    char *trigger_type;
    size_t trigger_type_len; /* "time", "event", "topic" */
    char *trigger_value;
    size_t trigger_value_len; /* "2026-03-15", "they mention X" */
    uint64_t created_at;
    bool completed;
} hu_prospective_item_t;

hu_error_t hu_prospective_create_table_sql(char *buf, size_t cap, size_t *out_len);
hu_error_t hu_prospective_insert_sql(const hu_prospective_item_t *item, char *buf, size_t cap,
                                    size_t *out_len);
hu_error_t hu_prospective_query_pending_sql(char *buf, size_t cap, size_t *out_len);
hu_error_t hu_prospective_complete_sql(int64_t id, char *buf, size_t cap, size_t *out_len);
hu_error_t hu_prospective_build_prompt(hu_allocator_t *alloc,
                                       const hu_prospective_item_t *items, size_t count,
                                       char **out, size_t *out_len);

/* --- F76 Emotional Residue --- */
typedef struct hu_emotional_residue {
    char *contact_id;
    size_t contact_id_len;
    char *emotion;
    size_t emotion_len;
    double intensity;
    uint64_t from_timestamp;
    double decay_rate;
} hu_emotional_residue_t;

hu_error_t hu_residue_create_table_sql(char *buf, size_t cap, size_t *out_len);
hu_error_t hu_residue_insert_sql(const hu_emotional_residue_t *r, char *buf, size_t cap,
                                size_t *out_len);
hu_error_t hu_residue_query_active_sql(const char *contact_id, size_t len, char *buf, size_t cap,
                                       size_t *out_len);
double hu_residue_current_intensity(double initial_intensity, double decay_rate,
                                   double hours_elapsed);
hu_error_t hu_residue_build_prompt(hu_allocator_t *alloc,
                                  const hu_emotional_residue_t *residues, size_t count,
                                  double hours_elapsed, char **out, size_t *out_len);

void hu_episode_deinit(hu_allocator_t *alloc, hu_deep_episode_t *ep);
void hu_prospective_item_deinit(hu_allocator_t *alloc, hu_prospective_item_t *item);
void hu_emotional_residue_deinit(hu_allocator_t *alloc, hu_emotional_residue_t *r);

#endif /* HU_MEMORY_DEEP_MEMORY_H */
