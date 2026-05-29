/* Phase 6 — F68 Protective Intelligence */
#include "human/context/protective.h"
#include "human/core/string.h"
#include <ctype.h>
#include <string.h>

#ifdef HU_ENABLE_SQLITE
#include "human/memory/boundary_repo.h"
#include <time.h>
#endif

/* Painful keywords: suppress memory surfacing when combined with negative valence or late hour */
static const char *const PAINFUL_KEYWORDS[] = {"death", "divorce", "loss", "funeral", "cancer"};
static const size_t PAINFUL_COUNT = sizeof(PAINFUL_KEYWORDS) / sizeof(PAINFUL_KEYWORDS[0]);

static bool contains_painful_keyword(const char *content, size_t len) {
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

bool hu_protective_memory_ok(hu_allocator_t *alloc, hu_memory_t *memory, const char *contact_id,
                             size_t contact_id_len, const char *memory_content, size_t memory_len,
                             float emotional_valence, int hour_local) {
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

bool hu_protective_is_boundary(hu_memory_t *memory, const char *contact_id, size_t contact_id_len,
                               const char *topic, size_t topic_len) {
    if (!memory || !contact_id || contact_id_len == 0 || !topic || topic_len == 0)
        return false;

    hu_allocator_t alloc = hu_system_allocator();
    hu_boundary_repo_t repo;
    hu_error_t err = hu_boundary_repo_create(memory, &alloc, &repo);
    /* Fail CLOSED: if we cannot determine boundary state (repo unavailable or
     * lookup error), treat the topic AS a boundary. A protective guard must
     * not fail open on transient storage failures (deny-by-default). */
    if (err != HU_OK)
        return true;

    bool is_b = false;
    err = repo.vtable->is_boundary(repo.ctx, contact_id, contact_id_len, topic, topic_len, &is_b);
    repo.vtable->deinit(repo.ctx);
    return (err == HU_OK) ? is_b : true;
}

hu_error_t hu_protective_add_boundary(hu_allocator_t *alloc, hu_memory_t *memory,
                                      const char *contact_id, size_t contact_id_len,
                                      const char *topic, size_t topic_len, const char *type,
                                      const char *source) {
    if (!memory || !contact_id || contact_id_len == 0 || !topic || topic_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    hu_boundary_repo_t repo;
    hu_error_t err = hu_boundary_repo_create(memory, alloc, &repo);
    if (err != HU_OK)
        return err;

    const char *t = type ? type : "avoid";
    const char *s = source ? source : "explicit";
    int64_t set_at = (int64_t)time(NULL);

    hu_boundary_t b = {.contact_id = contact_id,
                       .contact_id_len = contact_id_len,
                       .topic = topic,
                       .topic_len = topic_len,
                       .type = t,
                       .type_len = strlen(t),
                       .source = s,
                       .source_len = strlen(s),
                       .created_at = set_at};

    err = repo.vtable->add(repo.ctx, &b);
    repo.vtable->deinit(repo.ctx);
    return err;
}

#else

bool hu_protective_is_boundary(hu_memory_t *memory, const char *contact_id, size_t contact_id_len,
                               const char *topic, size_t topic_len) {
    (void)memory;
    (void)contact_id;
    (void)contact_id_len;
    (void)topic;
    (void)topic_len;
    return false;
}

hu_error_t hu_protective_add_boundary(hu_allocator_t *alloc, hu_memory_t *memory,
                                      const char *contact_id, size_t contact_id_len,
                                      const char *topic, size_t topic_len, const char *type,
                                      const char *source) {
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
static const char *const VENTING_WORDS[] = {"angry",       "frustrated", "upset",    "stressed",
                                            "overwhelmed", "exhausted",  "terrible", "awful"};
static const size_t VENTING_WORD_COUNT = sizeof(VENTING_WORDS) / sizeof(VENTING_WORDS[0]);

static bool looks_like_venting(const char *msg) {
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

bool hu_protective_advice_ok(const char *const *messages, size_t count) {
    if (!messages || count < 2)
        return false;

    size_t venting_count = 0;
    for (size_t i = 0; i < count; i++) {
        if (messages[i] && looks_like_venting(messages[i]))
            venting_count++;
    }
    return venting_count >= 2;
}
