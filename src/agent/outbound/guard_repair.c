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
    case HU_GUARD_REPAIR_KEPT_RETRY_TRIMMED:
        return "retry_trimmed";
    case HU_GUARD_REPAIR_KEPT_NONE:
        return "none";
    }
    return "none";
}

/* The guard found nothing wrong with the original except, at most, its
 * length. Any leak, echo, loop or stripped model token makes it unsendable.
 * The logged "repetition_run" is the longest-run statistic, not a violation;
 * a real loop sets detected_degenerate_repetition. */
static bool original_has_no_content_violation(const hu_guard_report_t *r) {
    return r && !r->detected_degenerate_repetition && !r->detected_semantic_leak &&
           !r->detected_director_echo && !r->detected_persona_pii_echo &&
           !r->detected_persona_identity_echo && !r->detected_naked_discourse_opener &&
           !r->detected_deliberation_leak && !r->stripped_harmony_tokens &&
           !r->stripped_thinking_block;
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
    if (have_retry && !hu_reply_is_fragment(retry, retry_len))
        return decision(HU_GUARD_REPAIR_KEPT_RETRY, retry_len, "ok");
    if (!have_retry)
        return decision(HU_GUARD_REPAIR_KEPT_NONE, 0, "retry_failed");

    /* The retry is cut off. The original goes out only when the guard has
     * nothing against it: within the length cap and no other violation. A
     * length-only reject (G5, the context-dump detector) is over the cap by
     * definition, so a dump is never sent, whole or sliced. */
    if (original && original_len > 0 && original_len <= length_cap &&
        original_has_no_content_violation(original_report) &&
        !hu_reply_is_fragment(original, original_len))
        return decision(HU_GUARD_REPAIR_KEPT_ORIGINAL, original_len, "retry_fragment");
    size_t t = hu_reply_trim_to_sentence(retry, retry_len, retry_len);
    if (t > 0 && !hu_reply_is_fragment(retry, t))
        return decision(HU_GUARD_REPAIR_KEPT_RETRY_TRIMMED, t, "retry_fragment");
    size_t k = hu_reply_drop_dangling_tail(retry, retry_len);
    if (k < retry_len && !hu_reply_is_fragment(retry, k))
        return decision(HU_GUARD_REPAIR_KEPT_RETRY_TRIMMED, k, "retry_fragment");
    return decision(HU_GUARD_REPAIR_KEPT_NONE, 0, "retry_fragment");
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
    /* No report / ctx: the caller (validator chain) never keeps the original. */
    size_t cap = (original_report && ctx) ? hu_guard_length_cap(ctx) : 0;
    hu_guard_repair_decision_t d =
        hu_guard_repair_decide(original, original_len, original_report, rt, rl, cap);

    char *out = NULL;
    if (d.kept == HU_GUARD_REPAIR_KEPT_ORIGINAL) {
        out = copy_if_guard_accepts(alloc, original, d.len, ctx);
        if (!out) /* the guard would reject it again (or OOM): never send it */
            d = hu_guard_repair_decide(original, original_len, original_report, rt, rl, 0);
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

static const char *const k_wh[] = {"what", "whats", "what's", "how",  "hows",  "how's", "why",
                                   "when", "where", "who",    "whos", "who's", "which", NULL};
static const char *const k_aux[] = {"did",  "do",  "does",  "are",   "is",   "was",
                                    "were", "can", "could", "would", "will", "should",
                                    "have", "has", "am",    "shall", NULL};
static const char *const k_subj[] = {"i", "you", "we",    "they",  "he",   "she", "it",
                                     "u", "ya",  "y'all", "there", "that", NULL};
static const char *const k_marker[] = {"so",   "ok", "okay", "and", "but", "hey",     "yo",
                                       "wait", "oh", "also", "btw", "hmm", "alright", NULL};

static bool word_in(const char *const *list, const char *w, size_t n) {
    for (size_t i = 0; list[i]; i++)
        if (strlen(list[i]) == n && strncasecmp(list[i], w, n) == 0)
            return true;
    return false;
}

/* Next word (letters + apostrophes) at or after *pos within [*pos, end). */
static size_t next_word(const char *msg, size_t end, size_t *pos) {
    size_t i = *pos;
    while (i < end && isspace((unsigned char)msg[i]))
        i++;
    size_t s = i;
    while (i < end && (isalpha((unsigned char)msg[i]) || msg[i] == '\''))
        i++;
    *pos = s;
    return i - s;
}

/* Does the clause msg[start, end) have interrogative structure: a fronted
 * wh-word ("whats the plan"), or subject-auxiliary inversion ("did you end
 * up", "can you send")? "have fun tonight" has an auxiliary but no inverted
 * subject, so it is not an ask. One leading discourse marker is skipped. */
static bool clause_is_interrogative(const char *msg, size_t start, size_t end) {
    size_t pos = start;
    size_t n = next_word(msg, end, &pos);
    if (n > 0 && word_in(k_marker, msg + pos, n)) {
        pos += n;
        n = next_word(msg, end, &pos);
    }
    if (n == 0)
        return false;
    if (word_in(k_wh, msg + pos, n))
        return true;
    if (!word_in(k_aux, msg + pos, n))
        return false;
    pos += n;
    size_t m = next_word(msg, end, &pos);
    return m > 0 && word_in(k_subj, msg + pos, m);
}

bool hu_guard_inbound_is_ask(const char *msg, size_t len) {
    if (!msg || len == 0)
        return false;
    if (memchr(msg, '?', len))
        return true;
    /* Casual questions drop the "?": check each clause for interrogative
     * structure ("so whats the plan for thanksgiving, walk me through it"). */
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || strchr(".,;!\n", msg[i])) {
            if (clause_is_interrogative(msg, start, i))
                return true;
            start = i + 1;
        }
    }
    return false;
}
