/* Phase 6 — F68 Protective Intelligence */
#include "human/context/protective.h"
#include "human/core/string.h"
#include <ctype.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE
#include "human/memory.h"
#include <sqlite3.h>
#include <time.h>
#endif

/* Painful keywords: suppress memory surfacing when combined with negative valence or late hour */
static const char *const PAINFUL_KEYWORDS[] = {"death", "divorce", "loss", "funeral", "cancer"};
static const size_t PAINFUL_COUNT = sizeof(PAINFUL_KEYWORDS) / sizeof(PAINFUL_KEYWORDS[0]);

static bool contains_painful_keyword(const char *content, size_t len)
{
    if (!content || len == 0)
        return false;
    for (size_t i = 0; i < PAINFUL_COUNT; i++) {
        const char *kw = PAINFUL_KEYWORDS[i];
        size_t kw_len = strlen(kw);
        if (kw_len > len)
            continue;
        for (size_t j = 0; j <= len - kw_len; j++) {
            bool match = true;
            for (size_t k = 0; k < kw_len && match; k++) {
                if (tolower((unsigned char)content[j + k]) != (unsigned char)kw[k])
                    match = false;
            }
            if (match)
                return true;
        }
    }
    return false;
}

bool hu_protective_memory_ok(hu_allocator_t *alloc, hu_memory_t *memory,
                            const char *contact_id, size_t contact_id_len,
                            const char *memory_content, size_t memory_len,
                            float emotional_valence, int hour_local)
{
    (void)alloc;
    (void)memory;
    (void)contact_id;
    (void)contact_id_len;

    if (!contains_painful_keyword(memory_content, memory_len))
        return true;

    bool late_hour = (hour_local >= 22 || hour_local < 6);
    bool negative_valence = emotional_valence < -0.3f;
    if (late_hour || negative_valence)
        return false;
    return true;
}

#ifdef HU_ENABLE_SQLITE

bool hu_protective_is_boundary(hu_memory_t *memory, const char *contact_id,
                               size_t contact_id_len, const char *topic,
                               size_t topic_len)
{
    if (!memory || !contact_id || contact_id_len == 0 || !topic || topic_len == 0)
        return false;

    sqlite3 *db = hu_sqlite_memory_get_db(memory);
    if (!db)
        return false;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
                               "SELECT 1 FROM boundaries WHERE contact_id=? AND topic=? LIMIT 1",
                               -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return false;

    sqlite3_bind_text(stmt, 1, contact_id, (int)contact_id_len, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, topic, (int)topic_len, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    bool found = (rc == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return found;
}

hu_error_t hu_protective_add_boundary(hu_allocator_t *alloc, hu_memory_t *memory,
                                     const char *contact_id, size_t contact_id_len,
                                     const char *topic, size_t topic_len,
                                     const char *type, const char *source)
{
    (void)alloc;
    if (!memory || !contact_id || contact_id_len == 0 || !topic || topic_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    sqlite3 *db = hu_sqlite_memory_get_db(memory);
    if (!db)
        return HU_ERR_NOT_SUPPORTED;

    const char *t = type ? type : "avoid";
    const char *s = source ? source : "explicit";
    int64_t set_at = (int64_t)time(NULL);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
                               "INSERT INTO boundaries (contact_id, topic, type, set_at, source) "
                               "VALUES (?, ?, ?, ?, ?)",
                               -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(stmt, 1, contact_id, (int)contact_id_len, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, topic, (int)topic_len, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, t, (int)strlen(t), SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 4, set_at);
    sqlite3_bind_text(stmt, 5, s, (int)strlen(s), SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

#else

bool hu_protective_is_boundary(hu_memory_t *memory, const char *contact_id,
                               size_t contact_id_len, const char *topic,
                               size_t topic_len)
{
    (void)memory;
    (void)contact_id;
    (void)contact_id_len;
    (void)topic;
    (void)topic_len;
    return false;
}

hu_error_t hu_protective_add_boundary(hu_allocator_t *alloc, hu_memory_t *memory,
                                     const char *contact_id, size_t contact_id_len,
                                     const char *topic, size_t topic_len,
                                     const char *type, const char *source)
{
    (void)alloc;
    (void)memory;
    (void)contact_id;
    (void)contact_id_len;
    (void)topic;
    (void)topic_len;
    (void)type;
    (void)source;
    return HU_ERR_NOT_SUPPORTED;
}

#endif /* HU_ENABLE_SQLITE */

/* Emotional words that suggest venting (no question = not asking for advice) */
static const char *const VENTING_WORDS[] = {"angry", "frustrated", "upset", "stressed",
                                            "overwhelmed", "exhausted", "terrible", "awful"};
static const size_t VENTING_WORD_COUNT =
    sizeof(VENTING_WORDS) / sizeof(VENTING_WORDS[0]);

static bool looks_like_venting(const char *msg)
{
    if (!msg)
        return false;
    size_t len = strlen(msg);
    if (len == 0)
        return false;
    /* Has question mark = asking for advice, not venting */
    for (size_t i = 0; i < len; i++) {
        if (msg[i] == '?')
            return false;
    }
    /* Check for emotional words (case-insensitive) */
    for (size_t w = 0; w < VENTING_WORD_COUNT; w++) {
        const char *vw = VENTING_WORDS[w];
        size_t vw_len = strlen(vw);
        if (vw_len > len)
            continue;
        for (size_t j = 0; j <= len - vw_len; j++) {
            bool match = true;
            for (size_t k = 0; k < vw_len && match; k++) {
                if (tolower((unsigned char)msg[j + k]) != (unsigned char)vw[k])
                    match = false;
            }
            if (match)
                return true;
        }
    }
    return false;
}

bool hu_protective_advice_ok(const char *const *messages, size_t count)
{
    if (!messages || count < 2)
        return false;

    size_t venting_count = 0;
    for (size_t i = 0; i < count; i++) {
        if (messages[i] && looks_like_venting(messages[i]))
            venting_count++;
    }
    return venting_count >= 2;
}
