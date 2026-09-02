#ifndef HU_MEMORY_PROACTIVE_DECISIONS_REPO_H
#define HU_MEMORY_PROACTIVE_DECISIONS_REPO_H

/*
 * Proactive-decisions repository (SOTA contract C5, Part A) — logs every
 * proactive "should I speak now?" decision the daemon makes, so the
 * When2Speak MIR/FIR measurement (scripts/eval_when_to_speak.py) has a
 * ground-truth record of what the daemon actually decided, not just what
 * it eventually sent.
 *
 * Recall (memory) bounded context. Per
 * ~/.claude/rules/sqlite-includer-ratchet.md this is one of the two legal
 * places (src/memory/repos/, src/memory/engines/) for a raw sqlite3
 * include — domain callers (init_proposer.c, daemon_proactive.c) never see
 * sqlite3 directly, they get a plain `sqlite3 *db` handle from
 * hu_sqlite_memory_get_db() and pass it straight through.
 *
 * Deliberately NOT the fuller vtable+factory shape used by
 * celebration_repo/boundary_repo — this repo has exactly one write path
 * (record) and one read path (count, used by tests + the eval script's
 * sanity check), so a pair of free functions taking `sqlite3 *db` directly
 * is the smallest correct shape. No caller needs to swap backends.
 */

#include "human/core/error.h"
#include <stdint.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Valid `decision` values. Storage is TEXT (for easy ad-hoc SQL from
 * scripts/eval_when_to_speak.py), but production code should use these
 * constants rather than hand-typing the strings. */
#define HU_PROACTIVE_DECISION_SEND    "send"
#define HU_PROACTIVE_DECISION_DECLINE "decline"
#define HU_PROACTIVE_DECISION_DEFER   "defer"

/* Idempotent — creates the proactive_decisions table + its indices if they
 * don't already exist. Safe to call on every write (cheap no-op after the
 * first CREATE TABLE IF NOT EXISTS). */
hu_error_t hu_proactive_decisions_repo_ensure_schema(sqlite3 *db);

/* Insert one decision row.
 *
 *   ts          — unix seconds of the decision (NOT of any later send).
 *   contact     — contact_id/handle this decision concerns; NULL for a
 *                 tick-level (not-yet-per-contact) decision.
 *   trigger     — what subsystem produced this decision, e.g.
 *                 "init_proposer_governor", "init_proposer_llm",
 *                 "proactive_send". Free text, non-NULL.
 *   decision    — MUST be one of HU_PROACTIVE_DECISION_{SEND,DECLINE,DEFER}.
 *                 Any other value is rejected with HU_ERR_INVALID_ARGUMENT
 *                 (per ~/.claude/rules/reports-success-does-nothing.md —
 *                 a typo'd decision string must not silently land as an
 *                 uncategorized row the eval script then miscounts).
 *   reason      — free-text reason (e.g. "quiet_hours", "low_confidence");
 *                 NULL allowed.
 *   sent        — 1 if a message was actually accepted by a channel as a
 *                 direct result of this decision, else 0.
 *   message_ref — free-text reference to the sent message (e.g. a length-
 *                 bounded prefix or a hash); NULL allowed. Never store the
 *                 full message body here — this table is a decision log,
 *                 not a message store, and the fewer copies of message
 *                 content that exist, the smaller the privacy surface.
 */
hu_error_t hu_proactive_decisions_repo_record(sqlite3 *db, int64_t ts, const char *contact,
                                              const char *trigger, const char *decision,
                                              const char *reason, int sent,
                                              const char *message_ref);

/* Total row count — used by tests and by the eval script's sanity check
 * (Part B falls back to production_outcomes/proactive_sends until this
 * table has real rows). */
hu_error_t hu_proactive_decisions_repo_count(sqlite3 *db, int64_t *out_count);

#ifdef __cplusplus
}
#endif

#endif /* HU_ENABLE_SQLITE */

#endif /* HU_MEMORY_PROACTIVE_DECISIONS_REPO_H */
