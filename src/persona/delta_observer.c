#include "human/persona/delta_observer.h"
#include "human/persona/persona_deltas.h"

#ifdef HU_ENABLE_LEARNING
#include "human/ml/learner_bridge.h"
#endif

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* High-precision pattern matcher for explicit user corrections. We deliberately
 * keep the table small: every accidental false positive becomes a persona delta,
 * and the daily evolver applies anything that crosses the apply_threshold with
 * enough corroboration. We err on the side of missing matches over inventing
 * them.
 *
 * Adversarial protection comes from three layers:
 *   1. Patterns require explicit imperative wording ("be more X", "stop saying
 *      X"). They do not fire on third-party narration ("she said be more
 *      formal" -> doesn't start a clause cleanly).
 *   2. Confidence is set just below the apply_threshold (0.7 vs 0.75), so a
 *      single match never auto-applies; the consumer requires corroboration.
 *   3. The propose layer rate-limits the same source to 10 per hour. */

#define HU_DELTA_OBS_SOURCE "user-explicit-correction"
#define HU_DELTA_OBS_DEFAULT_CONFIDENCE 0.70f

/* Case-insensitive substring search over [hay, hay+hay_len). Returns the
 * offset of the first byte of the match, or SIZE_MAX if not found. */
static size_t find_ci(const char *hay, size_t hay_len, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0 || nlen > hay_len)
        return (size_t)-1;
    for (size_t i = 0; i + nlen <= hay_len; i++) {
        size_t j = 0;
        for (; j < nlen; j++) {
            unsigned char a = (unsigned char)hay[i + j];
            unsigned char b = (unsigned char)needle[j];
            if (tolower(a) != tolower(b))
                break;
        }
        if (j == nlen)
            return i;
    }
    return (size_t)-1;
}

/* True when `pos` in `hay` looks like the start of an imperative clause:
 * pos == 0, or the byte before pos is a whitespace, sentence terminator, or
 * comma -- AND we don't see a third-person speech verb in the immediately
 * preceding ~24 chars (filters narrative like "she said be more formal" or
 * "they told me to be less wordy"). */
static bool is_clause_start(const char *hay, size_t pos) {
    if (pos == 0)
        return true;
    char c = hay[pos - 1];
    bool boundary = c == ' ' || c == '\t' || c == '\n' || c == '.' || c == '!' || c == '?' ||
                    c == ',' || c == ';' || c == ':' || c == '"' || c == '\'';
    if (!boundary)
        return false;
    /* Look back up to 24 chars for narration markers. */
    static const char *narrators[] = {" said ",  " says ",   " told ", " asked ",
                                      " wrote ", " quoted ", " says,", " said,",
                                      NULL};
    size_t back = pos < 24 ? pos : 24;
    size_t start = pos - back;
    for (size_t ni = 0; narrators[ni]; ni++) {
        if (find_ci(hay + start, back + 1, narrators[ni]) != (size_t)-1)
            return false;
    }
    /* Sentences that begin with " to " often indicate quoted intent ("she
     * wants me to be more X"); skip those too. */
    if (back >= 4) {
        const char *p = hay + pos - 4;
        if ((tolower((unsigned char)p[0]) == 'm' && tolower((unsigned char)p[1]) == 'e' &&
             p[2] == ' ' && tolower((unsigned char)p[3]) == 't') ||
            (p[0] == ' ' && tolower((unsigned char)p[1]) == 't' &&
             tolower((unsigned char)p[2]) == 'o' && p[3] == ' '))
            return false;
    }
    return true;
}

/* Read the next whitespace-delimited token starting at `start` in `hay`,
 * trimming trailing punctuation. Writes up to `out_cap-1` bytes into `out`
 * (always NUL-terminated) and returns the token length, or 0 if the source
 * is empty. */
