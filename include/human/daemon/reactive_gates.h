#ifndef HU_DAEMON_REACTIVE_GATES_H
#define HU_DAEMON_REACTIVE_GATES_H

/* llm_decides gate split.
 *
 * `channels.<ch>.daemon.llm_decides = true` hands the reply decision to the
 * LLM and bypasses the daemon's heuristic classifier gates. Until 2026-09-02
 * that one flag ALSO switched off three gates that are safety, not heuristics:
 * the consecutive-response limiter, the AI-tell retry, and the quality retry.
 * Production runs with llm_decides on, so "I apologize for the delay in
 * responding" reached a real contact with nothing in the way.
 *
 * This module is the single place that says which gates follow the flag and
 * which never do. Pure predicates; src/daemon.c consults them inline. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum hu_reactive_gate {
    /* Heuristic / cost gates: bypassed when llm_decides (LLM decides instead). */
    HU_REACTIVE_GATE_RESPONSE_MODE = 0, /* response_mode selective override */
    HU_REACTIVE_GATE_DROP_OFF,          /* natural conversation drop-off skip */
    HU_REACTIVE_GATE_TAPBACK_SKIP,      /* tapback-worthy → sometimes no reply */
    HU_REACTIVE_GATE_LEAVE_ON_READ,     /* deliberate non-response period */
    HU_REACTIVE_GATE_CONSTITUTIONAL,    /* LLM critique rewrite (extra call) */
    /* Safety gates: ALWAYS active. */
    HU_REACTIVE_GATE_CONSECUTIVE_LIMIT, /* 3 replies in a row → silent */
    HU_REACTIVE_GATE_AI_TELL_RETRY,     /* robotic phrase → one retry */
    HU_REACTIVE_GATE_QUALITY_RETRY,     /* quality score → one retry */
    HU_REACTIVE_GATE__COUNT
} hu_reactive_gate_t;

/* True for the safety gates above. Unknown values are treated as safety
 * (fail closed: an unclassified gate stays on). */
bool hu_reactive_gate_is_safety(hu_reactive_gate_t gate);

/* Should this gate run on the current reply? Safety gates: always. Heuristic
 * gates: only when llm_decides is false. */
bool hu_reactive_gate_active(hu_reactive_gate_t gate, bool llm_decides);

/* AI-tell detector: returns the matched phrase (static string) when the
 * response contains a known assistant-register tell, NULL when clean.
 * Case-insensitive substring match. NULL/empty response → NULL. */
const char *hu_reactive_response_ai_tell(const char *response);

/* What to do with a reply given the AI-tell verdict and whether the one
 * corrective retry has already been spent. A second miss is DROPPED: silence
 * beats sending a therapy-bot line to a friend. */
typedef enum hu_ai_tell_action {
    HU_AI_TELL_SEND = 0, /* no tell: send as-is */
    HU_AI_TELL_RETRY,    /* tell on the first attempt: retry with the hint */
    HU_AI_TELL_DROP,     /* tell after the retry: stay silent */
} hu_ai_tell_action_t;

hu_ai_tell_action_t hu_reactive_ai_tell_action(const char *ai_tell, bool retried);

/* ── Consecutive-reply limiter (rewritten 2026-09-04) ───────────────────
 * `count` replies have gone out to this contact since the real user last
 * stepped in; `cap` is behavior.max_consecutive_replies (0 = no cap). Before
 * checking, the caller resets `count` when hu_reactive_consecutive_burst_expired
 * says our previous reply is older than behavior.consecutive_reset_minutes — a
 * contact coming back after lunch starts a new burst instead of inheriting the
 * morning's silence. Why: on 2026-09-04 a rental agent's four messages, two of
 * them direct questions, went unanswered because three earlier outputs (one a
 * song link) had spent a hard-coded cap of 3 that only Seth typing could reset. */
bool hu_reactive_consecutive_limit_reached(uint32_t count, uint32_t cap);
bool hu_reactive_consecutive_burst_expired(int64_t last_reply_unix, int64_t now_unix,
                                           uint32_t reset_secs);
/* True when the inbound text asks something ('?' anywhere). The limiter logs a
 * silenced question at WARN so it is never invisible again. */
bool hu_reactive_message_is_question(const char *text, size_t len);

#endif /* HU_DAEMON_REACTIVE_GATES_H */
