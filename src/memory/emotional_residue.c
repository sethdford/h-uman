typedef int hu_emotional_residue_unused_;

/* Emotional-residue DOMAIN module: only the pure prompt-directive builder lives
 * here. The persistence (add / get_active SQL + the raw sqlite3 handle) was
 * relocated to src/memory/repos/emotional_residue_repo_sqlite.c so this module
 * no longer includes <sqlite3.h> (memory repository pattern; see
 * docs/standards/engineering/bounded-contexts.md + the sqlite-includer ratchet). */

#ifdef HU_ENABLE_SQLITE

#include "human/memory/emotional_residue.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdio.h>
#include <string.h>

char *hu_emotional_residue_build_directive(hu_allocator_t *alloc,
                                           const hu_emotional_residue_t *residues, size_t count,
                                           size_t *out_len) {
    if (!alloc || !residues || count == 0 || !out_len)
        return NULL;
    *out_len = 0;

    const char *sentiment = residues[0].valence >= 0.0 ? "positive" : "negative";
    const char *contact = residues[0].contact_id[0] ? residues[0].contact_id : "contact";
    double intensity = residues[0].intensity;

    char tmp[512];
    int n = snprintf(tmp, sizeof(tmp),
                     "[EMOTIONAL RESIDUE: Recent interaction with [%s] left [%s] weight ([%.2f]). "
                     "Match warmth/caution accordingly.]",
                     contact, sentiment, intensity);
    if (n < 0 || (size_t)n >= sizeof(tmp))
        return NULL;

    size_t need = (size_t)n + 1;
    char *buf = (char *)alloc->alloc(alloc->ctx, need);
    if (!buf)
        return NULL;
    memcpy(buf, tmp, need);
    *out_len = (size_t)n;
    return buf;
}

#else /* !HU_ENABLE_SQLITE */

#include "human/core/allocator.h"
#include "human/memory/emotional_residue.h"

char *hu_emotional_residue_build_directive(hu_allocator_t *alloc,
                                           const hu_emotional_residue_t *residues, size_t count,
                                           size_t *out_len) {
    (void)alloc;
    (void)residues;
    (void)count;
    (void)out_len;
    return NULL;
}

#endif /* HU_ENABLE_SQLITE */
