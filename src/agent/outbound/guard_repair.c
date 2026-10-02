/* Response-guard repair policy. See include/human/agent/guard_repair.h. */
#include "human/agent/guard_repair.h"
#include "human/context/reply_fragment.h"
#include "human/core/log.h"
#include "human/core/string.h"

#include <ctype.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

const char *hu_guard_repair_kept_name(hu_guard_repair_kept_t kept) {
    switch (kept) {
    case HU_GUARD_REPAIR_KEPT_RETRY:
        return "retry";
    case HU_GUARD_REPAIR_KEPT_ORIGINAL:
        return "original";
    case HU_GUARD_REPAIR_KEPT_TRIMMED:
        return "trimmed";
    case HU_GUARD_REPAIR_KEPT_RETRY_TRIMMED:
        return "retry_trimmed";
    case HU_GUARD_REPAIR_KEPT_NONE:
        return "none";
    }
    return "none";
}

/* The original's ONLY problem was its length. Anything else the guard found
 * (a leak, an echo, a loop, stripped model tokens) makes it unsendable. The
 * logged "repetition_run" is the longest run statistic, not a violation; a
 * real loop sets detected_degenerate_repetition. */
static bool original_only_too_long(const hu_guard_report_t *r) {
    return r && r->detected_length_anomaly && !r->detected_degenerate_repetition &&
           !r->detected_semantic_leak && !r->detected_director_echo &&
           !r->detected_persona_pii_echo && !r->detected_persona_identity_echo &&
           !r->detected_naked_discourse_opener && !r->detected_deliberation_leak &&
           !r->stripped_harmony_tokens && !r->stripped_thinking_block;
}

static hu_guard_repair_decision_t decision(hu_guard_repair_kept_t kept, size_t len,
                                           const char *reason) {
    hu_guard_repair_decision_t d = {kept, len, reason};
    return d;
}

hu_guard_repair_decision_t hu_guard_repair_decide(const char *original, size_t original_len,
                                                  const hu_guard_report_t *original_report,
                                                  const char *retry, size_t retry_len,
                                                  size_t length_cap) {
    bool have_retry = retry && retry_len > 0;
    bool retry_frag = have_retry && hu_reply_is_fragment(retry, retry_len);
    bool benign = original && original_len > 0 && original_only_too_long(original_report);
    /* Under 40% of a length-only original: the 767 -> 29 collapse. */
    bool collapsed = have_retry && benign && retry_len * 5 < original_len * 2;
    if (have_retry && !retry_frag && !collapsed)
        return decision(HU_GUARD_REPAIR_KEPT_RETRY, retry_len, "ok");
    const char *reason =
        !have_retry ? "retry_failed" : (retry_frag ? "retry_fragment" : "retry_collapsed");

    if (benign) {
        if (original_len <= length_cap && !hu_reply_is_fragment(original, original_len))
            return decision(HU_GUARD_REPAIR_KEPT_ORIGINAL, original_len, reason);
        size_t t = hu_reply_trim_to_sentence(original, original_len, length_cap);
        if (t > 0 && !hu_reply_is_fragment(original, t))
            return decision(HU_GUARD_REPAIR_KEPT_TRIMMED, t, reason);
        /* No sentence end under the cap: a complete-but-short retry still
         * beats an over-cap original the guard would reject again. */
        if (have_retry && !retry_frag)
            return decision(HU_GUARD_REPAIR_KEPT_RETRY, retry_len, reason);
    }
    if (retry_frag) {
        size_t t = hu_reply_trim_to_sentence(retry, retry_len, retry_len);
        if (t > 0 && !hu_reply_is_fragment(retry, t))
            return decision(HU_GUARD_REPAIR_KEPT_RETRY_TRIMMED, t, reason);
        size_t k = hu_reply_drop_dangling_tail(retry, retry_len);
        if (k < retry_len && !hu_reply_is_fragment(retry, k))
            return decision(HU_GUARD_REPAIR_KEPT_RETRY_TRIMMED, k, reason);
    }
    return decision(HU_GUARD_REPAIR_KEPT_NONE, 0, reason);
}

/* A NUL-terminated copy of text[0..len) when the guard passes it untouched
 * under `ctx`, else NULL. */
static char *copy_if_guard_accepts(hu_allocator_t *alloc, const char *text, size_t len,
                                   const hu_guard_context_t *ctx) {
    char *copy = hu_strndup(alloc, text, len);
    if (!copy)
        return NULL;
    char *out = NULL;
    size_t out_len = 0;
    hu_guard_outcome_t oc = HU_GUARD_REJECT;
    hu_guard_report_t rep;
    memset(&rep, 0, sizeof(rep));
    hu_error_t err = hu_response_guard_check_ex(alloc, copy, len, ctx, &out, &out_len, &oc, &rep);
    if (err == HU_OK && oc == HU_GUARD_OK)
        return copy;
    if (err == HU_OK && oc == HU_GUARD_REWROTE && out && out != copy)
        alloc->free(alloc->ctx, out, out_len + 1);
    alloc->free(alloc->ctx, copy, len + 1);
    return NULL;
}

