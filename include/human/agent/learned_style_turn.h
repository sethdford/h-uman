#ifndef HU_AGENT_LEARNED_STYLE_TURN_H
#define HU_AGENT_LEARNED_STYLE_TURN_H

/* Per-turn wiring of the Learned Style Profile (HU_LEARNED_STYLE, default
 * off). Called on the reactive turn right after the persona head is built,
 * before anything is appended to it (agent_stream.c, reply_prompt.c).
 *
 *   off    — returns immediately; head untouched (byte-identical).
 *   shadow — looks up the contact's learned style, renders the line, counts
 *            what LIVE would suppress, logs ONE aggregate line
 *            "[learned_style shadow] found=.. level=.. shape=.. n=.. p50=..
 *            p90=.. suppressed_rules=.. line_bytes=.." — no handle, no name —
 *            and leaves the head untouched.
 *   live   — rebuilds the active head (lean on the llm_decides path, compact
 *            immersive under HU_PERSONA_HEAD=live) with the length-imposing
 *            entries omitted and the learned line rendered, and sets
 *            out->live so the prompt builder strips the length sentences
 *            from the contact profile too. With the FULL head (neither of
 *            the above) LIVE logs head=full and changes nothing.
 *
 * Only known 1:1 contacts (the turn's memory_session_id resolves to a
 * persona contact) are eligible; anything else is a no-op. */

#include "human/agent.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hu_learned_style_turn {
    bool found;              /* a learned style answered this contact */
    bool live;               /* head rebuilt; prompt must strip contact length rules */
    size_t suppressed_rules; /* head + contact entries LIVE omits (or would) */
    size_t line_bytes;
} hu_learned_style_turn_t;

void hu_agent_learned_style_apply(hu_agent_t *agent, const char *msg, size_t msg_len, char **head,
                                  size_t *head_len, hu_learned_style_turn_t *out);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_LEARNED_STYLE_TURN_H */
