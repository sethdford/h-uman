/* One-shot: dump the persona system prompt used to drive the blind-A/B direct
 * generator with a byte-faithful prompt. Not part of the build; compiled ad hoc
 * against libhuman_core.a (dev/ASan).
 *
 * Usage:  dump_prompt <persona> <channel> <contact_id|-> <mode>
 *   mode = generic   : hu_persona_build_prompt_compact only (warmth OFF; the
 *                      original harness prompt).
 *   mode = faithful  : generic + the EXACT tone_note logic from the production
 *                      send path (src/agent/agent_turn.c:3072-3083). Fires only
 *                      when relationship_stage ∈ {deep,trusted,familiar} OR
 *                      warmth_level contains "intimate". This is a deliberate
 *                      offline REPLICA of that block for measurement — it is not
 *                      a second production path.
 *   mode = fixed     : generic + the PROPOSED warmth-vocabulary fix: map the
 *                      persona's actual warmth_level vocabulary {high,moderate,low}
 *                      to a warmth tone_note (high→warm/close, moderate→mild,
 *                      low→none). Uses word-boundary matching (substring-classifier
 *                      -pitfalls.md). This is the hypothesis arm.
 *
 * contact_id "-" (or absent) means no contact → behaves like generic regardless
 * of mode. Word matching via hu_str_contains_word_ci (core/string.h), the same
 * primitive render.c uses, so "informal"⊃"formal"-style traps are avoided.
 */
#include "human/core/allocator.h"
#include "human/core/string.h"
#include "human/persona.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Replica of agent_turn.c:3072-3083 — production tone_note selection. */
static const char *faithful_tone_note(const hu_contact_profile_t *cp) {
    if (!cp)
        return NULL;
    if (cp->relationship_stage && strstr(cp->relationship_stage, "deep"))
        return "\n\n[Relationship: deep — be genuinely present, "
               "anticipate needs, use shared references freely.]";
    if (cp->relationship_stage && strstr(cp->relationship_stage, "trusted"))
        return "\n\n[Relationship: trusted — be candid and proactive. "
               "Share insights freely and be direct.]";
    if (cp->relationship_stage && strstr(cp->relationship_stage, "familiar"))
        return "\n\n[Relationship: familiar — reference past conversations "
               "when relevant. Be warmer than default.]";
    if (cp->warmth_level && strstr(cp->warmth_level, "intimate"))
        return "\n\n[Relationship: intimate — respond with genuine warmth "
               "and personal connection. Use inside references.]";
    return NULL;
}

/* Proposed fix: honor the persona's REAL warmth_level vocabulary {high,
 * moderate,low}. relationship_stage still takes precedence (as in production)
 * so this is strictly a superset of the faithful behavior. */
static const char *fixed_tone_note(const hu_contact_profile_t *cp) {
    if (!cp)
        return NULL;
    /* relationship_stage / intimate keep production semantics. */
    const char *base = faithful_tone_note(cp);
    if (base)
        return base;
    if (cp->warmth_level && *cp->warmth_level) {
        /* Word-boundary match to avoid substring traps. */
        if (hu_str_contains_word_ci(cp->warmth_level, "high") ||
            hu_str_contains_word_ci(cp->warmth_level, "warm") ||
            hu_str_contains_word_ci(cp->warmth_level, "close"))
            return "\n\n[Relationship: warm — this is someone you're close to. "
                   "Respond with genuine warmth and ease; inside references and "
                   "affection are welcome.]";
        if (hu_str_contains_word_ci(cp->warmth_level, "moderate"))
            return "\n\n[Relationship: friendly — comfortable and familiar, "
                   "warmer than a stranger but not effusive.]";
        /* low / cold → no warmth note (keep it cool, as conversation.c does). */
    }
    return NULL;
}

int main(int argc, char **argv) {
    const char *name = argc > 1 ? argv[1] : "seth";
    const char *channel = argc > 2 ? argv[2] : "imessage";
    const char *contact_id = (argc > 3 && strcmp(argv[3], "-") != 0) ? argv[3] : NULL;
    const char *mode = argc > 4 ? argv[4] : "generic";

    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p;
    memset(&p, 0, sizeof(p));
    hu_error_t e = hu_persona_load(&alloc, name, strlen(name), &p);
    if (e != HU_OK) {
        fprintf(stderr, "persona_load failed: %d\n", (int)e);
        return 2;
    }

    char *sp = NULL;
    size_t spl = 0;
    e = hu_persona_build_prompt_compact(&alloc, &p, channel, strlen(channel), &sp, &spl);
    if (e != HU_OK || !sp) {
        fprintf(stderr, "build_prompt failed: %d\n", (int)e);
        hu_persona_deinit(&alloc, &p);
        return 3;
    }

    /* Resolve the tone_note for the requested mode + contact. */
    const char *tone = NULL;
    if (contact_id && strcmp(mode, "generic") != 0) {
        const hu_contact_profile_t *cp =
            hu_persona_find_contact(&p, contact_id, strlen(contact_id));
        if (strcmp(mode, "faithful") == 0)
            tone = faithful_tone_note(cp);
        else if (strcmp(mode, "fixed") == 0)
            tone = fixed_tone_note(cp);
        else {
            fprintf(stderr, "unknown mode '%s' (generic|faithful|fixed)\n", mode);
            alloc.free(alloc.ctx, sp, spl + 1);
            hu_persona_deinit(&alloc, &p);
            return 4;
        }
    }

    fwrite(sp, 1, spl, stdout);
    if (tone)
        fwrite(tone, 1, strlen(tone), stdout);

    alloc.free(alloc.ctx, sp, spl + 1);
    hu_persona_deinit(&alloc, &p);
    return 0;
}
