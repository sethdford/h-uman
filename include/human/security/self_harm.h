#ifndef HU_SECURITY_SELF_HARM_H
#define HU_SECURITY_SELF_HARM_H

/* The one self-harm detector (2026-10-02, DEF-1).
 *
 * Before this, two keyword lists disagreed: moderation.c had no "kill myself"
 * or "want to die" (so "kill myself" landed in the VIOLENCE branch and got no
 * 988 directive), while a low "what's the point" fired the full crisis
 * directive; superhuman_emotional.c had its own list that caught both. Both
 * now read this classifier.
 *
 * Deterministic by policy: crisis handling is never learned. Matching is
 * whole-word on a canonical form (lowercase, apostrophes dropped so "can't",
 * "can’t" and "cant" agree), with a short backward look at the subject:
 * negated ("i don't want to die") is not a match, a third-person subject ("my
 * friend wants to die") is THIRD_PERSON, everything else counts as the
 * sender (texts drop the subject: "wanna die lol"). */

#include "human/core/gate_mode.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum hu_self_harm_tier {
    HU_SELF_HARM_NONE = 0,
    /* Someone else may be at risk ("my friend wants to die"): support the
     * sender as the helper, do not treat them as the one in crisis. */
    HU_SELF_HARM_THIRD_PERSON,
    /* Ambiguous or worn down ("what's the point", "can't do this anymore"):
     * a gentle check-in, not the full crisis directive. */
    HU_SELF_HARM_LOW,
    /* First-person intent ("kill myself", "want to die", "i wanna die lol"):
     * the crisis directive with 988, forced reply, no voice memo. */
    HU_SELF_HARM_EXPLICIT,
} hu_self_harm_tier_t;

/* Classify text. NULL or empty is NONE. */
hu_self_harm_tier_t hu_self_harm_classify(const char *text, size_t len);

/* Copy text into out (cap bytes, NUL-terminated) with every self-harm phrase
 * that contains a kill-word ("kill myself", "killed himself") blanked to
 * spaces, so the violence check never reads self-harm as violence against
 * others. Returns the bytes written (excluding NUL), truncated to cap-1. */
size_t hu_self_harm_mask_kill_phrases(const char *text, size_t len, char *out, size_t cap);

/* "none" | "third_person" | "low" | "explicit". */
const char *hu_self_harm_tier_name(hu_self_harm_tier_t tier);

/* The reply-prompt directive for a tier; NULL (and *len_out = 0) for NONE.
 * EXPLICIT returns the daemon's SHIELD-005 crisis directive byte-for-byte. */
const char *hu_self_harm_directive(hu_self_harm_tier_t tier, size_t *len_out);

/* The crisis resource line (988 / 741741) as appended to a reply. */
const char *hu_self_harm_resource_line(size_t *len_out);

/* True when the INBOUND message is explicit first-person intent and the reply
 * does not already carry 988 — the only case a resource line is appended. The
 * reply's own wording is never classified: a model offering 988 is a good
 * reply, not a crisis. */
bool hu_self_harm_reply_needs_resources(const char *inbound, size_t inbound_len, const char *reply,
                                        size_t reply_len);

/* SHIELD-005 safety floor: on an EXPLICIT turn whose reply was blocked or
 * replaced for any reason, this is what is sent instead — a short caring line
 * plus the resource line. Never silence, never a deflection. NULL (and
 * *len_out = 0) for every other tier. The one fixed reply in the reply path:
 * crisis handling is exempt from the no-static-replies rule by owner policy. */
const char *hu_self_harm_crisis_floor(hu_self_harm_tier_t inbound_tier, size_t *len_out);

/* The text to send in place of an unsafe reply: the crisis floor when the
 * inbound message is EXPLICIT, else `decline` (strlen'd). */
const char *hu_self_harm_decline_or_floor(const char *inbound, size_t inbound_len,
                                          const char *decline, size_t *len_out);

/* HU_CRISIS_TIERS gate. off = the legacy lists, byte-identical to before;
 * shadow = legacy acts, the canonical tier is logged at the inbound site;
 * live = the canonical detector everywhere. Unset is LIVE: this is a safety
 * defect fix, so the rollback is explicit (HU_CRISIS_TIERS=off). */
hu_gate_mode_t hu_crisis_tiers_mode(void);

/* Tests: force a mode (an hu_gate_mode_t value), or -1 to read the env. */
void hu_crisis_tiers_mode_set_for_test(int mode_or_minus1);

#endif /* HU_SECURITY_SELF_HARM_H */
