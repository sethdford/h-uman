/* Emotional-moments DOMAIN module: only the pure topic-selection predicate
 * lives here. The persistence (record / get_due / mark_followed_up SQL + the
 * raw sqlite3 handle) was relocated to
 * src/memory/repos/emotional_moments_repo_sqlite.c so this module no longer
 * includes <sqlite3.h> (memory repository pattern; sqlite-includer ratchet). */
#include "human/memory/emotional_moments.h"
#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* ── Pure topic-selection predicate (P2-1, 2026-05-16 incident) ──────────
 *
 * SAFE topic selection for emotional-moment storage. Replaces the previous
 * daemon.c fallback that copied the raw user message ("combined", up to 255
 * chars) into emotional_moments.topic when primary_topic extraction yielded
 * nothing. That fallback shipped first-person confession fragments to family
 * contacts via F25 (see docs/research/2026-05-16-proactive-audit/findings.md).
 *
 * Contract:
 *   1. primary_topic non-empty, non-whitespace → copy it.
 *   2. else emotion non-empty → copy it.
 *   3. else return 0 → caller MUST skip the record. NEVER fall back to raw
 *      user text. */
size_t hu_emotional_moment_select_topic(const char *primary_topic, const char *emotion, char *out,
                                        size_t out_cap) {
    if (!out || out_cap == 0)
        return 0;
    out[0] = '\0';

    /* Helper inline: is the string non-NULL, non-empty, and non-whitespace? */
    const char *candidates[2] = {primary_topic, emotion};
    for (size_t ci = 0; ci < 2; ci++) {
        const char *s = candidates[ci];
        if (!s)
            continue;
        size_t slen = strlen(s);
        if (slen == 0)
            continue;
        bool all_ws = true;
        for (size_t i = 0; i < slen; i++) {
            if (!isspace((unsigned char)s[i])) {
                all_ws = false;
                break;
            }
        }
        if (all_ws)
            continue;
        size_t copy = (slen < out_cap - 1) ? slen : (out_cap - 1);
        memcpy(out, s, copy);
        out[copy] = '\0';
        return copy;
    }
    return 0;
}
