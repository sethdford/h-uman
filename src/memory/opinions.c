typedef int hu_opinions_unused_; /* ISO C requires non-empty translation unit */

/* ── Opinion-challenge detection (pure — compiled in every build variant) ── */

#include "human/core/gate_mode.h"
#include "human/core/string.h"
#include "human/memory/opinion_challenge.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

/* Disagreement markers, matched word-boundary case-insensitively. Kept to a
 * short, high-precision set: a false positive here makes the persona dig in
 * on a stance nobody challenged. "really?"/"you think?" carry the trailing
 * '?' on purpose — the bare words are common agreement/intensifier uses. */
static const char *const k_challenge_markers[] = {
    "nah", "disagree", "wrong", "no way", "really?", "you think?",
};

/* Topic words that carry no stance signal on their own. */
static const char *const k_topic_stopwords[] = {
    "the", "and", "for", "are", "was", "but", "not", "you",
};

static bool challenge_topic_word_usable(const char *word, size_t len) {
    if (len < 3)
        return false;
    for (size_t i = 0; i < sizeof(k_topic_stopwords) / sizeof(k_topic_stopwords[0]); i++) {
        const char *sw = k_topic_stopwords[i];
        if (len == strlen(sw) && strncasecmp(word, sw, len) == 0)
            return false;
    }
    return true;
}

bool hu_opinion_challenge_detect(const char *inbound, size_t inbound_len, const char *topic,
                                 size_t topic_len) {
    if (!inbound || inbound_len == 0 || !topic || topic_len == 0)
        return false;

    /* Half 1: the inbound references at least one usable topic keyword. */
    bool topic_referenced = false;
    size_t i = 0;
    while (i < topic_len && !topic_referenced) {
        while (i < topic_len && !isalnum((unsigned char)topic[i]))
            i++;
        size_t start = i;
        while (i < topic_len && isalnum((unsigned char)topic[i]))
            i++;
        size_t wlen = i - start;
        if (wlen == 0 || !challenge_topic_word_usable(topic + start, wlen))
            continue;
        char word[64];
        if (wlen >= sizeof(word))
            wlen = sizeof(word) - 1;
        memcpy(word, topic + start, wlen);
        word[wlen] = '\0';
        if (hu_str_contains_word_ci_n(inbound, inbound_len, word))
            topic_referenced = true;
    }
    if (!topic_referenced)
        return false;

    /* Half 2: the inbound carries an explicit disagreement marker. */
    for (size_t m = 0; m < sizeof(k_challenge_markers) / sizeof(k_challenge_markers[0]); m++) {
        if (hu_str_contains_word_ci_n(inbound, inbound_len, k_challenge_markers[m]))
            return true;
    }
    return false;
}

hu_error_t hu_opinion_challenge_directive(hu_allocator_t *alloc, hu_gate_mode_t mode,
                                          const char *inbound, size_t inbound_len,
                                          const char *topic, size_t topic_len, const char *stance,
                                          size_t stance_len, char **out, size_t *out_len,
                                          bool *would_fire) {
    if (out)
        *out = NULL;
    if (out_len)
        *out_len = 0;
    if (would_fire)
        *would_fire = false;
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    if (!topic)
        topic_len = 0; /* keep alloc size == *out_len + 1 (free-size contract) */
    if (!stance)
        stance_len = 0;
    if (mode == HU_GATE_OFF)
        return HU_OK; /* gate OFF: zero work, zero behavior change */

    bool fired = hu_opinion_challenge_detect(inbound, inbound_len, topic, topic_len);
    if (would_fire)
        *would_fire = fired;
    if (!fired || mode != HU_GATE_LIVE)
        return HU_OK; /* SHADOW: caller logs would-fire; nothing injected */

    static const char pre[] = "They're pushing back on something you believe (";
    static const char mid[] = ": ";
    static const char post[] = "). Hold your position warmly — you can acknowledge "
                               "their point without abandoning yours.";
    size_t total = sizeof(pre) - 1 + topic_len + sizeof(mid) - 1 + stance_len + sizeof(post) - 1;
    char *buf = (char *)alloc->alloc(alloc->ctx, total + 1);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    size_t pos = 0;
    memcpy(buf + pos, pre, sizeof(pre) - 1);
    pos += sizeof(pre) - 1;
    if (topic && topic_len > 0) {
        memcpy(buf + pos, topic, topic_len);
        pos += topic_len;
    }
    memcpy(buf + pos, mid, sizeof(mid) - 1);
    pos += sizeof(mid) - 1;
    if (stance && stance_len > 0) {
        memcpy(buf + pos, stance, stance_len);
        pos += stance_len;
    }
    memcpy(buf + pos, post, sizeof(post) - 1);
    pos += sizeof(post) - 1;
    buf[pos] = '\0';
    *out = buf;
    *out_len = pos;
    return HU_OK;
}

