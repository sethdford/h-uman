/* outbound/echo.c — semantic directive-echo detection.
 *
 * Detects when the LLM echoed back its prompt or a fragment of an
 * instruction-shaped phrase, instead of generating actual content.
 * Two algorithms layered:
 *
 *   1. STANDALONE-DIRECTIVE check (REJECT)
 *      Compares the trimmed message body against a small fixed list
 *      of directive-shaped phrases that no human would ever send
 *      standalone. This is INTENTIONALLY tiny — not a growing
 *      blocklist; it's the universal-failure set distilled from the
 *      2026-05-26 incident corpus:
 *        "shared history"     (corpus #7, #8)
 *        "principle"          (corpus #9)
 *        "under 10 words"     (corpus #10)
 *        "reference"          (variant of #5)
 *      Matching is case-insensitive on the trimmed body.
 *      REJECT (not REGENERATE) because there's nothing to regenerate
 *      from — the LLM produced a directive token verbatim.
 *
 *   2. PROMPT-OVERLAP check (REGENERATE)
 *      If `msg->prompt_used` is provided, compute token-overlap
 *      between the message body and the prompt. If >= 40% of the
 *      message's tokens (case-folded, length >= 4) appear in the
 *      prompt, REGENERATE with stricter system addendum.
 *      Catches corpus #5: "reference something specific you know
 *      about them or ask about something from a previous
 *      conversation" — every word came from the system prompt.
 *
 * Token definition for overlap: maximal runs of [A-Za-z0-9'] of
 * length >= 4. Words shorter than 4 chars are skipped (stop-word
 * approximation without a dictionary).
 *
 * NOT triggered by:
 *   - Short organic messages ("how are you", "hi mom") — no tokens
 *     >= 4 chars overlap with a meaningful prompt
 *   - Sharing a phrase that's COMMON ("how's the garden" might
 *     appear in the prompt but the overlap stays under 40%)
 *
 * Returns:
 *   SEND        — clean
 *   REGENERATE  — high prompt overlap; LLM should try again
 *   REJECT      — standalone directive token; not recoverable
 */

#include "human/agent/outbound_pipeline.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

/* Small fixed set of standalone-directive strings. Matched after
 * trim + case-fold. Order matters only for readability. */
static const char *const STANDALONE_DIRECTIVES[] = {
    "shared history",        "principle",  "under 10 words", "reference", "reference something",
    "previous conversation", "be concise", "be brief",       NULL,
};

/* Long directive PREFIXES — when the LLM echoes the start of an
 * instruction-shaped sentence verbatim. These match if the trimmed
 * body STARTS with the prefix (case-insensitive). Tested against the
 * incident corpus row #5. */
static const char *const DIRECTIVE_PREFIXES[] = {
    "reference something specific",
    "ask about something from a previous",
    "match their energy",
    "casual greeting back",
    "short empathetic reaction",
    "de-escalate: acknowledge feelings",
    "[safety]",
    "this response",
    NULL,
};

/* Trim leading/trailing whitespace + sentence terminators. Returns
 * pointer into s (into a trimmed window) and writes the trimmed
 * length to *out_len. The window may have NO NUL terminator. */
static const char *trim_view(const char *s, size_t len, size_t *out_len) {
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            i++;
        else
            break;
    }
    size_t j = len;
    while (j > i) {
        unsigned char c = (unsigned char)s[j - 1];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '.' || c == '!' || c == '?' ||
            c == ',') {
            j--;
        } else {
            break;
        }
    }
    *out_len = j - i;
    return s + i;
}

/* Case-insensitive compare of [a, a+n) to NUL-terminated b. */
static int ci_match_exact(const char *a, size_t n, const char *b) {
    size_t bl = strlen(b);
    if (n != bl)
        return 0;
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return 0;
    }
    return 1;
}

/* Case-insensitive prefix check: does [a, a+n) START with `b`? */
static int ci_starts_with(const char *a, size_t n, const char *b) {
    size_t bl = strlen(b);
    if (n < bl)
        return 0;
    for (size_t i = 0; i < bl; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return 0;
    }
    return 1;
}

/* Token iterator state. */
typedef struct {
    const char *s;
    size_t len;
    size_t i;
} tok_iter_t;

