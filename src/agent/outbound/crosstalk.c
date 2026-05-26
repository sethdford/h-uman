/* outbound/crosstalk.c — cross-contact bleed + metadata-leak detection.
 *
 * Two concerns this stage owns:
 *
 *   1. CROSS-CONTACT BLEED (the Annie/Mindy/Betty incident)
 *      Char-5-gram Jaccard between the outgoing message and each
 *      OTHER contact's recent (7-day) inbound text. If overlap >=
 *      0.4 with any other contact, REJECT with
 *      "crosstalk_other_contact".
 *
 *      Per Q-2 user decision: char-5-gram Jaccard, 7-day window.
 *      ~10ms/send budget.
 *
 *   2. INTERNAL-METADATA LEAK (corpus #4)
 *      The "(last: 1774705881)" pattern — a Unix timestamp wrapped
 *      in parentheses with a metadata-keyword prefix. No legitimate
 *      conversational text matches this pattern. REJECT with
 *      "crosstalk_metadata_pattern".
 *
 * Architecture: the cross-contact check needs other contacts' text
 * history. The stage delegates lookup via a pluggable callback so:
 *
 *   - Tests inject a fake corpus directly
 *   - Production wires to SQLite via the existing memory backend
 *
 * The pure Jaccard predicate is exported as
 * `hu_outbound_crosstalk_jaccard_5gram` for direct unit testing.
 *
 * When ctx->memory is NULL (test mode), the cross-contact check
 * gracefully degrades — only the metadata-leak check runs. Per
 * silent-config-gated-subsystems.md, the stage logs ONCE on
 * first SEND-without-memory so operators see the weakened path.
 *
 * Returns:
 *   SEND   — clean OR memory-unavailable + no metadata leak
 *   REJECT — bleed or metadata leak detected
 *   (Never REGENERATE — there's no LLM rewrite for "you accidentally
 *    pasted another contact's message into Mindy's chat.")
 */

#include "human/agent/outbound_pipeline.h"

#include <ctype.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "human/core/log.h"

#define CROSSTALK_NGRAM_K           5u
#define CROSSTALK_JACCARD_THRESHOLD 0.40
#define CROSSTALK_MIN_LEN           8u /* skip Jaccard for too-short messages */

/* ----------------------------------------------------------------- */
/* Pure Jaccard predicate — exported for unit tests                  */
/* ----------------------------------------------------------------- */

/* Lowercase + strip non-alnum chars to a working buffer. Returns
 * working length. Provides bytes-only normalization; non-ASCII
 * passes through untouched (we don't need Unicode word boundaries
 * for the verbatim-bleed detection case). */
static size_t normalize_to(const char *src, size_t src_len, char *dst, size_t dst_cap) {
    size_t w = 0;
    for (size_t i = 0; i < src_len && w + 1 < dst_cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if (isalnum(c) || (c >= 0x80)) {
            dst[w++] = (char)tolower(c);
        } else if (c == ' ') {
            /* Collapse whitespace to single space. */
            if (w == 0 || dst[w - 1] != ' ')
                dst[w++] = ' ';
        }
        /* Other punctuation dropped. */
    }
    /* Trim trailing space. */
    while (w > 0 && dst[w - 1] == ' ')
        w--;
    dst[w] = '\0';
    return w;
}

/* 5-gram set is approximated as a sorted, dedup'd array of 5-byte
 * windows. Allocated by caller. */
typedef struct {
    uint64_t *ngrams; /* packed first-5-bytes into 64-bit ints */
    size_t count;
    size_t capacity;
} ngram_set_t;

/* Pack 5 chars into the low 40 bits of a uint64. */
static uint64_t pack5(const char *p) {
    return ((uint64_t)(unsigned char)p[0]) | ((uint64_t)(unsigned char)p[1] << 8) |
           ((uint64_t)(unsigned char)p[2] << 16) | ((uint64_t)(unsigned char)p[3] << 24) |
           ((uint64_t)(unsigned char)p[4] << 32);
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    if (x < y)
        return -1;
    if (x > y)
        return 1;
    return 0;
}

/* Build the n-gram set from a normalized buffer. Returns 0 on OK,
 * -1 on OOM. */
