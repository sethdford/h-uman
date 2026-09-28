#ifndef HU_DAEMON_VOICE_FIRST_H
#define HU_DAEMON_VOICE_FIRST_H

/* Voice-first memos (spec docs/superpowers/specs/2026-09-28-voice-first-memos-design.md):
 * decide voice before the reply turn and, LIVE for a contact on the family
 * list (HU_VOICE_DELIVERY_ONLY), have the turn write a memo rather than a
 * text that is later read aloud. HU_VOICE_FIRST=off|shadow|live, default off. */

#include "human/context/voice_decision.h"
#include "human/core/allocator.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct hu_agent;

/* Above the 600-char ceiling where the texting shape rules ("one main point",
 * "don't answer every sub-point") apply — a memo must not follow them. */
#define HU_VOICE_FIRST_MEMO_MAX_CHARS 640

typedef struct {
    bool memo; /* LIVE, decided VOICE, and on the family list: write a memo */
    hu_voice_decision_t decision;
    const char *reason; /* hu_voice_intent_decide's reason, or "off" */
} hu_daemon_voice_first_t;

/* Decides, logs, and records the decision. When `out->memo`, prepends the memo
 * directive to *convo_ctx (reallocated with alloc) and sets *max_chars. */
void hu_daemon_voice_first_prepare(hu_allocator_t *alloc, struct hu_agent *agent,
                                   const char *batch_key, size_t key_len, const char *inbound,
                                   size_t inbound_len, char **convo_ctx, size_t *convo_ctx_len,
                                   uint32_t *max_chars, hu_daemon_voice_first_t *out);

#endif
