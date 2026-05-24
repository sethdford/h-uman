/* Action-layer directive emitters.
 *
 * Spec 2026-05-24-action-layers: turn Spec 3 (self-model) and Spec 4 (TOM)
 * from observe-only / descriptive-only into actually-used: each subsystem
 * gets ONE function here that reads its existing schema and formats a
 * short directive string the daemon appends to the next turn's system
 * prompt.
 *
 * Both functions are stateless: they read from sqlite, format into a
 * caller-provided buffer, and return. No agent-side action that bypasses
 * the LLM — we just inform it.
 *
 * Privacy (AC-AL-4): drift directive uses dimension names + magnitude
 * only; clarify directive uses topic strings + enum only. NEVER any
 * response content, user message text, or tool args.
 *
 * Feature-flag (AC-AL-3): when HU_ENABLE_ACTION_LAYERS is OFF, both
 * functions are stubs that return 0 (no directive emitted). Caller
 * code compiles in both modes without #ifdef. */
#ifndef HU_AGENT_ACTION_DIRECTIVES_H
#define HU_AGENT_ACTION_DIRECTIVES_H

#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#else
struct sqlite3;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Format the self-model drift directive into `out`. Reads the most-recent
 * unresolved row from `agent_self_concerns` (Spec 3 Phase C) with
 * `|magnitude_sigma| >= 2.0` created in the last 7 days. Returns the
 * number of bytes written (excluding the NUL terminator), or 0 when no
 * qualifying concern exists (out becomes empty string).
 *
 * Output shape (≤200 chars per AC-AL-1):
 *   "Recent drift: <dimension> is <±N.N>σ from your baseline. Lean
 *    toward <opposite> in this turn."
 *
 * Safe to call with NULL `db` or empty table (returns 0). Buffer must
 * be ≥ HU_ACTION_DIRECTIVE_MAX_LEN bytes. */
size_t hu_action_directive_drift(struct sqlite3 *db, char *out, size_t out_cap);

/* Format the TOM clarification directive into `out`. Reads unresolved
 * rows from `tom_user_expectations` (Spec 4 Phase A) where the row is
 * "stale enough" — created in a session_key DIFFERENT from
 * `current_session_key` OR created ≥10 minutes ago. Returns bytes
 * written, or 0 when no qualifying expectation exists.
 *
 * Output shape (≤200 chars per AC-AL-2):
 *   "User expects you know about <topic> (<expected_knowledge_type>),
 *    but no recorded belief. Consider asking briefly rather than
 *    guessing."
 *
 * `now_unix_ms` is injected for deterministic testing. */
size_t hu_action_directive_clarify(struct sqlite3 *db, const char *contact_id,
                                   size_t contact_id_len, const char *current_session_key,
                                   size_t current_session_key_len, int64_t now_unix_ms, char *out,
                                   size_t out_cap);

/* Max bytes per directive string (NUL-terminated). Keeps prompt-size
 * inflation bounded (per AC-AL-2 constraint). */
#define HU_ACTION_DIRECTIVE_MAX_LEN 256

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_ACTION_DIRECTIVES_H */
