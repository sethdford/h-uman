#ifndef HU_MEMORY_LIFECYCLE_ROLLOUT_H
#define HU_MEMORY_LIFECYCLE_ROLLOUT_H

#include <stdbool.h>
#include <stddef.h>

/* Rollout mode for hybrid (keyword + vector) adoption */
typedef enum hu_rollout_mode {
    HU_ROLLOUT_OFF,
    HU_ROLLOUT_SHADOW,
    HU_ROLLOUT_CANARY,
    HU_ROLLOUT_ON,
} hu_rollout_mode_t;

/* Decision for a given session */
typedef enum hu_rollout_decision {
    HU_ROLLOUT_KEYWORD_ONLY,
    HU_ROLLOUT_HYBRID,
    HU_ROLLOUT_SHADOW_HYBRID,
} hu_rollout_decision_t;

typedef struct hu_rollout_policy {
    hu_rollout_mode_t mode;
    unsigned canary_percent; /* 0-100 */
    unsigned shadow_percent; /* 0-100 */
} hu_rollout_policy_t;

/* Parse rollout mode from string. Unknown/empty -> HU_ROLLOUT_OFF */
hu_rollout_mode_t hu_rollout_mode_from_string(const char *s, size_t len);

/* Initialize policy from config-like values. Clamps percents to 100. */
hu_rollout_policy_t hu_rollout_policy_init(const char *rollout_mode, size_t mode_len,
                                           unsigned canary_hybrid_percent,
                                           unsigned shadow_hybrid_percent);

/* Decide retrieval strategy for a session. session_id NULL or empty -> keyword_only in canary. */
hu_rollout_decision_t hu_rollout_decide(const hu_rollout_policy_t *policy, const char *session_id,
                                        size_t session_id_len);

#endif /* HU_MEMORY_LIFECYCLE_ROLLOUT_H */
