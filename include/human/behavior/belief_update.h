#ifndef HU_BEHAVIOR_BELIEF_UPDATE_H
#define HU_BEHAVIOR_BELIEF_UPDATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Belief-update decision faculty — "Close the Conviction Loop" (A1).
 *
 * h-uman already holds opinions (memory/evolved_opinions.c), injects them
 * pre-generation (agent_turn.c:2698), expresses disagreement, and resists
 * sycophancy under pushback (behavior/trust.*). The missing piece was the
 * decision to CHANGE a held opinion when genuinely persuaded — without
 * caving to mere reassertion.
 *
 * This module is the pure decision core: given facts about the current
 * turn, decide whether to strengthen, weaken, flip, or leave a stored
 * stance unchanged. It is deliberately free of SQLite and agent state so
 * the full truth table is unit-testable without a database or a turn
 * (see .claude/rules/security-predicate-extraction.md).
 *
 * Spec: docs/plans/2026-05-29-conviction-loop/
 */

typedef enum hu_belief_update {
    HU_BELIEF_NO_CHANGE = 0, /* default — reassertion / no evidence land here */
    HU_BELIEF_STRENGTHEN,    /* new evidence agrees with the held stance */
    HU_BELIEF_WEAKEN,        /* new evidence partially undercuts the stance */
    HU_BELIEF_FLIP           /* new evidence contradicts; adopt opposing stance */
} hu_belief_update_t;

/* Facts the decision is computed from. All derived upstream from existing
 * signals (pressure_history, dialog_act, evolved_opinion lookup) — this
 * struct intentionally contains only plain values, no pointers to mutable
 * state, so the decision is reproducible. */
typedef struct hu_belief_facts {
    bool stance_exists;          /* agent holds a stored stance on this topic */
    bool has_new_evidence;       /* msg carries argument/fact, not bare assertion */
    bool is_reassertion;         /* pressure_history: same claim repeated */
    bool evidence_contradicts;   /* the new evidence opposes the held stance */
    double current_conviction;   /* [0,1] strength of the held stance */
    uint32_t changes_this_convo; /* belief changes already made this conversation */
} hu_belief_facts_t;

/* Maximum belief changes permitted per conversation (matches the cap
 * enforced internally by hu_evolved_opinion_upsert_with_history). */
#define HU_BELIEF_MAX_CHANGES_PER_CONVO 2u

/* Conviction at/above which a contradicting argument WEAKENS rather than
 * FLIPS a stance — strong convictions erode before they snap, preserving
 * the anti-sycophancy spine. */
#define HU_BELIEF_FLIP_CONVICTION_CEIL 0.7

/* The decision. Pure; never allocates; NULL-safe (NULL -> NO_CHANGE). */
hu_belief_update_t hu_belief_update_decide(const hu_belief_facts_t *facts);

/* Heuristic: does the message carry an evidence/argument cue (because,
 * data, study, actually, source, turns out, ...) as opposed to a bare
 * restatement? Pure, deterministic, ASCII case-insensitive, word-boundary
 * aware (avoids matching "factory" for "fact"). Used to derive
 * hu_belief_facts_t.has_new_evidence. NULL/empty -> false. */
bool hu_belief_msg_has_evidence_cue(const char *msg, size_t len);

/* Map a decision + current conviction to the conviction value to store.
 * STRENGTHEN: min(1.0, cur+0.2); WEAKEN: max(0.0, cur-0.2);
 * FLIP: a fresh moderate conviction (0.55) in the new direction;
 * NO_CHANGE: returns cur unchanged. Pure. */
double hu_belief_conviction_for(hu_belief_update_t decision, double current_conviction);

#ifdef HU_ENABLE_SQLITE
#include "human/behavior/pressure_history.h"
#include "human/core/allocator.h"
#include <sqlite3.h>

/*
 * Evaluate a just-finished turn for a possible belief update, and apply it.
 *
 * Scans the agent's held opinions (evolved_opinions), finds the first whose
 * topic the user message references, derives the decision facts from existing
 * signals — evidence cue (hu_belief_msg_has_evidence_cue), contradiction
 * (dialog_act DISAGREEMENT), and reassertion (pressure_history, the
 * anti-sycophancy veto) — and, on a non-NO_CHANGE decision, applies it via
 * hu_evolved_opinion_upsert_with_history (which records an opinion_history row
 * when the stance actually flips).
 *
 * This is the thin, testable integration seam: the daemon post-response site
 * is a single call. `ph` may be NULL (then reassertion is treated as false).
 * On a flip, the shift directive is returned via *out_directive (caller frees
 * via alloc) for next-turn acknowledgement; *out_changed reports whether any
 * update was applied. At most one belief is updated per turn.
 *
 * Returns HU_OK (best-effort; storage failures degrade to no-op).
 */
hu_error_t hu_belief_update_evaluate_turn(hu_allocator_t *alloc, sqlite3 *op_db,
                                          const hu_pressure_history_t *ph, const char *user_msg,
                                          size_t user_msg_len, uint32_t changes_this_convo,
                                          int64_t now_ts, char **out_directive,
                                          size_t *out_directive_len, bool *out_changed);
#endif /* HU_ENABLE_SQLITE */

#endif /* HU_BEHAVIOR_BELIEF_UPDATE_H */
