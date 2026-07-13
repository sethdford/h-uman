#ifndef HUMAN_DAEMON_DAEMON_SHAPE_H
#define HUMAN_DAEMON_DAEMON_SHAPE_H

#include <stddef.h>
#include <stdint.h>

#include "human/core/allocator.h"

struct hu_persona_overlay;
struct hu_contact_profile;

#ifdef __cplusplus
extern "C" {
#endif

/* Reactive send-path text shaper — the ORDER-DEPENDENT pure mutators, as one
 * testable unit.
 *
 * These calls used to live inline in daemon.c's reactive block under
 * `#ifndef HU_IS_TEST`, so the 13k-test suite never exercised the production
 * shaping ORDER. The 2026-07-12 egress audit traced every incident to that
 * blind spot (e.g. disfluency turning "Wish I could" into "Wish wait no I
 * could"). This orchestrator is compiled into ALL builds so tests see it.
 *
 * Applies, in order:
 *   1. typing quirks     (only when `overlay` carries quirks)
 *   2. stylometric variance / contractions
 *   3. filler injection
 *   4. disfluency         (NO-OP unless HU_DISFLUENCY=live)
 *   5. style governor     (NO-OP unless HU_STYLE_GOVERNOR=shadow/live)
 *
 * NOT included (they carry cross-stage state and stay in daemon.c for a later
 * phase): the typo block (snapshots `original_response` for a follow-up
 * correction) and the F40 inline-reply prefix (needs conversation history).
 *
 * Ordering note: in daemon.c the style governor previously ran AFTER the typo
 * block; lifting it into this unit runs it BEFORE typos. Because the governor
 * is a no-op at its default (OFF), this is byte-identical in production; it
 * differs only in the unshipped HU_STYLE_GOVERNOR=live x occasional-typos edge,
 * where shaping the coherent reply before typos is the intended order.
 *
 * Buffer ownership: `*buf` / `*len` / `*cap` are updated in place. `*cap` is
 * the TOTAL allocated byte count of `*buf` (including the NUL slot). Stages that
 * can grow the text (fillers, disfluency) realloc `*buf` via `alloc` when they
 * need headroom and write the new capacity back through `*cap`; on realloc
 * failure the buffer and capacity are left unchanged (the mutator then simply
 * has no room to inject, matching the pre-extraction behavior). `seed` drives
 * the deterministic stages so the whole composition is reproducible.
 */
void hu_daemon_shape_text_inplace(hu_allocator_t *alloc, char **buf, size_t *len, size_t *cap,
                                  uint32_t seed, const struct hu_persona_overlay *overlay,
                                  const struct hu_contact_profile *contact, const char *formality,
                                  size_t formality_len, const char *channel_name,
                                  size_t channel_name_len, float disfluency_freq);

#ifdef __cplusplus
}
#endif

#endif /* HUMAN_DAEMON_DAEMON_SHAPE_H */
