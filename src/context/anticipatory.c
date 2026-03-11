typedef int hu_anticipatory_unused_;

#ifdef HU_ENABLE_SQLITE

#include "human/context/anticipatory.h"
#include "human/core/string.h"
#include "human/memory.h"
#include <ctype.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HU_ANTICIPATORY_MAX_PREDICTIONS 16
#define HU_ANTICIPATORY_CONFIDENCE_THRESH 0.5f

/* Case-insensitive substring match. */
static bool contains_ci(const char *haystack, size_t hlen, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0 || hlen < nlen)
        return false;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        bool match = true;
        for (size_t j = 0; j < nlen; j++) {
            if (tolower((unsigned char)haystack[i + j]) != tolower((unsigned char)needle[j])) {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }
    return false;
}

/* Keyword → emotion mapping. First match wins. */
typedef struct {
    const char *keyword;
    const char *emotion;
    float confidence;
} keyword_emotion_t;

static const keyword_emotion_t KEYWORD_EMOTIONS[] = {
    {"kid game", "nervous", 0.75f},
    {"kid's game", "nervous", 0.75f},
    {"game tomorrow", "nervous", 0.7f},
    {"game next", "nervous", 0.65f},
    {"game", "nervous", 0.6f},
    {"exam", "stressed", 0.75f},
    {"test tomorrow", "stressed", 0.7f},
    {"interview", "nervous", 0.7f},
    {"birthday", "excited", 0.7f},
    {"loss anniversary", "sad", 0.8f},
    {"anniversary of loss", "sad", 0.8f},
    {"death anniversary", "sad", 0.8f},
    {"died", "sad", 0.65f},
    {"loss", "sad", 0.6f},
    {"anniversary", "sad", 0.55f},
    {"new job", "overwhelmed", 0.7f},
    {"started new job", "overwhelmed", 0.75f},
};
static const size_t KEYWORD_EMOTIONS_COUNT =
    sizeof(KEYWORD_EMOTIONS) / sizeof(KEYWORD_EMOTIONS[0]);

static void match_keyword_to_emotion(const char *text, size_t text_len,
                                    const char **out_emotion, float *out_conf,
                                    char *out_basis, size_t basis_cap) {
    *out_emotion = NULL;
    *out_conf = 0.0f;
    if (out_basis && basis_cap > 0)
        out_basis[0] = '\0';

    if (!text || text_len == 0)
        return;

    for (size_t k = 0; k < KEYWORD_EMOTIONS_COUNT; k++) {
        const char *kw = KEYWORD_EMOTIONS[k].keyword;
        if (contains_ci(text, text_len, kw)) {
            *out_emotion = KEYWORD_EMOTIONS[k].emotion;
            *out_conf = KEYWORD_EMOTIONS[k].confidence;
            size_t copy = text_len < basis_cap - 1 ? text_len : basis_cap - 1;
            if (out_basis && copy > 0) {
                memcpy(out_basis, text, copy);
                out_basis[copy] = '\0';
            }
            return;
        }
    }
}

