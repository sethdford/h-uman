#ifndef SC_MEMORY_LIFECYCLE_ROLLOUT_H
#define SC_MEMORY_LIFECYCLE_ROLLOUT_H

#include <stdbool.h>
#include <stddef.h>

/* Rollout mode for hybrid (keyword + vector) adoption */
typedef enum sc_rollout_mode {
    SC_ROLLOUT_OFF,
    SC_ROLLOUT_SHADOW,
    SC_ROLLOUT_CANARY,
    SC_ROLLOUT_ON,
} sc_rollout_mode_t;

/* Decision for a given session */
typedef enum sc_rollout_decision {
    SC_ROLLOUT_KEYWORD_ONLY,
    SC_ROLLOUT_HYBRID,
    SC_ROLLOUT_SHADOW_HYBRID,
} sc_rollout_decision_t;

typedef struct sc_rollout_policy {
    sc_rollout_mode_t mode;
    unsigned canary_percent;   /* 0-100 */
    unsigned shadow_percent;   /* 0-100 */
} sc_rollout_policy_t;

/* Parse rollout mode from string. Unknown/empty -> SC_ROLLOUT_OFF */
sc_rollout_mode_t sc_rollout_mode_from_string(const char *s, size_t len);

/* Initialize policy from config-like values. Clamps percents to 100. */
sc_rollout_policy_t sc_rollout_policy_init(const char *rollout_mode, size_t mode_len,
    unsigned canary_hybrid_percent, unsigned shadow_hybrid_percent);

/* Decide retrieval strategy for a session. session_id NULL or empty -> keyword_only in canary. */
sc_rollout_decision_t sc_rollout_decide(const sc_rollout_policy_t *policy,
    const char *session_id, size_t session_id_len);

#endif /* SC_MEMORY_LIFECYCLE_ROLLOUT_H */
