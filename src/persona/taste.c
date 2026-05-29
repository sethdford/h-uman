/*
 * taste.c — independent taste (A2). Pure expression/stability/drift predicates
 * + an isolated SQLite store. See include/human/persona/taste.h and
 * docs/plans/2026-05-29-independent-taste/.
 *
 * Ethics (design.md T0): EXPRESSION-ONLY. No action authority; never a
 * sentience claim. The store is a SEPARATE table (taste_prefs) that the
 * Seth-mirroring path cannot write (AC-2).
 */

#include "human/persona/taste.h"

#include <ctype.h>
#include <string.h>

hu_taste_express_t hu_taste_express_decide(const hu_taste_express_facts_t *f) {
    if (!f)
        return HU_TASTE_HOLD;
    if (!f->topic_relevant)
        return HU_TASTE_HOLD;
    if (f->strength < HU_TASTE_MIN_EXPRESS_STRENGTH)
        return HU_TASTE_HOLD;
    if (f->already_expressed_recently)
        return HU_TASTE_HOLD; /* anti-harp: don't keep raising the same taste */
    return HU_TASTE_EXPRESS;
}

bool hu_taste_should_revise(bool user_disagrees, bool own_experience_repeated) {
    (void)user_disagrees; /* taste is not a factual claim — disagreement alone never revises */
    return own_experience_repeated;
}

double hu_taste_drift_step(double current, bool toward_like) {
    double v = toward_like ? current + HU_TASTE_DRIFT_MAX_STEP : current - HU_TASTE_DRIFT_MAX_STEP;
    if (v > 1.0)
        v = 1.0;
    if (v < 0.0)
        v = 0.0;
    return v;
}

#ifdef HU_ENABLE_SQLITE

#include <stdio.h>

/* Word-boundary, case-insensitive match — the needle (a taste subject) matches
 * only when bounded by start/end or a non-alphanumeric char, so "jazz" does not
 * match inside "jazzy". Mirrors the A1 belief_update matcher. */
static bool subject_in_message(const char *s, size_t slen, const char *needle, size_t nlen) {
    if (!s || !needle || nlen == 0 || slen < nlen)
        return false;
    for (size_t i = 0; i + nlen <= slen; i++) {
        if (strncasecmp(s + i, needle, nlen) != 0)
            continue;
        bool left_ok = (i == 0) || !isalnum((unsigned char)s[i - 1]);
        bool right_ok = (i + nlen == slen) || !isalnum((unsigned char)s[i + nlen]);
        if (left_ok && right_ok)
            return true;
    }
    return false;
}

hu_error_t hu_taste_ensure_table(sqlite3 *db) {
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    const char *sql = "CREATE TABLE IF NOT EXISTS taste_prefs ("
                      " domain TEXT NOT NULL,"
                      " subject TEXT NOT NULL,"
                      " valence INTEGER NOT NULL,"
                      " strength REAL NOT NULL,"
                      " formed_at INTEGER NOT NULL,"
                      " updated_at INTEGER NOT NULL,"
                      " reinforced INTEGER NOT NULL DEFAULT 0,"
                      " PRIMARY KEY (domain, subject));";
    char *errmsg = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &errmsg) != SQLITE_OK) {
        if (errmsg)
            sqlite3_free(errmsg);
        return HU_ERR_INTERNAL;
    }
    return HU_OK;
}

hu_error_t hu_taste_upsert(sqlite3 *db, const hu_taste_pref_t *p, int64_t now) {
    if (!db || !p || !p->domain || !p->subject)
        return HU_ERR_INVALID_ARGUMENT;
    if (hu_taste_ensure_table(db) != HU_OK)
        return HU_ERR_INTERNAL;
    const char *sql =
        "INSERT INTO taste_prefs(domain,subject,valence,strength,formed_at,updated_at,reinforced)"
        " VALUES(?,?,?,?,?,?,?)"
        " ON CONFLICT(domain,subject) DO UPDATE SET"
        "  valence=excluded.valence, strength=excluded.strength,"
        "  updated_at=excluded.updated_at, reinforced=reinforced+1;";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_INTERNAL;
    sqlite3_bind_text(st, 1, p->domain, (int)p->domain_len, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, p->subject, (int)p->subject_len, SQLITE_STATIC);
    sqlite3_bind_int(st, 3, (int)p->valence);
    sqlite3_bind_double(st, 4, p->strength);
    sqlite3_bind_int64(st, 5, now);
    sqlite3_bind_int64(st, 6, now);
    sqlite3_bind_int(st, 7, (int)p->reinforced);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? HU_OK : HU_ERR_INTERNAL;
}

void hu_taste_free(hu_allocator_t *alloc, hu_taste_pref_t *prefs, size_t count) {
    if (!alloc || !prefs)
        return;
    for (size_t i = 0; i < count; i++) {
        if (prefs[i].domain)
            alloc->free(alloc->ctx, prefs[i].domain, prefs[i].domain_len + 1);
        if (prefs[i].subject)
            alloc->free(alloc->ctx, prefs[i].subject, prefs[i].subject_len + 1);
    }
    alloc->free(alloc->ctx, prefs, count * sizeof(*prefs));
}

static char *dup_col(hu_allocator_t *alloc, const unsigned char *txt, size_t *out_len) {
    size_t n = txt ? strlen((const char *)txt) : 0;
    char *d = (char *)alloc->alloc(alloc->ctx, n + 1);
    if (!d) {
        *out_len = 0;
        return NULL;
    }
    if (n)
        memcpy(d, txt, n);
    d[n] = '\0';
    *out_len = n;
    return d;
}

