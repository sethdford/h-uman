#ifndef HU_AGENT_LEARNED_STYLE_TURN_H
#define HU_AGENT_LEARNED_STYLE_TURN_H

/* Per-turn wiring of the Learned Style Profile (HU_LEARNED_STYLE, default
 * off). Builds the reactive turn's persona head and decides, in one place,
 * what the learned style does to it. Called by BOTH reactive paths:
 * hu_agent_turn (agent_turn_run — production: the provider does not stream,
 * so every reply and the daemon's retry land here) and
 * hu_agent_turn_stream_v2, plus the offline hu_reply_prompt_render.
 *
 *   off    — builds the head exactly as before (no lookup, no log).
 *   shadow — builds the head unchanged; looks up the contact's learned style,
 *            renders the line, COUNTS what LIVE would strip from the persona
 *            rules and the contact profile (no second head build, no RAG),
 *            and logs ONE aggregate line:
 *            "[learned_style shadow] found=.. level=.. shape=.. n=.. p50=..
 *            p90=.. suppressed_rules=.. line_bytes=.. head=.. applied=0" —
 *            no handle, no name, no text.
 *   live   — builds the head with the learned line in and the fixed-length
 *            sentences out, and sets out->live so the prompt builder strips
 *            the contact profile too. Only the head ACTUALLY built counts:
 *            lean (lean == true) or compact (HU_PERSONA_HEAD=live). If the
 *            full head was built, nothing changes and the log says head=full.
 *
 * Eligible turns: reactive (not proactive_turn) with a memory_session_id
 * that resolves to a persona contact. Group chat ids and strangers are not
 * persona contacts, and the learner only measures persona contacts. */

#include "human/agent.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hu_learned_style_turn {
    bool found;              /* a learned style answered this contact */
    bool live;               /* line rendered; prompt must strip contact length rules */
    size_t suppressed_rules; /* sentences LIVE removes (or would) from head + contact */
    size_t line_bytes;
} hu_learned_style_turn_t;

/* `lean` selects hu_agent_build_lean_persona_head (fed `inbound` as its RAG
 * query, as before); otherwise hu_agent_build_persona_head_ex with `topic`.
 * `inbound` is the turn's batch text (shape input, hu_learned_style_shape_inbound).
 * Returns the head builder's error; *ls is always initialised. */
hu_error_t hu_agent_build_head_learned(hu_agent_t *agent, bool lean, const char *topic,
                                       size_t topic_len, const char *inbound, size_t inbound_len,
                                       char **out, size_t *out_len, hu_learned_style_turn_t *ls);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_LEARNED_STYLE_TURN_H */
