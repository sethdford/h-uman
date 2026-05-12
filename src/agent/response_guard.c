/* hu_response_guard — last-mile sanitizer + degenerate-output detector.
 *
 * Two production failures motivate this module (caught 2026-05-10 in the
 * live h-uman daemon when the apple-foundationmodel reflexive-tier
 * provider leaked Harmony special tokens + a degenerate quote loop to a
 * real human contact via iMessage):
 *
 *   1) The provider returned text with `<|channel>thought` (a Harmony
 *      special token that should have been a stop / detokenized away).
 *      No upstream gate (verifier, quality, Turing, constitutional)
 *      stripped it.
 *
 *   2) The same response degenerated into `\" \" \" \" \" ...` for ~250
 *      tokens — a classic temperature/penalty failure.  Again, no gate.
 *
 * Design:
 *   - O(n) scan that strips known-bad <|...|> markers and <think|thought>
 *     blocks.
 *   - Bounded-cost repetition detector: longest single-char run + longest
 *     short-token run (1..8 chars between whitespace).
 *   - On detection, returns HU_GUARD_REJECT and the caller MUST suppress.
 *   - Thresholds calibrated so legitimate "yessss!! 😂😂", "very very very
 *     good", "lol" etc. pass cleanly, but the production failure (200x
 *     `\" `, 100x `a`) is rejected.
 *
 * Allocations:
 *   The OK path performs zero allocations (out_response = original input).
 *   The REWROTE path allocates one buffer of size ≤ original length, owned
 *   by the caller.  REJECT path performs zero allocations.
 */
#include "human/agent/response_guard.h"
#include "human/core/allocator.h"
#include "human/core/error.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* Calibrated thresholds — tuned against the production failure (200x `\" `,
 * 100x `a`) and against legitimate human-shaped text ("yessss!!", "lol",
 * triple emoji, etc.).
 *
 * Single-character runs above this are degenerate.  35 = roughly the
 * upper bound of a long emphatic word like "ohmygoooooooooooodddd!" plus
 * some safety margin. */
#define HU_GUARD_MAX_CHAR_RUN 35

/* Short-token run threshold (a token = run of non-whitespace, 1..8 chars).
 * 8 = enough to catch real degenerate loops while permitting natural
 * repetition like "hahaha" or "very very very". */
#define HU_GUARD_MAX_TOKEN_RUN 8

/* Tokens classified as "short" for the token-run detector. Longer tokens
 * (≥ 9 chars) bypass this check; collisions there are extraordinarily
 * unlikely to be degenerate model output anyway. */
#define HU_GUARD_TOKEN_LEN_MAX 8

/* Sprint 29 — numbered analytical-list dump detector thresholds.
 *
 * Calibrated against the 2026-05-12 leak (5 items, content lengths
 * 32 / 32 / 52 / 72 / 45 chars) and against legitimate short numbered
 * lists ("1. coffee\n2. lunch\n3. dinner" — items 6, 5, 6 chars).
 *
 * `LONG_THRESHOLD = 30` cleanly separates the two: every leak item is
 * >= 32 chars; every legit short-list item is <= 12 chars.
 *
 * `MIN_ITEMS = 3` is the smallest count that distinguishes a real
 * structured analytical dump from a one-off "as I said in (1) ..."
 * casual mention. */
#define HU_GUARD_NUMBERED_LONG_THRESHOLD 30
#define HU_GUARD_NUMBERED_MIN_ITEMS      3

/* Documented surface (handled by the catch-all <|...|> stripper, listed
 * here so future maintainers know what model formats we cover):
 *
 *   Harmony / OpenAI gpt-oss : <|start|>, <|end|>, <|return|>,
 *                              <|channel|>, <|message|>, <|thought|>,
 *                              <|user|>, <|assistant|>, <|system|>,
 *                              <|tool_call|>, <|tool_response|>
 *   ChatML / Qwen / Yi       : <|im_start|>, <|im_end|>
 *   GPT-2 / GPT-3            : <|endoftext|>
 *   Llama 3                  : <|eot_id|>, <|begin_of_text|>, <|end_of_text|>
 *
 * Unclosed forms (e.g. `<|channel>thought` — the actual production leak)
 * are handled by the unclosed-token branch below. */

/* ── Helpers (exposed) ─────────────────────────────────────────────────── */