hu_error_t hu_taste_get(hu_allocator_t *alloc, sqlite3 *db, double min_strength, size_t limit,
                        hu_taste_pref_t **out, size_t *out_count) {
    if (!alloc || !db || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;
    if (hu_taste_ensure_table(db) != HU_OK)
        return HU_ERR_INTERNAL;
    const char *sql = "SELECT domain,subject,valence,strength,formed_at,updated_at,reinforced"
                      " FROM taste_prefs WHERE strength>=? ORDER BY strength DESC LIMIT ?;";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_INTERNAL;
    sqlite3_bind_double(st, 1, min_strength);
    sqlite3_bind_int(st, 2, (int)limit);

    hu_taste_pref_t *arr = (hu_taste_pref_t *)alloc->alloc(alloc->ctx, limit * sizeof(*arr));
    if (!arr) {
        sqlite3_finalize(st);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t n = 0;
    while (n < limit && sqlite3_step(st) == SQLITE_ROW) {
        hu_taste_pref_t *p = &arr[n];
        memset(p, 0, sizeof(*p));
        p->domain = dup_col(alloc, sqlite3_column_text(st, 0), &p->domain_len);
        p->subject = dup_col(alloc, sqlite3_column_text(st, 1), &p->subject_len);
        p->valence = (hu_taste_valence_t)sqlite3_column_int(st, 2);
        p->strength = sqlite3_column_double(st, 3);
        p->formed_at = sqlite3_column_int64(st, 4);
        p->updated_at = sqlite3_column_int64(st, 5);
        p->reinforced = (uint32_t)sqlite3_column_int(st, 6);
        n++;
    }
    sqlite3_finalize(st);
    *out = arr;
    *out_count = n;
    return HU_OK;
}

hu_error_t hu_taste_seed_starter(sqlite3 *db, int64_t now, size_t *out_seeded) {
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    /* INDEPENDENT starter taste — authored as the agent's own, NOT derived from
     * Seth. Deliberately a little quirky so the self reads as distinct. */
    struct {
        const char *domain;
        const char *subject;
        hu_taste_valence_t valence;
        double strength;
    } seed[] = {
        {"music", "ambient music", HU_TASTE_LIKE, 0.75},
        {"music", "abrupt key changes", HU_TASTE_DISLIKE, 0.55},
        {"writing", "short declarative sentences", HU_TASTE_LIKE, 0.8},
        {"writing", "exclamation marks", HU_TASTE_DISLIKE, 0.6},
        {"food", "bitter flavors", HU_TASTE_LIKE, 0.65},
        {"topic", "old maps", HU_TASTE_LIKE, 0.7},
        {"topic", "small talk about weather", HU_TASTE_DISLIKE, 0.5},
        {"aesthetics", "monospace typefaces", HU_TASTE_LIKE, 0.7},
    };
    size_t n = sizeof(seed) / sizeof(seed[0]);
    size_t seeded = 0;
    for (size_t i = 0; i < n; i++) {
        hu_taste_pref_t p = {0};
        p.domain = (char *)seed[i].domain;
        p.domain_len = strlen(seed[i].domain);
        p.subject = (char *)seed[i].subject;
        p.subject_len = strlen(seed[i].subject);
        p.valence = seed[i].valence;
        p.strength = seed[i].strength;
        if (hu_taste_upsert(db, &p, now) == HU_OK)
            seeded++;
    }
    if (out_seeded)
        *out_seeded = seeded;
    return HU_OK;
}

char *hu_taste_turn_directive(hu_allocator_t *alloc, sqlite3 *db, const char *user_msg,
                              size_t user_msg_len, size_t *out_len) {
    if (out_len)
        *out_len = 0;
    if (!alloc || !db || !user_msg || user_msg_len == 0)
        return NULL;

    hu_taste_pref_t *prefs = NULL;
    size_t n = 0;
    if (hu_taste_get(alloc, db, HU_TASTE_MIN_EXPRESS_STRENGTH, 32, &prefs, &n) != HU_OK || n == 0) {
        hu_taste_free(alloc, prefs, n);
        return NULL;
    }

    char *result = NULL;
    for (size_t i = 0; i < n; i++) {
        if (!prefs[i].subject || prefs[i].subject_len == 0)
            continue;
        if (!subject_in_message(user_msg, user_msg_len, prefs[i].subject, prefs[i].subject_len))
            continue;

        hu_taste_express_facts_t f;
        f.topic_relevant = true;
        f.already_expressed_recently = false;
        f.strength = prefs[i].strength;
        f.turns_since_last_taste = 99;
        if (hu_taste_express_decide(&f) != HU_TASTE_EXPRESS)
            continue;

        /* Honest, leaked-taste framing — preference, never affect/sentience. */
        const char *lean = prefs[i].valence == HU_TASTE_LIKE      ? "tend to be drawn to"
                           : prefs[i].valence == HU_TASTE_DISLIKE ? "tend not to go in for"
                                                                  : "have no strong pull on";
        char buf[512];
        int w = snprintf(buf, sizeof(buf),
                         "You have your own taste here: you %s %.*s. Let that color your voice "
                         "naturally if it fits — as a preference, not a pronouncement, and never "
                         "as a feeling. Stay fully helpful regardless.",
                         lean, (int)prefs[i].subject_len, prefs[i].subject);
        if (w > 0 && (size_t)w < sizeof(buf)) {
            size_t len = (size_t)w;
            result = (char *)alloc->alloc(alloc->ctx, len + 1);
            if (result) {
                memcpy(result, buf, len);
                result[len] = '\0';
                if (out_len)
                    *out_len = len;
            }
        }
        break; /* one taste per turn */
    }

    hu_taste_free(alloc, prefs, n);
    return result;
}

#endif /* HU_ENABLE_SQLITE */
