#ifndef HU_DAEMON_VOICE_FIRST_H
#define HU_DAEMON_VOICE_FIRST_H

/* Voice-first memos (spec docs/superpowers/specs/2026-09-28-voice-first-memos-design.md):
 * decide voice before the reply turn and, LIVE for a contact on the family
 * list (HU_VOICE_DELIVERY_ONLY), have the turn write a memo rather than a
 * text that is later read aloud. HU_VOICE_FIRST=off|shadow|live, default off. */

#include "human/channel.h"
#include "human/context/voice_decision.h"
#include "human/core/allocator.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct hu_agent;
struct hu_reactive_turn_ctx;

/* Above the 600-char ceiling where the texting shape rules ("one main point",
 * "don't answer every sub-point") apply — a memo must not follow them. */
#define HU_VOICE_FIRST_MEMO_MAX_CHARS 640

typedef struct {
    bool memo; /* LIVE, decided VOICE, and on the family list: write a memo */
    hu_voice_decision_t decision;
    const char *reason; /* hu_voice_intent_decide's reason, or "off" */
} hu_daemon_voice_first_t;

/* Decides, logs, and records the decision. When `out->memo`, prepends the memo
 * directive to *convo_ctx (reallocated with alloc) and sets *max_chars. `rt`
 * (may be NULL) supplies the turn's channel history for the v2 reconnect
 * trigger (HU_VOICE_TRIGGERS_V2); *max_chars on entry is the turn's planned
 * reply budget. */
void hu_daemon_voice_first_prepare(hu_allocator_t *alloc, struct hu_agent *agent,
                                   const char *batch_key, size_t key_len, bool is_group, bool force,
                                   const char *inbound, size_t inbound_len, char **convo_ctx,
                                   size_t *convo_ctx_len, uint32_t *max_chars,
                                   const struct hu_reactive_turn_ctx *rt,
                                   hu_daemon_voice_first_t *out);

/* Seconds from the owner's newest message in `history` ("YYYY-MM-DD HH:MM:SS"
 * local, chat.db's format) to `now`; -1 when there is none. */
int64_t hu_daemon_voice_first_secs_since_owner_reply(const hu_channel_history_entry_t *history,
                                                     size_t count, int64_t now);

/* Would voice-first LIVE send this contact a memo if the moment called for
 * one? (LIVE, on the family list, not a group, a voice configured.) */
bool hu_daemon_voice_first_available(struct hu_agent *agent, const char *batch_key, size_t key_len,
                                     bool is_group);

/* hu_daemon_voice_reply's voice_first: 0 none, a memo turn, or an owner #voice
 * self-test (spoken whatever its length). */
#define HU_VOICE_FIRST_MEMO   1
#define HU_VOICE_FIRST_FORCED 2

/* The reason hu_daemon_voice_reply logs for a memo turn (trigger 'voice_reply').
 * A #voice self-test is logged as HU_VOICE_SELF_TEST_REASON so it never starts
 * the per-contact spacing gap (it is not a memo anyone received). */
#define HU_VOICE_SELF_TEST_REASON "self_test"
const char *hu_daemon_voice_first_reply_reason(int voice_first);

/* On a memo turn, the director's cue (usually a texting length like "one line")
 * becomes "Voice memo, ...: <its objective>" so the memo length wins. */
void hu_daemon_voice_first_direction(char *direction, size_t cap);

#endif
