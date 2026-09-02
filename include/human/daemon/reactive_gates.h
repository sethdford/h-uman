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

#endif /* HU_DAEMON_REACTIVE_GATES_H */