static int build_ngrams(hu_allocator_t *alloc, const char *norm, size_t nlen, ngram_set_t *out) {
    out->ngrams = NULL;
    out->count = 0;
    out->capacity = 0;
    if (nlen < CROSSTALK_NGRAM_K)
        return 0;
    size_t n = nlen - CROSSTALK_NGRAM_K + 1;
    uint64_t *buf = (uint64_t *)alloc->alloc(alloc->ctx, n * sizeof(uint64_t));
    if (!buf)
        return -1;
    for (size_t i = 0; i < n; i++)
        buf[i] = pack5(norm + i);
    qsort(buf, n, sizeof(uint64_t), cmp_u64);
    /* Dedup in place. */
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        if (w == 0 || buf[w - 1] != buf[i])
            buf[w++] = buf[i];
    }
    out->ngrams = buf;
    out->count = w;
    out->capacity = n;
    return 0;
}

static void free_ngrams(hu_allocator_t *alloc, ngram_set_t *s) {
    if (s->ngrams)
        alloc->free(alloc->ctx, s->ngrams, s->capacity * sizeof(uint64_t));
    s->ngrams = NULL;
    s->count = 0;
}

/* Jaccard = |A ∩ B| / |A ∪ B|. Both inputs are sorted. */
static double jaccard(const ngram_set_t *a, const ngram_set_t *b) {
    if (a->count == 0 || b->count == 0)
        return 0.0;
    size_t i = 0, j = 0, inter = 0;
    while (i < a->count && j < b->count) {
        if (a->ngrams[i] == b->ngrams[j]) {
            inter++;
            i++;
            j++;
        } else if (a->ngrams[i] < b->ngrams[j]) {
            i++;
        } else {
            j++;
        }
    }
    size_t uni = a->count + b->count - inter;
    if (uni == 0)
        return 0.0;
    return (double)inter / (double)uni;
}

/* Public predicate — pure, no I/O. Returns Jaccard score of the
 * 5-gram sets of `a` vs `b` after normalization. Allocator is used
 * for working buffers and freed before return. */
double hu_outbound_crosstalk_jaccard_5gram(hu_allocator_t *alloc, const char *a, size_t a_len,
                                           const char *b, size_t b_len) {
    if (!alloc || !a || !b || a_len == 0 || b_len == 0)
        return 0.0;
    /* Normalize. Two scratch bufs sized to the input. */
    char *na = (char *)alloc->alloc(alloc->ctx, a_len + 1);
    char *nb = (char *)alloc->alloc(alloc->ctx, b_len + 1);
    if (!na || !nb) {
        if (na)
            alloc->free(alloc->ctx, na, a_len + 1);
        if (nb)
            alloc->free(alloc->ctx, nb, b_len + 1);
        return 0.0;
    }
    size_t nalen = normalize_to(a, a_len, na, a_len + 1);
    size_t nblen = normalize_to(b, b_len, nb, b_len + 1);

    ngram_set_t sa = {0}, sb = {0};
    double score = 0.0;
    if (build_ngrams(alloc, na, nalen, &sa) == 0 && build_ngrams(alloc, nb, nblen, &sb) == 0) {
        score = jaccard(&sa, &sb);
    }
    free_ngrams(alloc, &sa);
    free_ngrams(alloc, &sb);
    alloc->free(alloc->ctx, na, a_len + 1);
    alloc->free(alloc->ctx, nb, b_len + 1);
    return score;
}

/* ----------------------------------------------------------------- */
/* Metadata-pattern check (corpus #4)                                */
/* ----------------------------------------------------------------- */

/* Detects "(keyword: NNNN)" where keyword is alpha and NNNN is a
 * 9-13 digit integer (Unix timestamp shape). Standalone OR embedded
 * — any occurrence rejects. */
