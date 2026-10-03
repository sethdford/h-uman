/* src/agent/turn/spoken_turn.c — latency-first prompt caps for spoken turns.
 * See include/human/agent/spoken_turn.h. */
#include "human/agent/spoken_turn.h"
#include "human/agent.h"

hu_gate_mode_t hu_spoken_turn_mode(void) {
    return hu_gate_mode_from_env("HU_SPOKEN_TURN", HU_GATE_OFF);
}

void hu_spoken_turn_memory_caps(bool voice, size_t *max_entries, size_t *max_chars) {
    if (max_entries)
        *max_entries = voice ? HU_SPOKEN_TURN_MEMORY_ENTRIES : HU_TEXT_MEMORY_ENTRIES;
    if (max_chars)
        *max_chars = voice ? HU_SPOKEN_TURN_MEMORY_CHARS : HU_TEXT_MEMORY_CHARS;
}

size_t hu_spoken_turn_example_cap(bool voice) {
    return voice ? HU_SPOKEN_TURN_EXAMPLES : HU_TEXT_EXAMPLES;
}

bool hu_spoken_turn_wants_app_context(bool voice) {
    return !voice;
}

void hu_spoken_turn_begin(struct hu_agent *agent, hu_gate_mode_t mode,
                          hu_spoken_turn_saved_t *saved) {
    if (!agent || !saved)
        return;
    saved->lean_prompt = agent->lean_prompt;
    saved->spoken_turn = agent->spoken_turn;
    if (mode == HU_GATE_LIVE) {
        agent->lean_prompt = true;
        agent->spoken_turn = true;
    }
}

void hu_spoken_turn_end(struct hu_agent *agent, const hu_spoken_turn_saved_t *saved) {
    if (!agent || !saved)
        return;
    agent->lean_prompt = saved->lean_prompt;
    agent->spoken_turn = saved->spoken_turn;
}