bool hu_response_guard_has_special_token(const char *s, size_t len) {
    if (!s || len < 2)
        return false;
    /* <|...|> form (closed). */
    for (size_t i = 0; i + 1 < len; i++) {
        if (s[i] == '<' && s[i + 1] == '|') {
            for (size_t j = i + 2; j + 1 < len; j++) {
                if (s[j] == '|' && s[j + 1] == '>')
                    return true;
                if (s[j] == '\n' || s[j] == '\r')
                    break;
            }
        }
    }
    /* <think>...</think> or <thought>...</thought> form. */
    if (len >= 7) {
        for (size_t i = 0; i + 6 < len; i++) {
            if (s[i] == '<' && (s[i + 1] == 't' || s[i + 1] == 'T') &&
                (s[i + 2] == 'h' || s[i + 2] == 'H') &&
                (s[i + 3] == 'i' || s[i + 3] == 'I') &&
                (s[i + 4] == 'n' || s[i + 4] == 'N') &&
                (s[i + 5] == 'k' || s[i + 5] == 'K') && s[i + 6] == '>')
                return true;
        }
    }
    if (len >= 9) {
        for (size_t i = 0; i + 8 < len; i++) {
            if (s[i] == '<' && (s[i + 1] == 't' || s[i + 1] == 'T') &&
                (s[i + 2] == 'h' || s[i + 2] == 'H') &&
                (s[i + 3] == 'o' || s[i + 3] == 'O') &&
                (s[i + 4] == 'u' || s[i + 4] == 'U') &&
                (s[i + 5] == 'g' || s[i + 5] == 'G') &&
                (s[i + 6] == 'h' || s[i + 6] == 'H') &&
                (s[i + 7] == 't' || s[i + 7] == 'T') && s[i + 8] == '>')
                return true;
        }
    }
    /* Unclosed `<|TAG` form (the production leak). Match `<|` followed by
     * an identifier char (a-z) — rare in legit text, common in token
     * spillage. */
    for (size_t i = 0; i + 2 < len; i++) {
        if (s[i] == '<' && s[i + 1] == '|') {
            char c = s[i + 2];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
                return true;
        }
    }
    return false;
}

size_t hu_response_guard_longest_char_run(const char *s, size_t len) {
    if (!s || len == 0)
        return 0;
    size_t best = 1;
    size_t cur = 1;
    for (size_t i = 1; i < len; i++) {
        if (s[i] == s[i - 1]) {
            cur++;
            if (cur > best)
                best = cur;
        } else {
            cur = 1;
        }
    }
    return best;
}

size_t hu_response_guard_longest_token_run(const char *s, size_t len) {
    if (!s || len == 0)
        return 0;
    size_t best = 0;
    /* Walk the string, slicing on whitespace. Track previous token's start
     * + length; when current token matches previous, increment a run. */
    size_t i = 0;
    char prev_tok[HU_GUARD_TOKEN_LEN_MAX + 1];
    size_t prev_len = 0;
    size_t cur_run = 0;
    while (i < len) {
        /* Skip whitespace. */
        while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
            i++;
        if (i >= len)
            break;
        /* Read one token. */
        size_t tok_start = i;
        while (i < len && s[i] != ' ' && s[i] != '\t' && s[i] != '\n' && s[i] != '\r')
            i++;
        size_t tok_len = i - tok_start;
        if (tok_len == 0 || tok_len > HU_GUARD_TOKEN_LEN_MAX) {
            prev_len = 0;
            cur_run = 0;
            continue;
        }
        if (prev_len == tok_len && memcmp(prev_tok, s + tok_start, tok_len) == 0) {
            cur_run++;
            if (cur_run > best)
                best = cur_run;
        } else {
            cur_run = 1; /* this token starts a new (possible) run */
            if (cur_run > best)
                best = cur_run;
            memcpy(prev_tok, s + tok_start, tok_len);
            prev_len = tok_len;
        }
    }
    return best;
}

/* ── Sprint 29: semantic leak detectors (CoT / prompt-context dumps) ───
 *
 * Three failure classes that Phase 0/1/2 cannot catch because they have
 * no markup signature and no repetition signature. They are all variants
 * of "the model dumped its internal reasoning verbatim into the reply."
 * Caught from a real production leak on 2026-05-12 to a real human
 * contact via the live service-loop daemon. See docs/postmortems/. */