static size_t read_token(const char *hay, size_t hay_len, size_t start, char *out,
                         size_t out_cap) {
    if (start >= hay_len || out_cap == 0) {
        if (out_cap)
            out[0] = '\0';
        return 0;
    }
    size_t end = start;
    while (end < hay_len && !isspace((unsigned char)hay[end]) && hay[end] != '.' &&
           hay[end] != ',' && hay[end] != '!' && hay[end] != '?' && hay[end] != ';' &&
           hay[end] != ':' && hay[end] != '"' && hay[end] != '\'')
        end++;
    size_t n = end - start;
    if (n + 1 > out_cap)
        n = out_cap - 1;
    for (size_t i = 0; i < n; i++)
        out[i] = (char)tolower((unsigned char)hay[start + i]);
    out[n] = '\0';
    return n;
}

/* Try the longest matching prefix from `prefixes` at offset `pos`. Returns
 * the prefix length on a clause-start match, 0 otherwise. The prefix table
 * MUST be ordered longest-first so we don't double-fire on overlapping
 * variants like "please stop saying" + "stop saying". */
static size_t match_prefix_longest(const char *hay, size_t hay_len, size_t pos,
                                   const char *const *prefixes) {
    if (!is_clause_start(hay, pos))
        return 0;
    for (size_t pi = 0; prefixes[pi]; pi++) {
        size_t plen = strlen(prefixes[pi]);
        if (pos + plen >= hay_len)
            continue;
        size_t k = 0;
        for (; k < plen; k++) {
            if (tolower((unsigned char)hay[pos + k]) != prefixes[pi][k])
                break;
        }
        if (k == plen)
            return plen;
    }
    return 0;
}

/* Each try_* function returns the number of bytes consumed from `pos` (prefix
 * + token) on a match, or 0 on miss. The outer loop uses this to skip past
 * the matched span and avoid double-firing on overlapping prefixes. */

static size_t try_be_more_less(struct hu_graph *graph, const char *contact_id, size_t contact_id_len,
                               const char *channel, const char *hay, size_t hay_len, size_t pos,
                               int64_t now_ms, size_t *proposed) {
    static const char *const prefixes[] = {"be more ", "be less ", NULL};
    size_t plen = match_prefix_longest(hay, hay_len, pos, prefixes);
    if (plen == 0)
        return 0;
    char tok[64];
    size_t tlen = read_token(hay, hay_len, pos + plen, tok, sizeof(tok));
    if (tlen == 0 || tlen > 32)
        return 0;
    char value[160];
    /* Reconstruct "more X" / "less X" -- skip the leading "be ". */
    snprintf(value, sizeof(value), "%s%s", hay[pos + 3] == 'm' ? "more " : "less ", tok);
    hu_persona_delta_kind_t kind = HU_PERSONA_DELTA_TONE;
    if (strcmp(tok, "formal") == 0 || strcmp(tok, "casual") == 0 ||
        strcmp(tok, "professional") == 0)
        kind = HU_PERSONA_DELTA_FORMALITY;
    else if (strcmp(tok, "concise") == 0 || strcmp(tok, "brief") == 0 ||
             strcmp(tok, "short") == 0 || strcmp(tok, "verbose") == 0 ||
             strcmp(tok, "detailed") == 0 || strcmp(tok, "wordy") == 0)
        kind = HU_PERSONA_DELTA_LENGTH;
    if (hu_persona_delta_propose(graph, contact_id, contact_id_len, kind, channel, value,
                                 HU_DELTA_OBS_DEFAULT_CONFIDENCE, HU_DELTA_OBS_SOURCE, now_ms,
                                 NULL) == HU_OK)
        (*proposed)++;
    return plen + tlen;
}

static size_t try_stop_saying(struct hu_graph *graph, const char *contact_id, size_t contact_id_len,
                              const char *channel, const char *hay, size_t hay_len, size_t pos,
                              int64_t now_ms, size_t *proposed) {
    /* Longest-first ordering. */
    static const char *const prefixes[] = {"please stop saying ", "stop saying ", "don't say ",
                                           "dont say ", NULL};
    size_t plen = match_prefix_longest(hay, hay_len, pos, prefixes);
    if (plen == 0)
        return 0;
    char tok[64];
    size_t tlen = read_token(hay, hay_len, pos + plen, tok, sizeof(tok));
    if (tlen == 0 || tlen > 48)
        return 0;
    if (hu_persona_delta_propose(graph, contact_id, contact_id_len, HU_PERSONA_DELTA_VOCAB_AVOID,
                                 channel, tok, HU_DELTA_OBS_DEFAULT_CONFIDENCE,
                                 HU_DELTA_OBS_SOURCE, now_ms, NULL) == HU_OK)
        (*proposed)++;
    return plen + tlen;
}

