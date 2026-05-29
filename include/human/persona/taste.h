#ifndef HU_PERSONA_TASTE_H
#define HU_PERSONA_TASTE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Independent taste — "A Self That Isn't You" (A2).
 *
 * h-uman currently clones Seth's style (persona/style_clone.c et al.) and
 * stores Seth's preferences (agent/preferences.c). This module gives the agent
 * preferences OF ITS OWN — likes/dislikes seeded independently and never
 * written by the Seth-mirroring path — that leak into how it talks.
 *
 * Like the A1 conviction loop, the decision/expression logic is a set of pure
 * predicates (no SQLite, no agent state) so the behaviour is unit-testable
 * without a database; persistence is a separate SQLite-gated store.
 *
 * Ethics (design.md T0): taste is EXPRESSION-ONLY. It has no action authority —
 * it never selects tools, changes recommendations, overrides the user, or
 * claims sentience. It only colours the agent's own voice.
 *
 * Spec: docs/plans/2026-05-29-independent-taste/
 */

typedef enum hu_taste_valence {
    HU_TASTE_DISLIKE = -1,
    HU_TASTE_NEUTRAL = 0,
    HU_TASTE_LIKE = 1
} hu_taste_valence_t;

typedef struct hu_taste_pref {
    char *domain; /* "music" | "writing" | "food" | "topic" | ... */
    size_t domain_len;
    char *subject; /* "ambient music", "long emails", "spicy food" */
    size_t subject_len;
    hu_taste_valence_t valence;
    double strength;     /* [0,1] how strongly held */
    int64_t formed_at;   /* unix seconds */
    int64_t updated_at;  /* unix seconds */
    uint32_t reinforced; /* times own experience reinforced it */
} hu_taste_pref_t;

/* ── Pure decision/expression layer (no SQLite) ─────────────────────────── */

typedef enum hu_taste_express { HU_TASTE_HOLD = 0, HU_TASTE_EXPRESS } hu_taste_express_t;

/* Facts the expression decision is computed from. */
typedef struct hu_taste_express_facts {
    bool topic_relevant;             /* the turn touches this pref's subject */
    bool already_expressed_recently; /* anti-harp: surfaced in last few turns */
    double strength;                 /* [0,1] of the held pref */
    uint32_t turns_since_last_taste; /* any taste surfaced recently? */
} hu_taste_express_facts_t;

/* Decide whether to let a held taste leak into this turn. Pure; NULL -> HOLD.
 * EXPRESS only when relevant, strong enough, and not repetitive. */
hu_taste_express_t hu_taste_express_decide(const hu_taste_express_facts_t *facts);

/* Strength below which a pref is too weak to ever surface. */
#define HU_TASTE_MIN_EXPRESS_STRENGTH 0.4

/* AC-4: taste is NOT a factual claim and does NOT yield to "evidence" /
 * disagreement the way A1 opinions do. Returns true ONLY for accumulated
 * own-experience (reinforcement), never for mere user disagreement. Pure. */
bool hu_taste_should_revise(bool user_disagrees, bool own_experience_repeated);

/* AC-5: one rate-limited, direction-coherent drift step. Returns the new
 * strength; movement is capped per step (no whiplash) and clamped to [0,1].
 * `toward_like` nudges up, else down. Pure. */
double hu_taste_drift_step(double current_strength, bool toward_like);

/* Max strength change per drift step (slow, coherent maturation). */
#define HU_TASTE_DRIFT_MAX_STEP 0.1

#ifdef HU_ENABLE_SQLITE
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <sqlite3.h>

hu_error_t hu_taste_ensure_table(sqlite3 *db);

/* Upsert one pref keyed on (domain, subject). */
hu_error_t hu_taste_upsert(sqlite3 *db, const hu_taste_pref_t *p, int64_t now);

/* Fetch prefs with strength >= min_strength, up to `limit`. Caller frees via
 * hu_taste_free. */
hu_error_t hu_taste_get(hu_allocator_t *alloc, sqlite3 *db, double min_strength, size_t limit,
                        hu_taste_pref_t **out, size_t *out_count);

void hu_taste_free(hu_allocator_t *alloc, hu_taste_pref_t *prefs, size_t count);

/* Seed the INDEPENDENT starter taste profile (not derived from Seth). Idempotent
 * via upsert. Returns the number of prefs seeded. */
hu_error_t hu_taste_seed_starter(sqlite3 *db, int64_t now, size_t *out_seeded);

/* Build a turn directive that lets a relevant held taste leak into the voice,
 * if the user message references a held subject and it clears the express gate.
 * Returns an allocated directive (caller frees via alloc) or NULL. The directive
 * is framed as leaked taste, never as affect or a sentience claim (AC-6). */
char *hu_taste_turn_directive(hu_allocator_t *alloc, sqlite3 *db, const char *user_msg,
                              size_t user_msg_len, size_t *out_len);
#endif /* HU_ENABLE_SQLITE */

#endif /* HU_PERSONA_TASTE_H */