/* Case-insensitive substring search. Returns true if `needle[0..nlen)`
 * occurs anywhere in `hay[0..hlen)`. ASCII-only; we do not normalize
 * unicode case here because every leak signature we match against is
 * pure ASCII. */
static bool hu_guard_ci_contains(const char *hay, size_t hlen, const char *needle, size_t nlen) {
    if (nlen == 0)
        return true;
    if (hlen < nlen)
        return false;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        bool match = true;
        for (size_t k = 0; k < nlen; k++) {
            char a = hay[i + k];
            char b = needle[k];
            if (a >= 'A' && a <= 'Z')
                a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z')
                b = (char)(b + 32);
            if (a != b) {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }
    return false;
}

/* G1 — numbered analytical-list dump.
 *
 * Returns true if `s[0..len)` contains ≥ HU_GUARD_NUMBERED_MIN_ITEMS
 * numbered list items (each on its own line, leading-whitespace tolerant)
 * where each qualifying item has content length ≥
 * HU_GUARD_NUMBERED_LONG_THRESHOLD chars.
 *
 * Shape recognized:    `<line-start>[ \t]*\d+[.)] [ \t]*CONTENT<\n>`
 *
 * Calibrated: legitimate short lists ("1. coffee\n2. lunch\n3. dinner")
 * have content lengths 6/5/6 chars — well below the 30-char threshold.
 * The 2026-05-12 leak items were 32/32/52/72/45 chars — well above. */
static bool hu_guard_has_numbered_analysis_dump(const char *s, size_t len) {
    if (len < 8)
        return false;
    int long_items = 0;
    size_t pos = 0;
    while (pos < len) {
        /* Walk to the next line start unless we're already at one. */
        if (pos > 0 && s[pos - 1] != '\n') {
            const char *nl = (const char *)memchr(s + pos, '\n', len - pos);
            if (!nl)
                break;
            pos = (size_t)(nl - s) + 1;
            if (pos >= len)
                break;
        }
        /* At a line start. Skip leading whitespace. */
        while (pos < len && (s[pos] == ' ' || s[pos] == '\t'))
            pos++;
        if (pos >= len)
            break;
        size_t scan = pos;
        /* Need 1+ digit. */
        if (!(s[scan] >= '0' && s[scan] <= '9')) {
            const char *nl = (const char *)memchr(s + pos, '\n', len - pos);
            if (!nl)
                break;
            pos = (size_t)(nl - s) + 1;
            continue;
        }
        while (scan < len && s[scan] >= '0' && s[scan] <= '9')
            scan++;
        /* Followed by `.` or `)`. */
        if (scan >= len || (s[scan] != '.' && s[scan] != ')')) {
            const char *nl = (const char *)memchr(s + pos, '\n', len - pos);
            if (!nl)
                break;
            pos = (size_t)(nl - s) + 1;
            continue;
        }
        scan++;
        /* Followed by 1+ whitespace (space or tab). */
        if (scan >= len || (s[scan] != ' ' && s[scan] != '\t')) {
            const char *nl = (const char *)memchr(s + pos, '\n', len - pos);
            if (!nl)
                break;
            pos = (size_t)(nl - s) + 1;
            continue;
        }
        while (scan < len && (s[scan] == ' ' || s[scan] == '\t'))
            scan++;
        /* `scan` is now at the content start. Measure to end-of-line. */
        const char *nl = (const char *)memchr(s + scan, '\n', len - scan);
        size_t content_end = nl ? (size_t)(nl - s) : len;
        size_t content_len = content_end - scan;
        if (content_len >= HU_GUARD_NUMBERED_LONG_THRESHOLD) {
            long_items++;
            if (long_items >= HU_GUARD_NUMBERED_MIN_ITEMS)
                return true;
        }
        pos = nl ? content_end + 1 : len;
    }
    return false;
}

/* G2 — model self-talk / scene-direction echo.
 *
 * These patterns have no legitimate human-to-human reply use case. A
 * single hit is a hard reject. All six were present in the 2026-05-12
 * leak; none can plausibly appear in a normal text message to a friend. */
static bool hu_guard_has_self_talk_pattern(const char *s, size_t len) {
    static const struct {
        const char *pat;
        size_t pat_len;
    } patterns[] = {
        {"the prompt says", 15},
        {"wait, the prompt", 16},
        {"i should still maintain", 23},
        {"(per scene direction", 20},
        {"per the scene direction", 23},
        {"the user is bombarding", 22},
    };
    const size_t n = sizeof(patterns) / sizeof(patterns[0]);
    for (size_t i = 0; i < n; i++) {
        if (hu_guard_ci_contains(s, len, patterns[i].pat, patterns[i].pat_len))
            return true;
    }
    return false;
}

/* G3 — third-person-about-the-user pattern counter.
 *
 * Returns the number of distinct third-person profile signals found.
 * Single hits are ambiguous (a real reply might say "He's coming over").
 * Two or more distinct signals is structural — that's a profile dump,
 * not a casual reference.
 *
 * Each pattern is counted at most once even if it appears multiple
 * times; we want pattern *diversity* as the signal. */
static int hu_guard_count_third_person_patterns(const char *s, size_t len) {
    static const char *const patterns[] = {
        " is a technical professional",
        " is a software engineer",
        " is a chief architect",
        " is a data scientist",
        " is a product manager",
        " is a senior engineer",
        " is a senior software",
        " is a software developer",
        " is a machine learning",
        "he's talking to ",
        "she's talking to ",
        "they're talking to ",
        " lives alone with",
        " lives with his ",
        " lives with her ",
        "(romantic interest",
        "(early stage relationship",
        "early stage)",
    };
    const size_t n = sizeof(patterns) / sizeof(patterns[0]);
    int hits = 0;
    for (size_t i = 0; i < n; i++) {
        if (hu_guard_ci_contains(s, len, patterns[i], strlen(patterns[i])))
            hits++;
    }
    /* Bonus rule: "<Capitalized-Name> just \"" — proper-name self-narration
     * with a quoted action. From the leak: `Seth just "glitched"`. We do
     * a single pass: find `just "` (5 chars + the quote), walk back to
     * the previous word boundary, check the word starts with an
     * uppercase letter. Counts at most once. */
    if (len >= 7) {
        for (size_t i = 0; i + 5 < len; i++) {
            if (s[i] == 'j' && s[i + 1] == 'u' && s[i + 2] == 's' && s[i + 3] == 't' &&
                s[i + 4] == ' ' && s[i + 5] == '"') {
                if (i == 0)
                    continue;
                /* Walk back to find start of prior word. */
                size_t end = i - 1;
                while (end > 0 && (s[end] == ' ' || s[end] == '\t'))
                    end--;
                if (end == 0 && (s[end] == ' ' || s[end] == '\t'))
                    continue;
                size_t start = end;
                while (start > 0 && s[start - 1] != ' ' && s[start - 1] != '\n' &&
                       s[start - 1] != '\t')
                    start--;
                if (start <= end && s[start] >= 'A' && s[start] <= 'Z') {
                    hits++;
                    break;
                }
            }
        }
    }
    return hits;
}

/* Orchestrator — returns true if the response trips ANY of G1/G2/G3.
 * Out-param `which` is set to a small bitmask for diagnostics:
 *   bit 0 = G1 numbered analysis    bit 1 = G2 self-talk
 *   bit 2 = G3 third-person double  (other bits reserved). */
static bool hu_guard_detect_semantic_leak(const char *s, size_t len, unsigned *which) {
    if (which)
        *which = 0;
    if (!s || len == 0)
        return false;
    bool hit = false;
    if (hu_guard_has_numbered_analysis_dump(s, len)) {
        if (which)
            *which |= 1u << 0;
        hit = true;
    }
    if (hu_guard_has_self_talk_pattern(s, len)) {
        if (which)
            *which |= 1u << 1;
        hit = true;
    }
    if (hu_guard_count_third_person_patterns(s, len) >= 2) {
        if (which)
            *which |= 1u << 2;
        hit = true;
    }
    return hit;
}

/* ── Internal: strip known patterns into `out` (must be ≥ in_len + 1) ── */

static void strip_special_into(const char *in, size_t in_len, char *out, size_t *out_len,
                               bool *stripped_harmony, bool *stripped_thinking) {
    size_t r = 0;
    size_t w = 0;
    *stripped_harmony = false;
    *stripped_thinking = false;

    while (r < in_len) {
        /* Try matching <think>...</think> or <thought>...</thought>. */
        if (in[r] == '<' && r + 6 < in_len) {
            const char *rest = in + r + 1;
            size_t rest_len = in_len - r - 1;
            const char *open_tag = NULL;
            const char *close_tag = NULL;
            if (rest_len >= 6 && (rest[0] == 't' || rest[0] == 'T') &&
                (rest[1] == 'h' || rest[1] == 'H') && (rest[2] == 'i' || rest[2] == 'I') &&
                (rest[3] == 'n' || rest[3] == 'N') && (rest[4] == 'k' || rest[4] == 'K') &&
                rest[5] == '>') {
                open_tag = "<think>";
                close_tag = "</think>";
            } else if (rest_len >= 8 && (rest[0] == 't' || rest[0] == 'T') &&
                       (rest[1] == 'h' || rest[1] == 'H') && (rest[2] == 'o' || rest[2] == 'O') &&
                       (rest[3] == 'u' || rest[3] == 'U') && (rest[4] == 'g' || rest[4] == 'G') &&
                       (rest[5] == 'h' || rest[5] == 'H') && (rest[6] == 't' || rest[6] == 'T') &&
                       rest[7] == '>') {
                open_tag = "<thought>";
                close_tag = "</thought>";
            }
            if (open_tag) {
                size_t open_len = strlen(open_tag);
                size_t close_len = strlen(close_tag);
                /* Find close tag (case-insensitive). */
                size_t scan = r + open_len;
                size_t found = (size_t)-1;
                while (scan + close_len <= in_len) {
                    bool match = true;
                    for (size_t k = 0; k < close_len; k++) {
                        char a = in[scan + k];
                        char b = close_tag[k];
                        if (a >= 'A' && a <= 'Z')
                            a = (char)(a + 32);
                        if (b >= 'A' && b <= 'Z')
                            b = (char)(b + 32);
                        if (a != b) {
                            match = false;
                            break;
                        }
                    }
                    if (match) {
                        found = scan;
                        break;
                    }
                    scan++;
                }
                if (found != (size_t)-1) {
                    r = found + close_len;
                    *stripped_thinking = true;
                    continue;
                }
                /* No close tag — strip from open to end of input as a
                 * defensive measure. */
                r = in_len;
                *stripped_thinking = true;
                continue;
            }
        }

        /* Try matching <|...|> (closed variant). */
        if (in[r] == '<' && r + 1 < in_len && in[r + 1] == '|') {
            /* Look for closing `|>` on the same logical line. */
            size_t scan = r + 2;
            size_t close = (size_t)-1;
            while (scan + 1 < in_len) {
                if (in[scan] == '|' && in[scan + 1] == '>') {
                    close = scan;
                    break;
                }
                if (in[scan] == '\n' || in[scan] == '\r')
                    break;
                scan++;
            }
            if (close != (size_t)-1) {
                r = close + 2;
                *stripped_harmony = true;
                continue;
            }
            /* Unclosed `<|TAG` (production leak shape: `<|channel>thought`).
             * Consume `<|`, then the tag identifier `[a-zA-Z_]+`,
             * optionally a `>`, and — if the post-`>` text begins with a
             * known Harmony channel value (`thought`, `final`, `analysis`,
             * `commentary`), consume that too. This swallows the spilled
             * token sequence without touching the user-visible content
             * that follows. */
            scan = r + 2;
            size_t tag_start = scan;
            while (scan < in_len) {
                char c = in[scan];
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')
                    scan++;
                else
                    break;
            }
            if (scan > tag_start) {
                /* Optional `>` (unclosed marker form). */
                if (scan < in_len && in[scan] == '>')
                    scan++;
                /* Optional Harmony channel value immediately after. */
                static const char *const channel_values[] = {"thought", "final", "analysis",
                                                             "commentary"};
                size_t cv_count = sizeof(channel_values) / sizeof(channel_values[0]);
                for (size_t k = 0; k < cv_count; k++) {
                    size_t cv_len = strlen(channel_values[k]);
                    if (scan + cv_len <= in_len &&
                        memcmp(in + scan, channel_values[k], cv_len) == 0) {
                        scan += cv_len;
                        break;
                    }
                }
                r = scan;
                *stripped_harmony = true;
                continue;
            }
        }

        out[w++] = in[r++];
    }
    out[w] = '\0';
    *out_len = w;

    /* Trim trailing whitespace introduced by stripping. */
    while (*out_len > 0 &&
           (out[*out_len - 1] == ' ' || out[*out_len - 1] == '\t' ||
            out[*out_len - 1] == '\n' || out[*out_len - 1] == '\r')) {
        out[--(*out_len)] = '\0';
    }
    /* Trim leading whitespace too. */
    size_t lead = 0;
    while (lead < *out_len && (out[lead] == ' ' || out[lead] == '\t')) {
        lead++;
    }
    if (lead > 0 && lead < *out_len) {
        memmove(out, out + lead, *out_len - lead);
        *out_len -= lead;
        out[*out_len] = '\0';
    } else if (lead == *out_len && lead > 0) {
        *out_len = 0;
        out[0] = '\0';
    }
}

/* ── Public entry point ─────────────────────────────────────────────────── */

hu_error_t hu_response_guard_check(hu_allocator_t *alloc, const char *response, size_t response_len,
                                   char **out_response, size_t *out_len,
                                   hu_guard_outcome_t *out_outcome, hu_guard_report_t *report) {
    if (!alloc || !response || !out_response || !out_len || !out_outcome)
        return HU_ERR_INVALID_ARGUMENT;

    *out_response = NULL;
    *out_len = 0;
    *out_outcome = HU_GUARD_OK;
    if (report)
        memset(report, 0, sizeof(*report));

    if (response_len == 0) {
        *out_response = (char *)response;
        *out_len = 0;
        *out_outcome = HU_GUARD_OK;
        return HU_OK;
    }

    /* Phase 0 — Gemma-4 untagged reasoning strip.
     *
     * Gemma-4 (and some other thinking models served via mlx_lm) emit their
     * chain-of-thought as plain-text bullet analysis at the START of the
     * response, without <think> tags.  The pattern looks like:
     *
     *   \n*   User: "Hey you!"
     *       *   Persona: Seth Douglas Ford (51, Chief Architect, t…
     *       *   Context: Annie (sister) is teasing Seth. This…
     *   <blank line>
     *   Actual reply text here
     *
     * We detect this by checking if the (whitespace-trimmed) response opens
     * with "* " (asterisk + space).  If it does, we scan forward for the
     * first line that does NOT start with whitespace or "*"; that is the
     * start of the real reply.  If no such line exists the entire response
     * is reasoning — reject it so the daemon retries. */
    const char *effective = response;
    size_t effective_len = response_len;

    /* Skip leading whitespace. */
    size_t skip_ws = 0;
    while (skip_ws < response_len &&
           (response[skip_ws] == ' ' || response[skip_ws] == '\t' ||
            response[skip_ws] == '\n' || response[skip_ws] == '\r'))
        skip_ws++;

    bool stripped_reasoning_prefix = false;

    if (skip_ws < response_len && response[skip_ws] == '*' &&
        skip_ws + 1 < response_len && response[skip_ws + 1] == ' ') {
        /* Looks like untagged reasoning.  Walk line-by-line to find where
         * the actual reply begins. A "reasoning line" either starts with
         * `*` or is indented (leading spaces/tabs).  The first
         * non-blank line that starts with an alphanumeric or common
         * punctuation (anything except * or whitespace) is the reply. */
        size_t reply_start = response_len;
        size_t i = skip_ws;
        while (i < response_len) {
            /* Find start of next line (skip current line). */
            while (i < response_len && response[i] != '\n')
                i++;
            if (i < response_len)
                i++; /* skip the newline */
            /* Skip blank lines. */
            size_t line_start = i;
            while (line_start < response_len &&
                   (response[line_start] == ' ' || response[line_start] == '\t'))
                line_start++;
            if (line_start >= response_len)
                break;
            if (response[line_start] == '\n' || response[line_start] == '\r') {
                i = line_start;
                continue;
            }
            /* If this line starts with `*` it's still reasoning. */
            if (response[line_start] == '*') {
                i = line_start;
                continue;
            }
            /* This line is the start of the actual reply. */
            reply_start = line_start;
            break;
        }

        if (reply_start < response_len) {
            effective = response + reply_start;
            effective_len = response_len - reply_start;
            stripped_reasoning_prefix = true;
        } else {
            /* Entire response is reasoning — no reply text. Reject. */
            *out_response = NULL;
            *out_len = 0;
            *out_outcome = HU_GUARD_REJECT;
            if (report) {
                memset(report, 0, sizeof(*report));
                report->stripped_thinking_block = true;
                report->bytes_stripped = response_len;
            }
            return HU_OK;
        }
    }

    /* Phase 1 — special-token strip. Always run; cheap. */
    bool needs_strip = hu_response_guard_has_special_token(effective, effective_len);

    char *cleaned = NULL;
    size_t cleaned_len = effective_len;
    bool stripped_harmony = false;
    bool stripped_thinking = false;

    if (needs_strip) {
        cleaned = (char *)alloc->alloc(alloc->ctx, effective_len + 1);
        if (!cleaned)
            return HU_ERR_OUT_OF_MEMORY;
        strip_special_into(effective, effective_len, cleaned, &cleaned_len, &stripped_harmony,
                           &stripped_thinking);
    }

    const char *for_repetition_check = needs_strip ? cleaned : effective;
    size_t check_len = needs_strip ? cleaned_len : effective_len;

    /* Phase 2 — degenerate repetition detection. */
    size_t char_run = hu_response_guard_longest_char_run(for_repetition_check, check_len);
    size_t tok_run = hu_response_guard_longest_token_run(for_repetition_check, check_len);
    bool degenerate = (char_run > HU_GUARD_MAX_CHAR_RUN) || (tok_run > HU_GUARD_MAX_TOKEN_RUN);

    if (report) {
        report->stripped_harmony_tokens = stripped_harmony;
        report->stripped_thinking_block = stripped_thinking || stripped_reasoning_prefix;
        report->detected_degenerate_repetition = degenerate;
        report->max_repetition_run = char_run > tok_run ? char_run : tok_run;
        size_t prefix_bytes = stripped_reasoning_prefix ? (response_len - effective_len) : 0;
        report->bytes_stripped =
            prefix_bytes + (needs_strip ? (effective_len - cleaned_len) : 0);
    }

    if (degenerate) {
        if (cleaned)
            alloc->free(alloc->ctx, cleaned, response_len + 1);
        *out_response = NULL;
        *out_len = 0;
        *out_outcome = HU_GUARD_REJECT;
        return HU_OK;
    }

    /* Phase 3 (Sprint 29) — semantic leak detection. Catches CoT /
     * prompt-context dumps that earlier phases miss because the leak
     * has no markup signature and no repetition signature.
     *
     * Production root-cause: 2026-05-12 leak via the live service-loop
     * daemon. The primary guard returned REWROTE (stripped 82 bytes of
     * leading whitespace) and let 897 bytes of model self-narration
     * pass to a real human contact. Phase 3 inspects the cleaned text
     * for three structural signatures (G1/G2/G3) and rejects on any
     * single hit.
     *
     * See `docs/postmortems/2026-05-12-cot-leak.md`. */
    unsigned semantic_which = 0;
    bool semantic_leak =
        hu_guard_detect_semantic_leak(for_repetition_check, check_len, &semantic_which);
    if (semantic_leak) {
        if (cleaned)
            alloc->free(alloc->ctx, cleaned, effective_len + 1);
        *out_response = NULL;
        *out_len = 0;
        *out_outcome = HU_GUARD_REJECT;
        if (report)
            report->detected_semantic_leak = true;
        return HU_OK;
    }

    if (needs_strip) {
        /* If everything was stripped away, treat as reject — the model
         * sent ONLY tokens, no real content. */
        if (cleaned_len == 0) {
            alloc->free(alloc->ctx, cleaned, effective_len + 1);
            *out_response = NULL;
            *out_len = 0;
            *out_outcome = HU_GUARD_REJECT;
            if (report)
                report->detected_degenerate_repetition = true;
            return HU_OK;
        }
        *out_response = cleaned;
        *out_len = cleaned_len;
        *out_outcome = HU_GUARD_REWROTE;
        return HU_OK;
    }

    if (stripped_reasoning_prefix) {
        /* Reasoning prefix was stripped — need to return a copy since
         * effective points into the middle of the original response. */
        char *copy = (char *)alloc->alloc(alloc->ctx, effective_len + 1);
        if (!copy)
            return HU_ERR_OUT_OF_MEMORY;
        memcpy(copy, effective, effective_len);
        copy[effective_len] = '\0';
        *out_response = copy;
        *out_len = effective_len;
        *out_outcome = HU_GUARD_REWROTE;
        return HU_OK;
    }

    *out_response = (char *)response;
    *out_len = response_len;
    *out_outcome = HU_GUARD_OK;
    return HU_OK;
}
