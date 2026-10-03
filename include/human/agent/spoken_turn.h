#ifndef HU_AGENT_SPOKEN_TURN_H
#define HU_AGENT_SPOKEN_TURN_H

/* Latency-first prompt profile for spoken turns.
 *
 * A voice turn has a few seconds, not the ~44 s a texting turn takes today. When
 * agent->spoken_turn is set (it implies lean_prompt), the turn loads fewer
 * memories, shows fewer persona examples, and adds a directive to answer the way
 * a person speaks. The skipped critique/constitutional round-trips come with
 * lean_prompt.
 *
 * Gate: HU_SPOKEN_TURN = off | shadow | live (default off), read by the gateway.
 * Set it only in the environment of a gateway process dedicated to voice: the
 * dashboard's voice and text chat share one bus channel, so the gateway applies the
 * profile to every turn in that process. */

#include "human/core/gate_mode.h"
#include <stdbool.h>
#include <stddef.h>

#define HU_SPOKEN_TURN_MEMORY_ENTRIES 4
#define HU_SPOKEN_TURN_MEMORY_CHARS   1500
#define HU_SPOKEN_TURN_EXAMPLES       2

/* Text-turn defaults the profile replaces (the long-standing values). */
#define HU_TEXT_MEMORY_ENTRIES 10
#define HU_TEXT_MEMORY_CHARS   4000
#define HU_TEXT_EXAMPLES       5

#define HU_SPOKEN_TURN_DIRECTIVE                                                             \
    "\nThis is a spoken voice conversation. Answer in one or two short sentences you would " \
    "say out loud: no emoji, lists, links, or anything that only works in text.\n"

hu_gate_mode_t hu_spoken_turn_mode(void);

void hu_spoken_turn_memory_caps(bool voice, size_t *max_entries, size_t *max_chars);

/* Whether the turn may use semantic (embedding) recall. The query embedding is a network
 * call to the embedder (~0.5 s measured on a live voice turn), paid before the model is
 * even asked; spoken turns use the store's keyword recall instead. */
bool hu_spoken_turn_wants_semantic_recall(bool voice);

size_t hu_spoken_turn_example_cap(bool voice);

/* Saved agent state for one turn; hu_spoken_turn_restore puts it back. */
typedef struct hu_spoken_turn_saved {
    bool lean_prompt;
    bool spoken_turn;
} hu_spoken_turn_saved_t;

struct hu_agent;

/* Live: sets lean_prompt and spoken_turn for this turn. Off/shadow: leaves the
 * agent alone. Always records the prior values in *saved. */
void hu_spoken_turn_begin(struct hu_agent *agent, hu_gate_mode_t mode,
                          hu_spoken_turn_saved_t *saved);

void hu_spoken_turn_end(struct hu_agent *agent, const hu_spoken_turn_saved_t *saved);

#endif /* HU_AGENT_SPOKEN_TURN_H */
