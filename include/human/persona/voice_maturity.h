#ifndef HU_VOICE_MATURITY_H
#define HU_VOICE_MATURITY_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum hu_voice_stage {
    HU_VOICE_FORMAL,   /* new contact, professional */
    HU_VOICE_WARM,     /* established rapport */
    HU_VOICE_CANDID,   /* trusted relationship */
    HU_VOICE_INTIMATE, /* deep familiarity */
} hu_voice_stage_t;

typedef struct hu_voice_profile {
    hu_voice_stage_t stage;
    uint32_t interaction_count;
    uint32_t shared_topics;
    uint32_t emotional_exchanges;
    float warmth_score;           /* 0.0-1.0 */
    float humor_allowance;        /* 0.0-1.0 */
    float vulnerability_level;    /* 0.0-1.0 */
    hu_voice_stage_t floor_stage; /* known-relationship floor; updates never drop below it */
} hu_voice_profile_t;

struct hu_persona;

/* Initialize a voice profile for a new contact */
void hu_voice_profile_init(hu_voice_profile_t *profile);

/* Update profile after an interaction */
void hu_voice_profile_update(hu_voice_profile_t *profile, bool had_emotional_content,
                             bool had_shared_topic, bool had_humor);

/* Build prompt guidance for current voice maturity */
hu_error_t hu_voice_build_guidance(const hu_voice_profile_t *profile, hu_allocator_t *alloc,
                                   char **out, size_t *out_len);

/* Determine stage from interaction metrics */
hu_voice_stage_t hu_voice_compute_stage(uint32_t interactions, uint32_t emotional_exchanges,
                                        float warmth);

/* Score vulnerability signals from message content.
 * Returns 0.0-1.0 based on emotional/personal/vulnerable language. */
float hu_voice_vulnerability_from_content(const char *text, size_t text_len);

/* Apply time decay to vulnerability between sessions.
 * hours_elapsed: hours since last interaction.
 * Decays vulnerability_level toward 0 at a rate proportional to elapsed time. */
void hu_voice_vulnerability_decay(hu_voice_profile_t *profile, float hours_elapsed);

/* Build a stage-specific directive string for injection into the LLM prompt.
 * Writes into out_buf (NUL-terminated, truncated if necessary).
 * Returns the number of bytes written (excluding NUL), or 0 on error.
 * This is the enforcement-layer companion to hu_voice_build_guidance: it emits
 * a concise directive tag suitable for inclusion alongside mood_ctx/somatic_ctx. */
size_t hu_voice_maturity_build_directive(hu_voice_stage_t stage, char *out_buf, size_t buf_len);

/* Stage a known relationship starts at: family, partners and close friends
 * INTIMATE; friend CANDID; casual / professional_friend / coworker /
 * acquaintance WARM; anything else (incl. "test", unknown) FORMAL. Exact,
 * case- and separator-insensitive match ("close_friend" == "Close Friend");
 * never a substring match, so "professional_friend" is not "friend". Pure. */
hu_voice_stage_t hu_voice_stage_floor_for_relationship(const char *rel, size_t rel_len);

/* Raise the profile to at least `floor` (stage, warmth, humor,
 * vulnerability) and keep it there through later updates. */
void hu_voice_profile_apply_floor(hu_voice_profile_t *profile, hu_voice_stage_t floor);

/* Gated by HU_VOICE_RELATIONSHIP_FLOOR (off/shadow/live, default off): LIVE
 * applies the floor of the contact's persona relationship (falling back to
 * relationship_type); SHADOW logs once and changes nothing. */
void hu_voice_profile_apply_relationship(hu_voice_profile_t *profile,
                                         const struct hu_persona *persona, const char *contact_id,
                                         size_t contact_id_len);

#endif
