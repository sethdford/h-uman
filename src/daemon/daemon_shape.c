/* daemon_shape.c — reactive send-path text shaper (see daemon_shape.h).
 *
 * The order-dependent pure mutators of daemon.c's reactive send path, lifted
 * into one unit that is compiled into ALL builds so the test suite exercises
 * the production shaping ORDER (the 2026-07-12 egress-audit blind spot). The
 * calls, the grow logic, and the order are moved verbatim from daemon.c; the
 * only change is that the single `seed` is shared across the deterministic
 * stages (in the daemon these read time(NULL) micro-seconds apart, i.e. the
 * same second) so the composition is reproducible.
 */

#include "human/daemon/daemon_shape.h"

#include "human/agent/style_governor.h"
#include "human/context/conversation.h"
#include "human/persona.h"

/* Ensure *buf can hold at least *len + 16 bytes, mirroring the daemon's inline
 * grow: realloc from the current capacity; on failure leave buffer + capacity
 * unchanged (the mutator then simply has no room to inject). */
static void shape_grow_headroom(hu_allocator_t *alloc, char **buf, size_t len, size_t *cap) {
    if (*cap < len + 16) {
        char *grown = (char *)alloc->realloc(alloc->ctx, *buf, *cap, len + 16);
        if (grown) {
            *buf = grown;
            *cap = len + 16;
        }
    }
}

void hu_daemon_shape_text_inplace(hu_allocator_t *alloc, char **buf, size_t *len, size_t *cap,
                                  uint32_t seed, const struct hu_persona_overlay *overlay,
                                  const struct hu_contact_profile *contact, const char *formality,
                                  size_t formality_len, const char *channel_name,
                                  size_t channel_name_len, float disfluency_freq) {
    if (!alloc || !buf || !*buf || !len || !cap || *len == 0)
        return;

    /* 1. Typing quirks from the persona overlay (in-place shrink). */
    if (overlay && overlay->typing_quirks && overlay->typing_quirks_count > 0) {
        *len = hu_conversation_apply_typing_quirks(
            *buf, *len, (const char *const *)overlay->typing_quirks, overlay->typing_quirks_count);
    }

    /* 2. BTH Tier 3: stylometric variance (contractions) — in-place. */
    *len = hu_conversation_vary_complexity(*buf, *len, seed);

    /* 3. BTH Tier 2: filler injection — may grow. */
    shape_grow_headroom(alloc, buf, *len, cap);
    *len = hu_conversation_apply_fillers(*buf, *len, *cap, seed, channel_name, channel_name_len);

    /* 4. BTH: text disfluency — NO-OP unless HU_DISFLUENCY=live; may grow. */
    shape_grow_headroom(alloc, buf, *len, cap);
    *len = hu_conversation_apply_disfluency(*buf, *len, *cap, seed, disfluency_freq, contact,
                                            formality, formality_len);

    /* 5. Style governor — measured-shape enforcement; NO-OP unless
     * HU_STYLE_GOVERNOR=shadow/live. Only ever shrinks, so no headroom needed. */
    *len = hu_style_governor_apply_inplace(alloc, *buf, *len);
}