hu_error_t hu_anticipatory_predict(hu_allocator_t *alloc, hu_memory_t *memory,
                                  const char *contact_id, size_t contact_id_len,
                                  int64_t now_ts, hu_emotional_prediction_t **out,
                                  size_t *out_count) {
    (void)now_ts;
    if (!alloc || !memory || !contact_id || contact_id_len == 0 || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;

    *out = NULL;
    *out_count = 0;

    sqlite3 *db = hu_sqlite_memory_get_db(memory);
    if (!db)
        return HU_ERR_NOT_SUPPORTED;

    /* Query micro_moments for contact. */
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "SELECT fact, significance FROM micro_moments "
                                "WHERE contact_id=? ORDER BY created_at DESC LIMIT 32",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(stmt, 1, contact_id, (int)contact_id_len, SQLITE_STATIC);

    hu_emotional_prediction_t *preds =
        (hu_emotional_prediction_t *)alloc->alloc(alloc->ctx,
                                                  HU_ANTICIPATORY_MAX_PREDICTIONS *
                                                      sizeof(hu_emotional_prediction_t));
    if (!preds) {
        sqlite3_finalize(stmt);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t count = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW && count < HU_ANTICIPATORY_MAX_PREDICTIONS) {
        const char *fact = (const char *)sqlite3_column_text(stmt, 0);
        const char *sig = (const char *)sqlite3_column_text(stmt, 1);
        const char *fact_s = fact ? fact : "";
        const char *sig_s = sig && sig[0] ? sig : "";
        size_t fact_len = strlen(fact_s);
        size_t sig_len = strlen(sig_s);

        /* Combine fact + significance for matching. */
        char combined[512];
        size_t comb_len = 0;
        if (fact_len > 0) {
            size_t copy = fact_len < sizeof(combined) - 1 ? fact_len : sizeof(combined) - 1;
            memcpy(combined, fact_s, copy);
            comb_len = copy;
            combined[comb_len] = '\0';
        }
        if (sig_len > 0 && comb_len < sizeof(combined) - 2) {
            combined[comb_len++] = ' ';
            size_t rem = sizeof(combined) - comb_len - 1;
            size_t copy = sig_len < rem ? sig_len : rem;
            memcpy(combined + comb_len, sig_s, copy);
            comb_len += copy;
            combined[comb_len] = '\0';
        }

        const char *emotion = NULL;
        float conf = 0.0f;
        char basis[64];

        match_keyword_to_emotion(combined, comb_len, &emotion, &conf, basis, sizeof(basis));
        if (!emotion || conf <= 0.0f)
            continue;

        /* Avoid duplicates (same emotion). */
        bool dup = false;
        for (size_t i = 0; i < count; i++) {
            if (strcmp(preds[i].predicted_emotion, emotion) == 0) {
                dup = true;
                break;
            }
        }
        if (dup)
            continue;

        hu_emotional_prediction_t *p = &preds[count];
        memset(p, 0, sizeof(*p));
        size_t cid_copy = contact_id_len < sizeof(p->contact_id) - 1
                             ? contact_id_len
                             : sizeof(p->contact_id) - 1;
        memcpy(p->contact_id, contact_id, cid_copy);
        p->contact_id[cid_copy] = '\0';

        size_t em_copy = strlen(emotion);
        if (em_copy >= sizeof(p->predicted_emotion))
            em_copy = sizeof(p->predicted_emotion) - 1;
        memcpy(p->predicted_emotion, emotion, em_copy);
        p->predicted_emotion[em_copy] = '\0';

        p->confidence = conf;
        size_t basis_len = strlen(basis);
        if (basis_len >= sizeof(p->basis))
            basis_len = sizeof(p->basis) - 1;
        memcpy(p->basis, basis, basis_len);
        p->basis[basis_len] = '\0';

        p->target_date = now_ts;

        /* Store in emotional_predictions table. */
        sqlite3_stmt *ins = NULL;
        rc = sqlite3_prepare_v2(db,
                                "INSERT INTO emotional_predictions(contact_id,predicted_emotion,"
                                "confidence,basis,target_date,verified) VALUES(?,?,?,?,?,0)",
                                -1, &ins, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text(ins, 1, contact_id, (int)contact_id_len, SQLITE_STATIC);
            sqlite3_bind_text(ins, 2, emotion, -1, SQLITE_STATIC);
            sqlite3_bind_double(ins, 3, (double)conf);
            sqlite3_bind_text(ins, 4, basis, -1, SQLITE_STATIC);
            sqlite3_bind_int64(ins, 5, now_ts);
            sqlite3_step(ins);
            sqlite3_finalize(ins);
        }

        count++;
    }
    sqlite3_finalize(stmt);

    if (count == 0) {
        alloc->free(alloc->ctx, preds,
                   HU_ANTICIPATORY_MAX_PREDICTIONS * sizeof(hu_emotional_prediction_t));
        *out = NULL;
        *out_count = 0;
        return HU_OK;
    }

    *out = preds;
    *out_count = count;
    return HU_OK;
}

char *hu_anticipatory_build_directive(hu_allocator_t *alloc,
                                     const hu_emotional_prediction_t *preds, size_t count,
                                     const char *contact_name, size_t name_len,
                                     size_t *out_len) {
    if (!alloc || !preds || !out_len)
        return NULL;
    *out_len = 0;

    const char *name = contact_name && name_len > 0 ? contact_name : "They";
    if (name_len == 0 && contact_name)
        name_len = strlen(contact_name);
    if (name_len > 32)
        name_len = 32;

    char buf[512];
    size_t pos = 0;

    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "[ANTICIPATORY: ");
    if (name_len > 0) {
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "%.*s's ", (int)name_len, name);
    }

    bool first = true;
    for (size_t i = 0; i < count && pos < sizeof(buf) - 64; i++) {
        if (preds[i].confidence <= HU_ANTICIPATORY_CONFIDENCE_THRESH)
            continue;

        if (!first)
            pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, " ");
        first = false;

        if (preds[i].basis[0]) {
            pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "%.*s",
                                   (int)(sizeof(preds[i].basis) - 1), preds[i].basis);
        } else {
            pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "upcoming event");
        }
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, " — they may be %s.",
                               preds[i].predicted_emotion);
    }

    if (first) {
        /* No predictions above threshold. */
        return NULL;
    }

    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, " Consider checking in.]");

    char *result = hu_strndup(alloc, buf, pos);
    if (result)
        *out_len = pos;
    return result;
}

void hu_anticipatory_predictions_free(hu_allocator_t *alloc,
                                     hu_emotional_prediction_t *preds, size_t count) {
    (void)count;
    if (alloc && preds)
        alloc->free(alloc->ctx, preds,
                   HU_ANTICIPATORY_MAX_PREDICTIONS * sizeof(hu_emotional_prediction_t));
}

#else /* !HU_ENABLE_SQLITE */

#include "human/context/anticipatory.h"
#include "human/core/error.h"
#include "human/memory.h"
#include <stddef.h>
#include <string.h>

hu_error_t hu_anticipatory_predict(hu_allocator_t *alloc, hu_memory_t *memory,
                                  const char *contact_id, size_t contact_id_len,
                                  int64_t now_ts, hu_emotional_prediction_t **out,
                                  size_t *out_count) {
    (void)alloc;
    (void)memory;
    (void)contact_id;
    (void)contact_id_len;
    (void)now_ts;
    (void)out;
    (void)out_count;
    return HU_ERR_NOT_SUPPORTED;
}

char *hu_anticipatory_build_directive(hu_allocator_t *alloc,
                                     const hu_emotional_prediction_t *preds, size_t count,
                                     const char *contact_name, size_t name_len,
                                     size_t *out_len) {
    (void)alloc;
    (void)preds;
    (void)count;
    (void)contact_name;
    (void)name_len;
    (void)out_len;
    return NULL;
}

void hu_anticipatory_predictions_free(hu_allocator_t *alloc,
                                     hu_emotional_prediction_t *preds, size_t count) {
    (void)alloc;
    (void)preds;
    (void)count;
}

#endif /* HU_ENABLE_SQLITE */
