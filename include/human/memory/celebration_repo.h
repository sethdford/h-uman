#ifndef HU_MEMORY_CELEBRATION_REPO_H
#define HU_MEMORY_CELEBRATION_REPO_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h" /* hu_memory_t */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Celebration repository (B1c) — remembers which wins h-uman has already
 * celebrated for a contact, so it never re-celebrates the same thing and
 * never spams. Recall (memory) bounded context.
 *
 * This is the DDD-clean persistence pattern (mirrors boundary_repo): domain
 * code (the prosocial producers) depends ONLY on this interface — it never
 * sees sqlite3. The SQL + raw handle live in
 * src/memory/repos/celebration_repo_sqlite.c, the one layer where that is
 * legal (per .claude/rules/sqlite-includer-ratchet.md). Establishing this
 * pattern here also pays down the inline-SQL debt the A-series carried.
 *
 * Spec: docs/plans/2026-05-29-prosocial-uplift/
 */

/* Pure domain value object — no storage detail leaks. `win_key` is a caller-
 * supplied fingerprint of the specific win (e.g. a normalized snippet/hash). */
typedef struct hu_celebration {
    const char *contact_id;
    size_t contact_id_len;
    const char *win_key;
    size_t win_key_len;
    int kind; /* hu_win_kind_t, stored as int to keep the contexts decoupled */
    int64_t celebrated_at;
} hu_celebration_t;

struct hu_celebration_repo_vtable;
typedef struct hu_celebration_repo {
    void *ctx;
    const struct hu_celebration_repo_vtable *vtable;
} hu_celebration_repo_t;

typedef struct hu_celebration_repo_vtable {
    /* True if (contact, win_key) was celebrated within `window_secs` of `now`. */
    hu_error_t (*was_recent)(void *ctx, const char *contact_id, size_t contact_id_len,
                             const char *win_key, size_t win_key_len, int64_t now,
                             int64_t window_secs, bool *out);
    /* Record a celebration (idempotent on contact+win_key — updates timestamp). */
    hu_error_t (*record)(void *ctx, const hu_celebration_t *c);
    void (*deinit)(void *ctx);
} hu_celebration_repo_vtable_t;

/* Factory: the ONLY entry point domain code uses. Returns a sqlite-backed repo
 * when `mem` is sqlite; HU_ERR_NOT_SUPPORTED for non-SQL backends. Caller owns
 * *out and must call out->vtable->deinit. */
hu_error_t hu_celebration_repo_create(hu_memory_t *mem, hu_allocator_t *alloc,
                                      hu_celebration_repo_t *out);

#endif /* HU_MEMORY_CELEBRATION_REPO_H */