static size_t try_keep_it(struct hu_graph *graph, const char *contact_id, size_t contact_id_len,
                          const char *channel, const char *hay, size_t hay_len, size_t pos,
                          int64_t now_ms, size_t *proposed) {
    static const char *const prefixes[] = {"keep things ", "keep them ", "keep it ", NULL};
    size_t plen = match_prefix_longest(hay, hay_len, pos, prefixes);
    if (plen == 0)
        return 0;
    char tok[64];
    size_t tlen = read_token(hay, hay_len, pos + plen, tok, sizeof(tok));
    if (tlen == 0)
        return 0;
    if (strcmp(tok, "short") != 0 && strcmp(tok, "brief") != 0 && strcmp(tok, "tight") != 0 &&
        strcmp(tok, "concise") != 0 && strcmp(tok, "long") != 0 && strcmp(tok, "detailed") != 0)
        return 0;
    char value[160];
    snprintf(value, sizeof(value), "prefer-%s", tok);
    if (hu_persona_delta_propose(graph, contact_id, contact_id_len, HU_PERSONA_DELTA_LENGTH,
                                 channel, value, HU_DELTA_OBS_DEFAULT_CONFIDENCE,
                                 HU_DELTA_OBS_SOURCE, now_ms, NULL) == HU_OK)
        (*proposed)++;
    return plen + tlen;
}

static size_t try_dont_talk_about(struct hu_graph *graph, const char *contact_id, size_t contact_id_len,
                                  const char *channel, const char *hay, size_t hay_len,
                                  size_t pos, int64_t now_ms, size_t *proposed) {
    static const char *const prefixes[] = {"stop talking about ", "don't talk about ",
                                           "dont talk about ", "don't bring up ",
                                           "dont bring up ", NULL};
    size_t plen = match_prefix_longest(hay, hay_len, pos, prefixes);
    if (plen == 0)
        return 0;
    char tok[64];
    size_t tlen = read_token(hay, hay_len, pos + plen, tok, sizeof(tok));
    if (tlen == 0)
        return 0;
    if (hu_persona_delta_propose(graph, contact_id, contact_id_len, HU_PERSONA_DELTA_BOUNDARY,
                                 channel, tok, HU_DELTA_OBS_DEFAULT_CONFIDENCE,
                                 HU_DELTA_OBS_SOURCE, now_ms, NULL) == HU_OK)
        (*proposed)++;
    return plen + tlen;
}

