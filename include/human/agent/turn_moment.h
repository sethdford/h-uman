#ifndef HU_AGENT_TURN_MOMENT_H
#define HU_AGENT_TURN_MOMENT_H

/*
 * Per-turn moment cue for hu_agent_turn: the "[moment] ..." fragment that
 * tells the model what time it is for the contact, whether this is the same
 * thread, and whether to greet.
 *
 * The cue is built from the contact's REAL thread (the channel history the
 * daemon hands the agent for the turn), so a greeting is suggested only when
 * there actually was a gap before the message being answered, or the message
 * is the first one in the conversation. Until 2026-10-01 the turn passed NULL
 * history and "never" timestamps, so every reply 05:30-11:00 carried "greet
 * for a fresh morning" and every reply 00:00-05:30 "acknowledge the late-hour
 * gap" (DEF-3, about 228 turns).
 */

#include "human/channel.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hu_agent;
struct hu_persona_t;
struct hu_persona_overlay_t;

/* Render the moment cue from a chronological thread (oldest first, as
 * load_conversation_history returns it; timestamps "YYYY-MM-DD HH:MM[:SS]"
 * local time). The trailing run of inbound entries within 10 minutes of the
 * newest one is the burst being answered: the gap is measured to whatever
 * precedes it. No thread (NULL or empty) means no evidence of a gap or of the
 * first message of the day, so no cue is rendered. Returns the fragment
 * length written into buf (NUL-terminated), 0 when there is no cue. */
size_t hu_turn_moment_render_entries(const struct hu_persona_t *persona,
                                     const struct hu_persona_overlay_t *overlay,
                                     const hu_channel_history_entry_t *entries, size_t count,
                                     int64_t now_s, char *buf, size_t cap);

/* hu_agent_turn's call: the agent's persona, its overlay for the active
 * channel and the turn's thread (agent->ab_history_entries, set by the
 * daemon). Returns the fragment length, 0 for no cue. */
size_t hu_turn_moment_render(const struct hu_agent *agent, int64_t now_s, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_TURN_MOMENT_H */