/* Returns 1 if a token was extracted into out_buf (zero-terminated,
 * case-folded), 0 if no more tokens. Tokens are runs of
 * [A-Za-z0-9'] with length >= 4. Shorter runs are skipped (stop-word
 * approximation). */
static int next_token(tok_iter_t *it, char *out_buf, size_t out_cap) {
    while (it->i < it->len) {
        /* skip non-token chars */
        while (it->i < it->len) {
            unsigned char c = (unsigned char)it->s[it->i];
            if (isalnum(c) || c == '\'')
                break;
            it->i++;
        }
        if (it->i >= it->len)
            return 0;
        size_t start = it->i;
        while (it->i < it->len) {
            unsigned char c = (unsigned char)it->s[it->i];
            if (isalnum(c) || c == '\'')
                it->i++;
            else
                break;
        }
        size_t tlen = it->i - start;
        if (tlen < 4)
            continue; /* skip short */
        if (tlen >= out_cap)
            tlen = out_cap - 1;
        for (size_t k = 0; k < tlen; k++) {
            out_buf[k] = (char)tolower((unsigned char)it->s[start + k]);
        }
        out_buf[tlen] = '\0';
        return 1;
    }
    return 0;
}

/* Returns true if `needle` (NUL-terminated) appears as a token in
 * the prompt window. */
static int prompt_has_token(const char *prompt, size_t plen, const char *needle) {
    tok_iter_t it = {prompt, plen, 0};
    char tok[64];
    while (next_token(&it, tok, sizeof(tok))) {
        if (strcmp(tok, needle) == 0)
            return 1;
    }
    return 0;
}

/* Compute fraction of message tokens that appear in the prompt.
 * Returns 0.0 if message has zero tokens. */
static double prompt_token_overlap(const char *msg, size_t mlen, const char *prompt, size_t plen) {
    if (!prompt || plen == 0)
        return 0.0;
    tok_iter_t it = {msg, mlen, 0};
    char tok[64];
    size_t total = 0;
    size_t hits = 0;
    while (next_token(&it, tok, sizeof(tok))) {
        total++;
        if (prompt_has_token(prompt, plen, tok))
            hits++;
    }
    if (total == 0)
        return 0.0;
    return (double)hits / (double)total;
}

#define ECHO_OVERLAP_THRESHOLD 0.40

static hu_outbound_verdict_t echo_run(hu_outbound_stage_t *self, hu_outbound_message_t *msg,
                                      hu_outbound_context_t *ctx) {
    (void)self;
    (void)ctx;
    if (!msg || !msg->content || msg->content_len == 0)
        return hu_outbound_verdict_send();

    size_t trimmed_len = 0;
    const char *trimmed = trim_view(msg->content, msg->content_len, &trimmed_len);
    if (trimmed_len == 0)
        return hu_outbound_verdict_send();

    /* Algorithm 1a: standalone-directive REJECT. */
    for (size_t i = 0; STANDALONE_DIRECTIVES[i]; i++) {
        if (ci_match_exact(trimmed, trimmed_len, STANDALONE_DIRECTIVES[i])) {
            return hu_outbound_verdict_reject("echo_standalone_directive");
        }
    }

    /* Algorithm 1b: directive-prefix REJECT — body starts with a
     * known instruction-shaped phrase. Caught corpus #5 (long LLM
     * echo of system prompt). */
    for (size_t i = 0; DIRECTIVE_PREFIXES[i]; i++) {
        if (ci_starts_with(trimmed, trimmed_len, DIRECTIVE_PREFIXES[i])) {
            return hu_outbound_verdict_reject("echo_directive_prefix");
        }
    }

    /* Algorithm 2: prompt-overlap REGENERATE. Only if prompt is
     * available. The pipeline context provides msg->prompt_used. */
    if (msg->prompt_used && msg->prompt_used_len > 0) {
        double overlap = prompt_token_overlap(msg->content, msg->content_len, msg->prompt_used,
                                              msg->prompt_used_len);
        if (overlap >= ECHO_OVERLAP_THRESHOLD) {
            return hu_outbound_verdict_regenerate(
                "echo_prompt_overlap",
                "Generate an actual reply — do not echo back any of the system "
                "instructions. Write what Seth would say.");
        }
    }

    return hu_outbound_verdict_send();
}

hu_outbound_stage_t hu_outbound_stage_echo = {
    .name = "echo",
    .run = echo_run,
    .state = NULL,
};