static int has_metadata_leak(const char *s, size_t len) {
    for (size_t i = 0; i + 4 < len; i++) {
        if (s[i] != '(')
            continue;
        size_t j = i + 1;
        size_t kw_start = j;
        while (j < len && (isalpha((unsigned char)s[j]) || s[j] == '_'))
            j++;
        if (j == kw_start)
            continue;
        if (j >= len || s[j] != ':')
            continue;
        j++;
        while (j < len && s[j] == ' ')
            j++;
        size_t num_start = j;
        while (j < len && isdigit((unsigned char)s[j]))
            j++;
        size_t num_len = j - num_start;
        if (num_len < 9 || num_len > 13)
            continue;
        if (j >= len || s[j] != ')')
            continue;
        return 1;
    }
    return 0;
}

/* ----------------------------------------------------------------- */
/* Pluggable lookup callback                                         */
/* ----------------------------------------------------------------- */

/* Module-level callback hook. NULL by default — when NULL, the
 * cross-contact check is a no-op. Production wires this in
 * pipeline_configs.c (or daemon startup) to a SQLite-backed lookup.
 * Type declared in include/human/agent/outbound_pipeline.h. */
static hu_outbound_crosstalk_lookup_fn_t s_lookup = NULL;
static void *s_lookup_userdata = NULL;
static atomic_uint_least32_t s_no_memory_warned = 0;

void hu_outbound_crosstalk_set_lookup(hu_outbound_crosstalk_lookup_fn_t fn, void *userdata) {
    s_lookup = fn;
    s_lookup_userdata = userdata;
}

/* ----------------------------------------------------------------- */
/* Stage entry                                                       */
/* ----------------------------------------------------------------- */

static hu_outbound_verdict_t crosstalk_run(hu_outbound_stage_t *self, hu_outbound_message_t *msg,
                                           hu_outbound_context_t *ctx) {
    (void)self;
    if (!msg || !msg->content || msg->content_len == 0)
        return hu_outbound_verdict_send();
    if (!ctx || !ctx->alloc)
        return hu_outbound_verdict_send();

    /* Metadata-pattern check first (cheap, no allocations). */
    if (has_metadata_leak(msg->content, msg->content_len)) {
        return hu_outbound_verdict_reject("crosstalk_metadata_pattern");
    }

    /* Cross-contact Jaccard check. */
    if (msg->content_len < CROSSTALK_MIN_LEN)
        return hu_outbound_verdict_send();

    if (!s_lookup) {
        /* Degraded path — log once. The pipeline still functions;
         * defense-in-depth is just weaker without contact history. */
        uint32_t prev = atomic_fetch_or_explicit(&s_no_memory_warned, 1u, memory_order_relaxed);
        if ((prev & 1u) == 0) {
            hu_log_info("outbound", NULL,
                        "crosstalk: no lookup callback registered — cross-contact "
                        "bleed check is a no-op. Register via "
                        "hu_outbound_crosstalk_set_lookup() to enable.");
        }
        return hu_outbound_verdict_send();
    }

    char **other_texts = NULL;
    size_t other_count = 0;
    if (s_lookup(s_lookup_userdata, ctx->alloc, ctx->recipient_contact_id,
                 ctx->recipient_contact_id_len, &other_texts, &other_count) != 0) {
        return hu_outbound_verdict_send();
    }

    hu_outbound_verdict_t verdict = hu_outbound_verdict_send();
    for (size_t i = 0; i < other_count; i++) {
        if (!other_texts[i])
            continue;
        size_t olen = strlen(other_texts[i]);
        if (olen == 0)
            continue;
        double score = hu_outbound_crosstalk_jaccard_5gram(ctx->alloc, msg->content,
                                                           msg->content_len, other_texts[i], olen);
        if (score >= CROSSTALK_JACCARD_THRESHOLD) {
            verdict = hu_outbound_verdict_reject("crosstalk_other_contact");
            break;
        }
    }

    /* Free the lookup's heap-owned strings and the array. */
    for (size_t i = 0; i < other_count; i++) {
        if (other_texts[i])
            ctx->alloc->free(ctx->alloc->ctx, other_texts[i], strlen(other_texts[i]) + 1);
    }
    if (other_texts)
        ctx->alloc->free(ctx->alloc->ctx, other_texts, other_count * sizeof(char *));

    return verdict;
}

hu_outbound_stage_t hu_outbound_stage_crosstalk = {
    .name = "crosstalk",
    .run = crosstalk_run,
    .state = NULL,
};