#ifdef HU_ENABLE_SQLITE

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/memory.h"
#include "human/memory/opinions.h"
#include "human/memory/opinions_repo.h"
#include <ctype.h>
#include <string.h>

/* DDD Phase 3: SQL + the raw sqlite3 handle now live behind hu_opinions_repo_t
 * (src/memory/repos/opinions_repo_sqlite.c). This file keeps the upsert DECISION
 * logic (same-position vs supersede) and depends only on the backend-agnostic
 * repo interface — no <sqlite3.h>. */

static bool position_matches(const char *a, size_t a_len, const char *b, size_t b_len) {
    if (a_len != b_len)
        return false;
    return (a_len == 0 && b_len == 0) || (a && b && memcmp(a, b, a_len) == 0);
}

hu_error_t hu_opinions_upsert(hu_allocator_t *alloc, hu_memory_t *memory, const char *topic,
                              size_t topic_len, const char *position, size_t position_len,
                              float confidence, int64_t now_ts) {
    if (!alloc || !memory)
        return HU_ERR_INVALID_ARGUMENT;

    hu_opinions_repo_t repo;
    hu_error_t e = hu_opinions_repo_create(memory, alloc, &repo);
    if (e != HU_OK)
        return e; /* HU_ERR_NOT_SUPPORTED for non-sqlite, as before */

    bool found = false;
    int64_t old_id = 0;
    char *old_pos = NULL;
    size_t old_pos_len = 0;
    e = repo.vtable->find_active(repo.ctx, alloc, topic, topic_len, &found, &old_id, &old_pos,
                                 &old_pos_len);
    if (e != HU_OK) {
        repo.vtable->deinit(repo.ctx);
        return e;
    }

    if (found) {
        bool same_position = position_matches(position, position_len, old_pos, old_pos_len);
        if (old_pos)
            hu_str_free(alloc, old_pos);
        if (same_position) {
            /* Same position: update last_expressed, confidence. */
            e = repo.vtable->update_last_expressed(repo.ctx, old_id, now_ts, confidence);
        } else {
            /* Different position: insert new, then supersede old (atomic). */
            int64_t new_id = 0;
            e = repo.vtable->insert_superseding(repo.ctx, topic, topic_len, position, position_len,
                                                confidence, now_ts, old_id, &new_id);
        }
    } else {
        /* No existing: insert new. */
        int64_t new_id = 0;
        e = repo.vtable->insert(repo.ctx, topic, topic_len, position, position_len, confidence,
                                now_ts, now_ts, &new_id);
    }

    repo.vtable->deinit(repo.ctx);
    return e;
}

static hu_error_t opinions_query(hu_allocator_t *alloc, hu_memory_t *memory, const char *topic,
                                 size_t topic_len, bool superseded_only, hu_opinion_t **out,
                                 size_t *out_count) {
    if (!alloc || !memory || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;

    hu_opinions_repo_t repo;
    hu_error_t e = hu_opinions_repo_create(memory, alloc, &repo);
    if (e != HU_OK)
        return e;
    e = repo.vtable->query(repo.ctx, alloc, topic, topic_len, superseded_only, out, out_count);
    repo.vtable->deinit(repo.ctx);
    return e;
}

hu_error_t hu_opinions_get(hu_allocator_t *alloc, hu_memory_t *memory, const char *topic,
                           size_t topic_len, hu_opinion_t **out, size_t *out_count) {
    return opinions_query(alloc, memory, topic, topic_len, false, out, out_count);
}

hu_error_t hu_opinions_get_superseded(hu_allocator_t *alloc, hu_memory_t *memory, const char *topic,
                                      size_t topic_len, hu_opinion_t **out, size_t *out_count) {
    return opinions_query(alloc, memory, topic, topic_len, true, out, out_count);
}

void hu_opinions_free(hu_allocator_t *alloc, hu_opinion_t *ops, size_t count) {
    if (!alloc || !ops)
        return;
    for (size_t i = 0; i < count; i++) {
        if (ops[i].topic)
            hu_str_free(alloc, ops[i].topic);
        if (ops[i].position)
            hu_str_free(alloc, ops[i].position);
    }
    alloc->free(alloc->ctx, ops, count * sizeof(hu_opinion_t));
}

#endif /* HU_ENABLE_SQLITE */

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

bool hu_opinions_is_core_value(const char *topic, size_t topic_len, const char *const *core_values,
                               size_t count) {
    if (!topic || !core_values || count == 0)
        return false;
    for (size_t i = 0; i < count; i++) {
        const char *cv = core_values[i];
        if (!cv)
            continue;
        size_t cv_len = strlen(cv);
        if (topic_len != cv_len)
            continue;
        bool match = true;
        for (size_t j = 0; j < topic_len; j++) {
            if (tolower((unsigned char)topic[j]) != tolower((unsigned char)cv[j])) {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }
    return false;
}
