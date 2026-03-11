#ifndef HU_MEMORY_EPISODIC_H
#define HU_MEMORY_EPISODIC_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

/* Episode: a summarized interaction/conversation with a contact.
 * Used for associative recall and salience reinforcement.
 * NOTE: hu_episode_t may also be defined in agent/episodic.h or memory/deep_memory.h;
 * use the _SQLITE variant for this module. */

typedef struct hu_episode_sqlite {
    int64_t id;
    char contact_id[128];
    char summary[2048];
    size_t summary_len;
    char emotional_arc[256];
    size_t emotional_arc_len;
    char key_moments[2048];
    size_t key_moments_len;
    double impact_score;
    double salience_score;
    int64_t last_reinforced_at;
    char source[32];
    size_t source_len;
    int64_t created_at;
} hu_episode_sqlite_t;

/* Insert an episode. Returns HU_OK and sets *out_id to the new row id. */
hu_error_t hu_episode_store_insert(hu_allocator_t *alloc, void *db, const char *contact_id,
                                  size_t cid_len, const char *summary, size_t sum_len,
                                  const char *emotional_arc, size_t arc_len,
                                  const char *key_moments, size_t km_len, double impact_score,
                                  const char *source, size_t src_len, int64_t *out_id);

/* Get episodes by contact_id, created_at >= since, ordered by created_at DESC, limited. */
hu_error_t hu_episode_get_by_contact(hu_allocator_t *alloc, void *db, const char *contact_id,
                                   size_t cid_len, size_t limit, int64_t since,
                                   hu_episode_sqlite_t **out, size_t *out_count);

/* Associative recall: keyword matching on summary+key_moments. */
hu_error_t hu_episode_associative_recall(hu_allocator_t *alloc, void *db, const char *query,
                                         size_t query_len, const char *contact_id,
                                         size_t cid_len, size_t limit,
                                         hu_episode_sqlite_t **out, size_t *out_count);

/* Reinforce episode: salience_score = MIN(1.0, salience_score+0.1), last_reinforced_at = now. */
hu_error_t hu_episode_reinforce(void *db, int64_t episode_id, int64_t now_ts);

/* Free array of episodes allocated by get_by_contact or associative_recall. */
void hu_episode_free(hu_allocator_t *alloc, hu_episode_sqlite_t *episodes, size_t count);

#endif /* HU_MEMORY_EPISODIC_H */
