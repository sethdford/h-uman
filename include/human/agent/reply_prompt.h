#ifndef HU_AGENT_REPLY_PROMPT_H
#define HU_AGENT_REPLY_PROMPT_H

/*
 * Offline rendering of the system prompt the daemon sends for a reactive 1:1
 * reply on the llm_decides path, for a given (persona, channel, contact,
 * incoming message). Nothing is sent and no model is called.
 *
 * Why (2026-09-26): an on-policy preference generator needs "what h-uman would
 * have said here". Harnesses that post `persona show <name> <channel>` to the
 * model measured a different prompt (the full persona head, no contact
 * context, no length limit) than production sends, so their samples were not
 * the policy. This renders the production pieces with the production
 * builders: the lean head (hu_agent_build_lean_persona_head), relationship
 * tone and hard-moment notes (same env gates), the contact profile context,
 * length calibration + honesty check as conversation context, the relational
 * length limit with its measured floor, hu_prompt_build_system on the
 * immersive branch, and hu_agent_finalize_system_prompt.
 *
 * NOT included, because they need live daemon state: memory/graph recall,
 * prospective directive, tapback/reaction context, trust state, replay and
 * community insights, contact style/emotional overlays from memory.db, the
 * time-of-day brief cap and situational hints. Callers must label samples as
 * produced from this approximation.
 */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/persona.h"
#include "human/persona/relationship.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hu_reply_prompt_request {
    hu_persona_t *persona; /* required */
    const char *channel;   /* e.g. "imessage"; required */
    const char *contact;   /* handle as keyed in persona contacts; may be NULL */
    const char *incoming;  /* the inbound batch being answered */
    size_t incoming_len;
    hu_relationship_stage_t stage; /* daemon-wide stage; not observable offline */
    uint32_t channel_max_chars;    /* channel response constraint; 0 = none */
} hu_reply_prompt_request_t;

/* Renders the system prompt into *out (caller frees *out_len + 1 bytes). */
hu_error_t hu_reply_prompt_render(hu_allocator_t *alloc, const hu_reply_prompt_request_t *req,
                                  char **out, size_t *out_len);

/* The max_response_chars the daemon would apply: the relational limit (with the
 * contact's measured reply floor) capped by channel_max_chars. Pure. */
uint32_t hu_reply_prompt_max_chars(const hu_reply_prompt_request_t *req);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_REPLY_PROMPT_H */