hu_error_t hu_persona_observe_user_correction(struct hu_graph *graph, const char *contact_id,
                                              size_t contact_id_len, const char *channel,
                                              size_t channel_len, const char *msg,
                                              size_t msg_len, int64_t now_ms,
                                              size_t *out_proposed) {
    size_t local_count = 0;
    if (out_proposed)
        *out_proposed = 0;
    if (!msg || msg_len == 0)
        return HU_OK;
    if (!graph)
        return HU_OK; /* no place to store -- silent no-op for callers that wire unconditionally */
    if (!contact_id || contact_id_len == 0)
        return HU_OK; /* the propose layer rejects empty contact_id; treat as no-op */
    /* channel may be NULL; we pass "" (the propose layer accepts empty key). */
    char channel_buf[64] = {0};
    if (channel && channel_len > 0) {
        size_t n = channel_len < sizeof(channel_buf) - 1 ? channel_len : sizeof(channel_buf) - 1;
        memcpy(channel_buf, channel, n);
        channel_buf[n] = '\0';
    }

    /* Walk every byte and try each pattern. The patterns themselves enforce
     * is_clause_start so we don't emit duplicates from interior matches. */
    static const struct {
        const char *anchor;
    } anchors[] = {
        {"be "}, {"stop "}, {"don't "}, {"dont "}, {"please "}, {"keep "}, {NULL},
    };
    (void)anchors;

    /* Cheap pre-filter: if none of the anchor verbs appears, skip the full
     * walk. This keeps us off the hot path for the vast majority of messages. */
    bool any_anchor = (find_ci(msg, msg_len, "be ") != (size_t)-1) ||
                      (find_ci(msg, msg_len, "stop ") != (size_t)-1) ||
                      (find_ci(msg, msg_len, "don't ") != (size_t)-1) ||
                      (find_ci(msg, msg_len, "dont ") != (size_t)-1) ||
                      (find_ci(msg, msg_len, "keep ") != (size_t)-1);
    if (!any_anchor)
        return HU_OK;

    for (size_t i = 0; i < msg_len;) {
        size_t consumed;
        consumed = try_be_more_less(graph, contact_id, contact_id_len, channel_buf, msg, msg_len,
                                    i, now_ms, &local_count);
        if (consumed) {
            i += consumed;
            continue;
        }
        consumed = try_stop_saying(graph, contact_id, contact_id_len, channel_buf, msg, msg_len,
                                   i, now_ms, &local_count);
        if (consumed) {
            i += consumed;
            continue;
        }
        consumed = try_keep_it(graph, contact_id, contact_id_len, channel_buf, msg, msg_len, i,
                               now_ms, &local_count);
        if (consumed) {
            i += consumed;
            continue;
        }
        consumed = try_dont_talk_about(graph, contact_id, contact_id_len, channel_buf, msg,
                                       msg_len, i, now_ms, &local_count);
        if (consumed) {
            i += consumed;
            continue;
        }
        i++;
    }

    if (out_proposed)
        *out_proposed = local_count;
    return HU_OK;
}

hu_error_t hu_persona_observe_user_correction_with_learner(
    struct hu_graph *graph, struct hu_learner *learner, const char *contact_id, size_t contact_id_len,
    const char *channel, size_t channel_len, const char *msg, size_t msg_len, int64_t now_ms,
    size_t *out_proposed) {
    /* Step 1: run the standard observation. Same semantics as the original
     * function — the graph receives delta proposals, *out_proposed counts
     * matches, no signal-collection side effects yet. */
    size_t observed = 0;
    hu_error_t e = hu_persona_observe_user_correction(graph, contact_id, contact_id_len, channel,
                                                      channel_len, msg, msg_len, now_ms, &observed);
    if (out_proposed)
        *out_proposed = observed;
    if (e != HU_OK)
        return e;

    /* Step 2: emit signals to the learner if one is present. NULL learner
     * is a clean no-op — callers wire this in unconditionally and the
     * disabled-installation path stays free of branches. */
    if (!learner || observed == 0)
        return HU_OK;

#if defined(HU_ENABLE_LEARNING) && defined(HU_ENABLE_SQLITE)
    /* The just-proposed deltas are PENDING in the graph. Drain them and
     * hand them to the bridge, which uses its watermark to ensure replays
     * (e.g. the same message processed twice on retry) don't double-emit.
     *
     * The list is bounded to the most recent 64 deltas — the rate limiter
     * in hu_persona_delta_propose caps a single call to far fewer than
     * that, so we never miss anything in practice. */
    hu_learner_t *l = learner;
    hu_allocator_t *alloc = l->alloc;
    if (!alloc)
        return HU_OK;

    hu_persona_delta_t *deltas = NULL;
    size_t n = 0;
    if (hu_persona_delta_list(graph, alloc, contact_id, contact_id_len, HU_DELTA_STATUS_PENDING, 64,
                              &deltas, &n) != HU_OK)
        return HU_OK;
    if (n > 0)
        (void)hu_learner_bridge_emit_persona_deltas(l, deltas, n);
    hu_persona_delta_free(alloc, deltas, n);
#else
    (void)learner;
#endif
    return HU_OK;
}
