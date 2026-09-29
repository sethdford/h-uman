#include "human/daemon/insight_overuse.h"

#include "human/agent/memory_loader.h"
#include "human/core/log.h"
#include "human/core/string.h"
#ifdef HU_ENABLE_SQLITE
#include "human/memory/contact_insights_repo.h"
#endif

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

hu_gate_mode_t hu_insight_overuse_mode(void) {
    return hu_gate_mode_from_env("HU_INSIGHT_OVERUSE", HU_GATE_OFF);
}

/* Words that clear the length bar but carry no memory content. Kept short on
 * purpose: the metric compares the twin against Seth's own rate measured
 * with the SAME list (scripts/insight_overuse_report.py), so the list only
 * has to be stable, not complete. */
static const char *const k_stop[] = {
    "with",  "that",   "this", "have",  "they",     "them",    "their", "about",  "just",
    "like",  "what",   "when", "from",  "will",     "your",    "been",  "were",   "also",
    "into",  "some",   "than", "then",  "there",    "would",   "could", "should", "really",
    "think", "going",  "want", "know",  "dont",     "doesnt",  "didnt", "still",  "because",
    "thing", "things", "week", "today", "tomorrow", "tonight", "yeah",  "okay",
};

static bool is_stop(const char *tok) {
    for (size_t i = 0; i < sizeof(k_stop) / sizeof(k_stop[0]); i++)
        if (strcmp(tok, k_stop[i]) == 0)
            return true;
    return false;
}

#define TOK_MAX   40
#define TOK_LIMIT 256 /* the block is <= HU_INSIGHT_MAX_BYTES; this never binds */

/* Next content token from s[*pos..len): lowercased into tok. Returns false at
 * end. Runs shorter than 4, all-digit runs, and stop words are skipped. */
static bool next_token(const char *s, size_t len, size_t *pos, char *tok, size_t cap) {
    while (*pos < len) {
        while (*pos < len && !(isalnum((unsigned char)s[*pos]) || s[*pos] == '\''))
            (*pos)++;
        size_t start = *pos;
        bool any_alpha = false;
        while (*pos < len && (isalnum((unsigned char)s[*pos]) || s[*pos] == '\'')) {
            if (isalpha((unsigned char)s[*pos]))
                any_alpha = true;
            (*pos)++;
        }
        size_t n = *pos - start;
        if (n < 4 || !any_alpha || n >= cap)
            continue;
        for (size_t i = 0; i < n; i++)
            tok[i] = (char)tolower((unsigned char)s[start + i]);
        tok[n] = '\0';
        if (is_stop(tok))
            continue;
        return true;
    }
    return false;
}

hu_error_t hu_insight_overuse_count(const char *insights, size_t insights_len, const char *inbound,
                                    size_t inbound_len, const char *reply, size_t reply_len,
                                    hu_insight_overuse_t *out) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (!insights || insights_len == 0)
        return HU_OK;
    if ((!inbound && inbound_len) || (!reply && reply_len))
        return HU_ERR_INVALID_ARGUMENT;

    /* Dedupe by remembering every token already counted; the block is small
     * (<= 900 bytes), so a linear scan over prior tokens is fine. */
    char seen[TOK_LIMIT][TOK_MAX];
    size_t n_seen = 0;
    size_t pos = 0;
    char tok[TOK_MAX];
    while (n_seen < TOK_LIMIT && next_token(insights, insights_len, &pos, tok, sizeof(tok))) {
        bool dup = false;
        for (size_t i = 0; i < n_seen && !dup; i++)
            dup = strcmp(seen[i], tok) == 0;
        if (dup)
            continue;
        memcpy(seen[n_seen++], tok, strlen(tok) + 1);
        out->injected++;
        if (reply_len && hu_str_contains_word_ci_n(reply, reply_len, tok)) {
            out->surfaced++;
            if (inbound_len && hu_str_contains_word_ci_n(inbound, inbound_len, tok))
                out->prompted++;
        }
    }
    return HU_OK;
}

hu_error_t hu_daemon_insight_overuse_scan(hu_memory_t *memory, hu_allocator_t *alloc,
                                          const char *contact_id, size_t contact_id_len,
                                          const char *inbound, size_t inbound_len,
                                          const char *reply, size_t reply_len, hu_gate_mode_t mode,
                                          void *observer, hu_insight_overuse_t *out) {
    hu_insight_overuse_t local = {0, 0, 0};
    if (out)
        *out = local;
    if (mode == HU_GATE_OFF)
        return HU_OK;
    if (!memory || !alloc || !contact_id || contact_id_len == 0 || !reply)
        return HU_ERR_INVALID_ARGUMENT;
    /* Nothing was injected unless the insight stream itself is LIVE. */
    if (hu_memory_loader_insight_mode() != HU_GATE_LIVE)
        return HU_OK;

#ifdef HU_ENABLE_SQLITE
    char *lines = NULL;
    size_t lines_len = 0;
    hu_error_t err = hu_contact_insights_render(memory, alloc, contact_id, contact_id_len,
                                                HU_INSIGHT_MAX_ITEMS, HU_INSIGHT_MAX_BYTES,
                                                HU_INSIGHT_MIN_CONFIDENCE, &lines, &lines_len);
    if (err != HU_OK)
        return err;
    if (lines && lines_len) {
        err = hu_insight_overuse_count(lines, lines_len, inbound, inbound_len, reply, reply_len,
                                       &local);
        alloc->free(alloc->ctx, lines, lines_len + 1);
        if (err != HU_OK)
            return err;
    }
    if (out)
        *out = local;
    /* One line per reply; the report script aggregates. "shadow" is literal
     * for both modes: nothing sent changes until the closed loop lands. */
    hu_log_info("insight-overuse", observer,
                "shadow: injected=%zu surfaced=%zu prompted=%zu unprompted=%zu for %.*s",
                local.injected, local.surfaced, local.prompted, local.surfaced - local.prompted,
                (int)(contact_id_len > 24 ? 24 : contact_id_len), contact_id);
    return HU_OK;
#else
    (void)observer;
    (void)inbound;
    (void)inbound_len;
    (void)reply_len;
    return HU_OK; /* no SQLite: the loader injects nothing either */
#endif
}