hu_guard_repair_kept_t hu_guard_repair_resolve(hu_allocator_t *alloc, hu_observer_t *obs,
                                               const char *original, size_t original_len,
                                               const hu_guard_report_t *original_report,
                                               const hu_guard_context_t *ctx, char **retry,
                                               size_t *retry_len) {
    if (!alloc || !retry || !retry_len)
        return HU_GUARD_REPAIR_KEPT_RETRY;
    char *rt = *retry;
    size_t rl = rt ? *retry_len : 0;
    size_t cap = hu_guard_length_cap(ctx);
    hu_guard_repair_decision_t d =
        hu_guard_repair_decide(original, original_len, original_report, rt, rl, cap);

    char *out = NULL;
    if (d.kept == HU_GUARD_REPAIR_KEPT_ORIGINAL || d.kept == HU_GUARD_REPAIR_KEPT_TRIMMED) {
        out = copy_if_guard_accepts(alloc, original, d.len, ctx);
        if (!out) {
            /* The guard would reject this text (or OOM): never send it. */
            hu_guard_report_t unsafe;
            memset(&unsafe, 0, sizeof(unsafe));
            unsafe.detected_semantic_leak = true;
            d = hu_guard_repair_decide(original, original_len, &unsafe, rt, rl, cap);
        }
    }
    if (d.kept == HU_GUARD_REPAIR_KEPT_RETRY_TRIMMED)
        out = hu_strndup(alloc, rt, d.len);
    if (d.kept != HU_GUARD_REPAIR_KEPT_RETRY) {
        if (!out && d.kept != HU_GUARD_REPAIR_KEPT_NONE)
            d.kept = HU_GUARD_REPAIR_KEPT_NONE; /* OOM: never fall back to a fragment */
        if (rt)
            alloc->free(alloc->ctx, rt, rl + 1);
        *retry = out;
        *retry_len = out ? d.len : 0;
    }
    hu_log_warn("response_guard", obs,
                "[guard_repair] kept=%s reason=%s orig_len=%zu retry_len=%zu sent_len=%zu",
                hu_guard_repair_kept_name(d.kept), d.reason, original_len, rl, *retry_len);
    return d.kept;
}

/* ── Inbound ask detection ─────────────────────────────────────────────── */

static const char *const k_request_phrases[] = {"walk me through",
                                                "talk me through",
                                                "tell me",
                                                "explain",
                                                "can you",
                                                "could you",
                                                "would you",
                                                "will you",
                                                "please",
                                                "pls",
                                                "plz",
                                                "show me",
                                                "describe",
                                                "remind me",
                                                "help me",
                                                "let me know",
                                                "lmk",
                                                "give me",
                                                "send me",
                                                "fill me in",
                                                "thoughts on",
                                                "what do you think",
                                                NULL};

static const char *const k_question_openers[] = {
    "what",  "whats", "what's", "how",  "hows", "how's", "why", "when", "where",
    "who",   "which", "did",    "do",   "does", "are",   "is",  "can",  "could",
    "would", "will",  "should", "have", "has",  "were",  "was", NULL};

static const char *const k_discourse_markers[] = {"so",  "ok", "okay",    "and",  "but",
                                                  "hey", "yo", "wait",    "also", "btw",
                                                  "hmm", "um", "alright", "oh",   NULL};

static bool word_in(const char *const *list, const char *w, size_t n) {
    for (size_t i = 0; list[i]; i++)
        if (strlen(list[i]) == n && strncasecmp(list[i], w, n) == 0)
            return true;
    return false;
}

/* Next word (letters + apostrophes) at or after *pos; returns its length. */
static size_t next_word(const char *msg, size_t len, size_t *pos) {
    size_t i = *pos;
    while (i < len && !isalpha((unsigned char)msg[i]))
        i++;
    size_t s = i;
    while (i < len && (isalpha((unsigned char)msg[i]) || msg[i] == '\''))
        i++;
    *pos = s;
    return i - s;
}

bool hu_guard_inbound_is_ask(const char *msg, size_t len) {
    if (!msg || len == 0)
        return false;
    if (memchr(msg, '?', len))
        return true;
    for (size_t i = 0; k_request_phrases[i]; i++)
        if (hu_str_contains_word_ci_n(msg, len, k_request_phrases[i]))
            return true;
    /* A question opener as the first word, after at most one discourse
     * marker ("so whats the plan"). Casual questions often drop the "?". */
    size_t pos = 0;
    size_t n = next_word(msg, len, &pos);
    if (n > 0 && word_in(k_discourse_markers, msg + pos, n)) {
        pos += n;
        n = next_word(msg, len, &pos);
    }
    return n > 0 && word_in(k_question_openers, msg + pos, n);
}
