/* include/human/persona/relationship_tone.h — relationship-based tone note.
 *
 * Pure predicate (security-predicate-extraction shape): maps a contact
 * profile to the tone-note string appended to the persona prompt, or NULL
 * when no note applies. Extracted from the inline chain in agent_turn.c so
 * the truth table is testable without building a full agent.
 *
 * Two vocabularies:
 *   - Legacy relationship_stage vocabulary {deep, trusted, familiar} plus
 *     warmth_level containing "intimate" — always honored (production
 *     behavior since the block shipped).
 *   - The persona's REAL warmth_level vocabulary {high/warm/close,
 *     moderate, low} — honored only when `warmth_vocab_enabled` is true.
 *     Measured 2026-07-11 (tools_dump_prompt.c): across all real corpus
 *     contacts the legacy vocabulary NEVER fires (stage/intimate values
 *     don't occur in real personas), so the legacy-only note is dead code
 *     in practice; the warmth vocabulary fires for every contact.
 *
 * Activation of the warmth vocabulary is env-gated in agent_turn.c
 * (HU_WARMTH_TONE_VOCAB: live/shadow/off) per
 * .claude/rules/feature-gate-requires-measurement.md.
 */
#ifndef HUMAN_PERSONA_RELATIONSHIP_TONE_H
#define HUMAN_PERSONA_RELATIONSHIP_TONE_H

#include "human/persona.h"

#include <stdbool.h>

/* Returns a static tone-note string ("\n\n[Relationship: ...]") or NULL.
 * Legacy stage/intimate vocabulary takes precedence; the {high, moderate,
 * low} warmth vocabulary is consulted only when warmth_vocab_enabled and
 * the legacy vocabulary produced nothing — the result is a strict superset
 * of legacy behavior. low/cold warmth intentionally yields NULL. */
const char *hu_persona_relationship_tone_note(const hu_contact_profile_t *cp,
                                              bool warmth_vocab_enabled);

#endif /* HUMAN_PERSONA_RELATIONSHIP_TONE_H */
