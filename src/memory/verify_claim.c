#include "human/memory/verify_claim.h"
#include "human/core/string.h"
#include "human/memory/consolidation.h"
#include <string.h>
#include <time.h>

/* Claim-language patterns to detect in responses */
static const char *const CLAIM_PATTERNS[] = {
    "I remember when you",
    "I remember you",
    "you told me",
    "you mentioned",
    "you said",
    "last time we",
    "you once told",
    "you shared",
    "we talked about",
    "we discussed",
    "you explained",
    "you described",
    "as you mentioned",
    "from our conversation",
    "I recall you",
    "I recall that you",
    NULL,
};

bool hu_memory_has_claim_language(const char *text, size_t text_len) {
    if (!text || text_len == 0)
        return false;
    for (size_t i = 0; CLAIM_PATTERNS[i] != NULL; i++) {
        if (hu_str_contains_ci_cstr(text, text_len, CLAIM_PATTERNS[i]))
            return true;
    }
    return false;
}

#ifdef HU_ENABLE_SQLITE
#include "human/memory/episodic.h"

hu_error_t hu_memory_verify_claim(hu_allocator_t *alloc, void *db, const char *contact_id,
                                  size_t contact_id_len, const char *claim_text,
                                  size_t claim_text_len, hu_claim_result_t *out) {
    if (!alloc || !db || !claim_text || claim_text_len == 0 || !out)
        return HU_ERR_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));

    /* Search episodes for this contact that match the claim */
    hu_episode_sqlite_t *episodes = NULL;
    size_t ep_count = 0;
    const char *cid = contact_id ? contact_id : "";
    size_t cid_len = contact_id ? contact_id_len : 0;

    hu_error_t err = hu_episode_associative_recall(alloc, (sqlite3 *)db, claim_text, claim_text_len,
                                                   cid, cid_len, 5, &episodes, &ep_count);
    if (err != HU_OK)
        return err;

    if (!episodes || ep_count == 0) {
        out->confidence = 0.0;
        out->has_provenance = false;
        out->contact_match = false;
        out->timestamp_ok = false;
        return HU_OK;
    }

    /* Find the best-matching episode */
    double best_sim = 0.0;
    size_t best_idx = 0;
    for (size_t i = 0; i < ep_count; i++) {
        uint32_t sim = hu_similarity_score(claim_text, claim_text_len, episodes[i].summary,
                                           episodes[i].summary_len);
        double d = (double)sim / 100.0;
        if (d > best_sim) {
            best_sim = d;
            best_idx = i;
        }
        /* Also check key_moments */
        if (episodes[i].key_moments_len > 0) {
            uint32_t km_sim = hu_similarity_score(
                claim_text, claim_text_len, episodes[i].key_moments, episodes[i].key_moments_len);
            double km_d = (double)km_sim / 100.0;
            if (km_d > best_sim) {
                best_sim = km_d;
                best_idx = i;
            }
        }
    }

    out->has_provenance = (best_sim >= 0.15);

    /* Verify contact_id matches */
    if (cid_len > 0 && episodes[best_idx].contact_id[0] != '\0') {
        size_t ep_cid_len = strlen(episodes[best_idx].contact_id);
        out->contact_match =
            (ep_cid_len == cid_len && memcmp(episodes[best_idx].contact_id, cid, cid_len) == 0);
    } else {
        out->contact_match = (cid_len == 0);
    }

    /* Verify timestamp plausibility: episode must not be in the future */
    int64_t now_ts = (int64_t)time(NULL);
    out->timestamp_ok = (episodes[best_idx].created_at <= now_ts);

    /* Compute overall confidence */
    double conf = best_sim;
    if (!out->contact_match)
        conf *= 0.1; /* severe penalty for wrong contact */
    if (!out->timestamp_ok)
        conf *= 0.5;
    out->confidence = conf > 1.0 ? 1.0 : conf;

    hu_episode_free(alloc, episodes, ep_count);
    return HU_OK;
}

#else

hu_error_t hu_memory_verify_claim(hu_allocator_t *alloc, void *db, const char *contact_id,
                                  size_t contact_id_len, const char *claim_text,
                                  size_t claim_text_len, hu_claim_result_t *out) {
    (void)alloc;
    (void)db;
    (void)contact_id;
    (void)contact_id_len;
    (void)claim_text;
    (void)claim_text_len;
    (void)out;
    return HU_ERR_NOT_SUPPORTED;
}

#endif /* HU_ENABLE_SQLITE */

hu_error_t hu_memory_hedge_claim(hu_allocator_t *alloc, const char *text, size_t text_len,
                                 char **out, size_t *out_len) {
    if (!alloc || !text || text_len == 0 || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    /* Prefix with hedging disclaimer */
    const char *hedge = "I think ";
    size_t hedge_len = 8;

    /* Replace strong claim patterns with hedged versions */
    /* For simplicity, prepend a hedge if claim language is found,
     * and replace "I remember" with "I think I remember" */
    size_t max_len = text_len + 64;
    char *buf = (char *)alloc->alloc(alloc->ctx, max_len + 1);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    size_t pos = 0;
    size_t i = 0;
    bool hedged = false;

    while (i < text_len && pos < max_len - 1) {
        /* Check for "I remember" pattern */
        if (!hedged && i + 10 <= text_len && hu_str_contains_ci_cstr(text + i, 10, "I remember")) {
            const char *replacement = "I think I remember";
            size_t rep_len = 18;
            if (pos + rep_len < max_len) {
                memcpy(buf + pos, replacement, rep_len);
                pos += rep_len;
                i += 10; /* skip "I remember" */
                hedged = true;
                continue;
            }
        }
        /* Check for "you told me" */
        if (!hedged && i + 11 <= text_len && hu_str_contains_ci_cstr(text + i, 11, "you told me")) {
            const char *replacement = "I believe you told me";
            size_t rep_len = 21;
            if (pos + rep_len < max_len) {
                memcpy(buf + pos, replacement, rep_len);
                pos += rep_len;
                i += 11;
                hedged = true;
                continue;
            }
        }
        buf[pos++] = text[i++];
    }

    /* If no specific pattern was hedged, prepend the hedge */
    if (!hedged && pos > 0) {
        size_t total = hedge_len + pos;
        if (total <= max_len) {
            memmove(buf + hedge_len, buf, pos);
            memcpy(buf, hedge, hedge_len);
            pos = total;
        }
    }

    buf[pos] = '\0';
    *out = buf;
    *out_len = pos;
    return HU_OK;
}
