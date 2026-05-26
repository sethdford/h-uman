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
#include "human/core/log.h"
#include "human/observer.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
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

/* Sprint 31 — context-aware detection thresholds.
 *
 * Length anomaly multiplier. Calibrated against the 2026-05-12 Brea
 * leak (979 char response vs 44 char rolling avg = 22x). A
 * "long-but-legit" reply (story telling, link with explanation,
 * detailed answer) is typically 3-5x the rolling avg. 8x is the safe
 * midpoint that catches the leak with margin.
 *
 * Director echo minimum match length. The leak quoted 64 verbatim
 * chars from the director string. 30 chars is shorter than that (so
 * we catch it) and longer than incidental phrase matches like
 * "I think we should" that happen to appear in both director and
 * reply for normal reasons. */
#define HU_GUARD_LENGTH_ANOMALY_MULT     HU_GUARD_LENGTH_ANOMALY_MULT_DEFAULT
#define HU_GUARD_DIRECTOR_ECHO_MIN_MATCH 30

/* Persona identity echo minimum match length. Persona identity strings
 * are typically 30-60 chars ("Chief Architect at Pure Health Solutions"
 * = 40, "51-year-old technical professional, lives alone with a cat" =
 * 58). 25 chars is below typical identity length (catches verbatim
 * quotes) and above common-phrase floor ("thanks for the message" = 22)
 * to avoid incidental overlap. Calibrated against the 2026-05-12 audit:
 * every identity-class leak quoted ≥ 30 contiguous chars. */
#define HU_GUARD_PERSONA_IDENTITY_MIN_MATCH 25

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
                (s[i + 2] == 'h' || s[i + 2] == 'H') && (s[i + 3] == 'i' || s[i + 3] == 'I') &&
                (s[i + 4] == 'n' || s[i + 4] == 'N') && (s[i + 5] == 'k' || s[i + 5] == 'K') &&
                s[i + 6] == '>')
                return true;
        }
    }
    if (len >= 9) {
        for (size_t i = 0; i + 8 < len; i++) {
            if (s[i] == '<' && (s[i + 1] == 't' || s[i + 1] == 'T') &&
                (s[i + 2] == 'h' || s[i + 2] == 'H') && (s[i + 3] == 'o' || s[i + 3] == 'O') &&
                (s[i + 4] == 'u' || s[i + 4] == 'U') && (s[i + 5] == 'g' || s[i + 5] == 'G') &&
                (s[i + 6] == 'h' || s[i + 6] == 'H') && (s[i + 7] == 't' || s[i + 7] == 'T') &&
                s[i + 8] == '>')
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

/* G2 + G4 — model self-talk + prompt-template label leak.
 *
 * These patterns have no legitimate human-to-human reply use case. A
 * single hit is a hard reject. Two classes:
 *
 *   G2 (Sprint 29) — model self-talk / scene-direction echo. All six
 *   were present in the 2026-05-12 17:04:37 Brea leak (rowid 56354).
 *   None can plausibly appear in a normal text message to a friend.
 *
 *   G4 (Sprint 30) — prompt-template label leak. Found in chat.db
 *   audit after Sprint 29 closed. Two May-11 leaks (rowid 56055,
 *   56065) to a third contact dumped the literal prompt template
 *   (User:/Context:/Persona:/Scene Direction:/Rules:/Constraints:)
 *   plus candidate response drafts to the recipient. These do not
 *   trip G1 (no numbered list), G2 (no self-talk substrings), or G3
 *   (only one third-person hit). G4 catches them on the literal
 *   template-label substrings.
 *
 * Both classes are merged into a single substring table since the
 * detection logic is identical (case-insensitive contains → REJECT). */
static bool hu_guard_has_self_talk_pattern(const char *s, size_t len) {
    static const struct {
        const char *pat;
        size_t pat_len;
    } patterns[] = {
        /* G2 (Sprint 29) — model self-talk. */
        {"the prompt says", 15},
        {"wait, the prompt", 16},
        {"i should still maintain", 23},
        {"(per scene direction", 20},
        {"per the scene direction", 23},
        {"the user is bombarding", 22},
        /* G4 (Sprint 30) — prompt-template labels. Each is a verbatim
         * substring from one of the 4 audit-discovered leaks. The
         * `\n` prefix on most ensures we match line-prefix style
         * (mid-sentence "the persona: warm" wouldn't trip on the
         * leading-newline form). */
        {"\nPersona:", 9},
        {" Persona:", 9},
        {"\nScene Direction:", 17},
        {" Scene Direction:", 17},
        {"\nUser: \"", 8},
        {" User: \"", 8},
        {"\nRules: All lowercase", 21},
        {" Rules: All lowercase", 21},
        {"\nConstraints: All lowercase", 27},
        {" Constraints: All lowercase", 27},
        {"\nSystem prompt:", 15},
        {" System prompt:", 15},
        /* NOTE: we do NOT hardcode the persona's name / title / age
         * here even though they appeared verbatim in the audit leaks.
         * Hardcoding PII into open-source guard code is a leak in
         * its own right, and the structural template-label patterns
         * above already match every audit leak 4+ times. A future
         * sprint should make this dynamic by reading the loaded
         * persona's biographical fields and rejecting any response
         * that quotes them verbatim — that generalizes to every
         * user's deployment without baking specific names into
         * source control. */
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

/* G5 — length anomaly. Returns true if `response_len` exceeds
 * `ctx->recent_avg_len * HU_GUARD_LENGTH_ANOMALY_MULT`. A NULL ctx or
 * `recent_avg_len == 0` disables the check (returns false). */
static bool guard_channel_prefix_ci(const char *ch, size_t ch_len, const char *prefix) {
    size_t plen = strlen(prefix);
    if (ch_len < plen)
        return false;
    for (size_t i = 0; i < plen; i++) {
        char a = ch[i];
        char b = prefix[i];
        if (a >= 'A' && a <= 'Z')
            a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z')
            b = (char)(b + 32);
        if (a != b)
            return false;
    }
    /* "imessage:thread-7" or "cli" exact. */
    return ch_len == plen || ch[plen] == ':' || ch[plen] == '_';
}

unsigned hu_guard_length_anomaly_mult_for_channel(const char *channel, size_t channel_len) {
    static const char *compact[] = {"imessage", "cli", "sms", NULL};
    if (!channel || channel_len == 0)
        return HU_GUARD_LENGTH_ANOMALY_MULT_DEFAULT;
    for (size_t k = 0; compact[k]; k++) {
        if (guard_channel_prefix_ci(channel, channel_len, compact[k]))
            return HU_GUARD_LENGTH_ANOMALY_MULT_COMPACT;
    }
    return HU_GUARD_LENGTH_ANOMALY_MULT_DEFAULT;
}

static bool hu_guard_has_length_anomaly(const hu_guard_context_t *ctx, size_t response_len) {
    if (!ctx || ctx->recent_avg_len == 0)
        return false;
    unsigned mult = ctx->length_anomaly_mult;
    if (mult == 0)
        mult = HU_GUARD_LENGTH_ANOMALY_MULT_DEFAULT;
    return response_len > ctx->recent_avg_len * (size_t)mult;
}

/* Helper — slide a 30-char window over `src[0..src_len)` and return
 * true if any window appears verbatim (case-insensitively) in
 * `s[0..len)`. Skips when src is NULL/short or response is short. */
static bool hu_guard_director_window_matches(const char *src, size_t src_len, const char *s,
                                             size_t len) {
    if (!src || src_len < (size_t)HU_GUARD_DIRECTOR_ECHO_MIN_MATCH)
        return false;
    if (len < (size_t)HU_GUARD_DIRECTOR_ECHO_MIN_MATCH)
        return false;
    size_t window = (size_t)HU_GUARD_DIRECTOR_ECHO_MIN_MATCH;
    for (size_t i = 0; i + window <= src_len; i++) {
        if (hu_guard_ci_contains(s, len, src + i, window))
            return true;
    }
    return false;
}

/* G6 — director-string echo. Returns true if any 30-char substring of
 * `ctx->director_text` (current turn) OR any entry in
 * `ctx->director_history[]` (past turns, most-recent-first) appears
 * verbatim (case-insensitively) in `s[0..len)`. NULL ctx disables the
 * check entirely. Per-source skip rules:
 *
 *   - current `director_text` skipped if NULL or director_len < 30.
 *   - history slot skipped if its entry is NULL or its length < 30.
 *
 * Returns on first match (any source), so cost is bounded by
 * HU_DIRECTOR_HISTORY_MAX + 1 sliding-window scans. */
static bool hu_guard_has_director_echo(const hu_guard_context_t *ctx, const char *s, size_t len) {
    if (!ctx)
        return false;
    /* Current-turn director (Sprint 31 baseline). */
    if (hu_guard_director_window_matches(ctx->director_text, ctx->director_len, s, len))
        return true;
    /* Cross-turn history (Sprint 37). */
    if (ctx->director_history && ctx->director_history_lens && ctx->director_history_count > 0) {
        for (size_t i = 0; i < ctx->director_history_count; i++) {
            if (hu_guard_director_window_matches(ctx->director_history[i],
                                                 ctx->director_history_lens[i], s, len))
                return true;
        }
    }
    return false;
}

/* G7 — persona-PII echo. Returns true if `ctx->persona_name` appears
 * verbatim (case-insensitively) in `s[0..len)` followed within
 * HU_GUARD_PERSONA_LOOKAHEAD bytes by a third-person profile
 * construct: " is ", " was ", " has ", " lives ", " works ", " said ",
 * " would ", or possessive "'s ".
 *
 * Word-boundary aware: the name must be preceded by start-of-string or
 * a non-letter character. Without this, "Bethseth" would match "Seth"
 * and a model writing "ethel said" would trip on "Eth" as a name.
 *
 * NULL ctx, NULL persona_name, persona_name_len < 2 or > 64, or empty
 * response disable the check. */
/* Sprint 39 — widened from 30→60 to catch "Seth, 51, is a Chief Architect"
 * style parentheticals between name and third-person verb (audit rowid 56055). */
#define HU_GUARD_PERSONA_LOOKAHEAD 60

static bool hu_guard_persona_pii_construct_at(const char *s, size_t len, size_t pos) {
    /* Look for a third-person construct starting at `pos`. We accept up
     * to HU_GUARD_PERSONA_LOOKAHEAD bytes of slack between the name and
     * the construct — covers cases like `"Seth, 51, is a ..."`. */
    if (pos >= len)
        return false;
    size_t scan_end = pos + HU_GUARD_PERSONA_LOOKAHEAD;
    if (scan_end > len)
        scan_end = len;

    /* Possessive: <Name>'s — the apostrophe must be at pos exactly. */
    if (pos < len && s[pos] == '\'' && pos + 2 < len && (s[pos + 1] == 's' || s[pos + 1] == 'S') &&
        s[pos + 2] == ' ')
        return true;

    /* Sliding window for verbs. Patterns include the leading space. */
    static const char *constructs[] = {
        " is a ",       " is an ",  " is the ",   " is currently", " was a ", " was an ",
        " was the ",    " has a ",  " has an ",   " has been ",    " lives ", " lives in ",
        " lives alone", " works ",  " works at ", " works as ",    " said ",  " would ",
        " prefers ",    " enjoys ", " likes ",
    };
    static const size_t n_constructs = sizeof(constructs) / sizeof(constructs[0]);

    for (size_t p = pos; p < scan_end; p++) {
        for (size_t k = 0; k < n_constructs; k++) {
            size_t clen = strlen(constructs[k]);
            if (p + clen > len)
                continue;
            /* Case-insensitive prefix match. */
            bool match = true;
            for (size_t j = 0; j < clen; j++) {
                char a = s[p + j];
                char b = constructs[k][j];
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
    }
    return false;
}

static bool hu_guard_has_persona_pii_echo(const hu_guard_context_t *ctx, const char *s,
                                          size_t len) {
    if (!ctx || !ctx->persona_name || ctx->persona_name_len < 2 || ctx->persona_name_len > 64)
        return false;
    if (!s || len < ctx->persona_name_len + 4) /* need name + " is " minimum */
        return false;

    size_t nlen = ctx->persona_name_len;
    /* Find each case-insensitive occurrence of the name with word
     * boundaries on both sides. */
    for (size_t i = 0; i + nlen <= len; i++) {
        /* Left boundary: start-of-string or non-letter. */
        if (i > 0) {
            char prev = s[i - 1];
            bool prev_letter = (prev >= 'a' && prev <= 'z') || (prev >= 'A' && prev <= 'Z');
            if (prev_letter)
                continue;
        }
        /* Case-insensitive name match. */
        bool name_match = true;
        for (size_t j = 0; j < nlen; j++) {
            char a = s[i + j];
            char b = ctx->persona_name[j];
            if (a >= 'A' && a <= 'Z')
                a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z')
                b = (char)(b + 32);
            if (a != b) {
                name_match = false;
                break;
            }
        }
        if (!name_match)
            continue;
        /* Right boundary: end-of-string or non-letter. */
        if (i + nlen < len) {
            char next = s[i + nlen];
            bool next_letter = (next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z');
            if (next_letter)
                continue;
        }
        /* Word-boundary-isolated occurrence at [i, i+nlen). Look for a
         * third-person construct starting at i+nlen. */
        if (hu_guard_persona_pii_construct_at(s, len, i + nlen))
            return true;
    }
    return false;
}

/* G8 helper — verbatim substring echo for any persona text blob. */
static bool hu_guard_persona_text_echo(const char *text, size_t text_len, const char *s,
                                       size_t len) {
    if (!text || text_len < (size_t)HU_GUARD_PERSONA_IDENTITY_MIN_MATCH)
        return false;
    if (len < (size_t)HU_GUARD_PERSONA_IDENTITY_MIN_MATCH)
        return false;
    size_t window = (size_t)HU_GUARD_PERSONA_IDENTITY_MIN_MATCH;
    for (size_t i = 0; i + window <= text_len; i++) {
        if (hu_guard_ci_contains(s, len, text + i, window))
            return true;
    }
    return false;
}

/* G8 — persona identity / biography echo. Returns true if any
 * contiguous 25-byte substring of `persona_identity` OR
 * `persona_biography` appears verbatim in `s[0..len)`. */
static bool hu_guard_has_persona_identity_echo(const hu_guard_context_t *ctx, const char *s,
                                               size_t len) {
    if (!ctx)
        return false;
    if (hu_guard_persona_text_echo(ctx->persona_identity, ctx->persona_identity_len, s, len))
        return true;
    if (hu_guard_persona_text_echo(ctx->persona_biography, ctx->persona_biography_len, s, len))
        return true;
    return false;
}

/* Sprint 38 — process-wide REJECT telemetry (G5–G9). */
static atomic_uint_fast64_t s_guard_stat_semantic;
static atomic_uint_fast64_t s_guard_stat_length;
static atomic_uint_fast64_t s_guard_stat_director;
static atomic_uint_fast64_t s_guard_stat_persona_pii;
static atomic_uint_fast64_t s_guard_stat_persona_identity;
/* Sprint 41 — G9 naked discourse-marker opener. */
static atomic_uint_fast64_t s_guard_stat_naked_opener;

/* Sprint 41 follow-up #3 — G9 retry-outcome breakdown. */
static atomic_uint_fast64_t s_guard_stat_g9_retry_rescued;
static atomic_uint_fast64_t s_guard_stat_g9_retry_thrashed;
static atomic_uint_fast64_t s_guard_stat_g9_retry_starved;

/* Sprint 41 follow-up — process-wide G9 kill switch. */
static atomic_bool s_g9_globally_disabled;

void hu_response_guard_set_naked_opener_globally_disabled(bool disabled) {
    atomic_store_explicit(&s_g9_globally_disabled, disabled, memory_order_relaxed);
}

bool hu_response_guard_naked_opener_globally_disabled(void) {
    return atomic_load_explicit(&s_g9_globally_disabled, memory_order_relaxed);
}

/* Sprint 41 follow-up #4 — per-channel G9 disable list.
 *
 * The list is small (production channel count is single digits), so a
 * linear scan is fine and we don't need a hash table. Process-private
 * storage: a heap-allocated array of dup'd channel strings + a count.
 * Both protected by a mutex around the writer path; readers do a
 * snapshot under the lock and scan the snapshot lock-free.
 *
 * Liveness contract: hu_response_guard_g9_disabled_for_channel can be
 * called from any thread at any time. The setter is called only from
 * daemon startup and config-reload paths, both of which are
 * single-threaded with respect to the response_guard subsystem.
 *
 * To keep the lock-free fast path simple, we use atomic load/store of a
 * pointer-to-array pair, with a tiny strdup arena per snapshot. */
#include <stdlib.h>

typedef struct {
    char **names;
    size_t count;
} g9_channel_list_t;

static atomic_uintptr_t s_g9_disabled_channels; /* g9_channel_list_t * */

static void g9_channel_list_free(g9_channel_list_t *l) {
    if (!l)
        return;
    for (size_t i = 0; i < l->count; i++)
        free(l->names[i]);
    free(l->names);
    free(l);
}

void hu_response_guard_set_g9_disabled_channels(const char *const *channels, size_t count) {
    g9_channel_list_t *new_list = NULL;
    if (count > 0 && channels) {
        new_list = (g9_channel_list_t *)calloc(1, sizeof(*new_list));
        if (!new_list)
            return;
        new_list->names = (char **)calloc(count, sizeof(char *));
        if (!new_list->names) {
            free(new_list);
            return;
        }
        for (size_t i = 0; i < count; i++) {
            if (channels[i]) {
                new_list->names[new_list->count] = strdup(channels[i]);
                if (new_list->names[new_list->count])
                    new_list->count++;
            }
        }
    }
    uintptr_t old = atomic_exchange_explicit(&s_g9_disabled_channels, (uintptr_t)new_list,
                                             memory_order_acq_rel);
    g9_channel_list_free((g9_channel_list_t *)old);
}

bool hu_response_guard_g9_disabled_for_channel(const char *channel, size_t channel_len) {
    if (!channel || channel_len == 0)
        return false;
    g9_channel_list_t *snap =
        (g9_channel_list_t *)atomic_load_explicit(&s_g9_disabled_channels, memory_order_acquire);
    if (!snap || snap->count == 0)
        return false;
    /* Case-insensitive whole-name match. Channel names in production are
     * short ASCII slugs ("imessage", "voice", "discord") so strncasecmp
     * is fine. */
    for (size_t i = 0; i < snap->count; i++) {
        const char *name = snap->names[i];
        if (!name)
            continue;
        size_t nlen = strlen(name);
        if (nlen == channel_len && strncasecmp(name, channel, channel_len) == 0)
            return true;
    }
    return false;
}

static void hu_guard_record_reject_stats(const hu_guard_report_t *report) {
    if (!report)
        return;
    if (report->detected_semantic_leak)
        atomic_fetch_add_explicit(&s_guard_stat_semantic, 1, memory_order_relaxed);
    if (report->detected_length_anomaly)
        atomic_fetch_add_explicit(&s_guard_stat_length, 1, memory_order_relaxed);
    if (report->detected_director_echo)
        atomic_fetch_add_explicit(&s_guard_stat_director, 1, memory_order_relaxed);
    if (report->detected_persona_pii_echo)
        atomic_fetch_add_explicit(&s_guard_stat_persona_pii, 1, memory_order_relaxed);
    if (report->detected_persona_identity_echo)
        atomic_fetch_add_explicit(&s_guard_stat_persona_identity, 1, memory_order_relaxed);
    if (report->detected_naked_discourse_opener)
        atomic_fetch_add_explicit(&s_guard_stat_naked_opener, 1, memory_order_relaxed);
}

void hu_guard_reject_stats_snapshot(hu_guard_reject_stats_t *out) {
    if (!out)
        return;
    out->semantic_leak = atomic_load_explicit(&s_guard_stat_semantic, memory_order_relaxed);
    out->length_anomaly = atomic_load_explicit(&s_guard_stat_length, memory_order_relaxed);
    out->director_echo = atomic_load_explicit(&s_guard_stat_director, memory_order_relaxed);
    out->persona_pii_echo = atomic_load_explicit(&s_guard_stat_persona_pii, memory_order_relaxed);
    out->persona_identity_echo =
        atomic_load_explicit(&s_guard_stat_persona_identity, memory_order_relaxed);
    out->naked_discourse_opener =
        atomic_load_explicit(&s_guard_stat_naked_opener, memory_order_relaxed);
    out->g9_retry_rescued =
        atomic_load_explicit(&s_guard_stat_g9_retry_rescued, memory_order_relaxed);
    out->g9_retry_thrashed =
        atomic_load_explicit(&s_guard_stat_g9_retry_thrashed, memory_order_relaxed);
    out->g9_retry_starved =
        atomic_load_explicit(&s_guard_stat_g9_retry_starved, memory_order_relaxed);
}

void hu_guard_reject_stats_reset(void) {
    atomic_store_explicit(&s_guard_stat_semantic, 0, memory_order_relaxed);
    atomic_store_explicit(&s_guard_stat_length, 0, memory_order_relaxed);
    atomic_store_explicit(&s_guard_stat_director, 0, memory_order_relaxed);
    atomic_store_explicit(&s_guard_stat_persona_pii, 0, memory_order_relaxed);
    atomic_store_explicit(&s_guard_stat_persona_identity, 0, memory_order_relaxed);
    atomic_store_explicit(&s_guard_stat_naked_opener, 0, memory_order_relaxed);
    atomic_store_explicit(&s_guard_stat_g9_retry_rescued, 0, memory_order_relaxed);
    atomic_store_explicit(&s_guard_stat_g9_retry_thrashed, 0, memory_order_relaxed);
    atomic_store_explicit(&s_guard_stat_g9_retry_starved, 0, memory_order_relaxed);
}

void hu_response_guard_record_g9_retry_outcome(bool retry_succeeded, bool retry_tripped_g9_again) {
    if (!retry_succeeded) {
        atomic_fetch_add_explicit(&s_guard_stat_g9_retry_starved, 1, memory_order_relaxed);
        return;
    }
    if (retry_tripped_g9_again) {
        atomic_fetch_add_explicit(&s_guard_stat_g9_retry_thrashed, 1, memory_order_relaxed);
        return;
    }
    atomic_fetch_add_explicit(&s_guard_stat_g9_retry_rescued, 1, memory_order_relaxed);
}

bool hu_guard_audit_numbered_analysis_dump(const char *s, size_t len) {
    return hu_guard_has_numbered_analysis_dump(s, len);
}

bool hu_guard_audit_self_talk_leak(const char *s, size_t len) {
    return hu_guard_has_self_talk_pattern(s, len);
}

bool hu_response_is_critique_echo(const char *s, size_t len) {
    /* The reflection-retry critique always begins with a verdict token
     * from reflection.c's prompt template ("NEEDS_RETRY" or lowercase).
     * When the LLM echoes the critique structure as its response, the
     * resulting string begins with one of those tokens. Test the prefix
     * only — embedded mentions in the middle of a legitimate response
     * are NOT echoes (e.g. "I evaluated the NEEDS_RETRY case earlier"
     * is fine). Matches the dpo write-side guard in
     * src/ml/dpo.c::hu_dpo_record_from_retry. */
    if (!s || len < 11)
        return false;
    if (memcmp(s, "NEEDS_RETRY", 11) == 0)
        return true;
    if (memcmp(s, "needs_retry", 11) == 0)
        return true;
    return false;
}

/* Sprint 41 (2026-05-26 Jordan incident) — naked discourse-marker opener.
 *
 * The discourse markers below ("tbh", "ngl", etc.) function as backchannel
 * cues in real speech — they presuppose a prior proposition to contrast or
 * soften ("the food was fine, tbh I expected worse"). When a LoRA adapter
 * over-fits on raw casual-texting corpus, it learns these markers as
 * high-probability sentence-openers without learning the discourse
 * constraint that something prior is required. The model then emits
 * "tbh morning. you awake yet?" — an unsemantic concatenation that reads
 * as obviously machine-generated to any human reader.
 *
 * The greeting nouns below ("morning", "hey", "yo") are interpersonal
 * openers and cannot serve as the "prior proposition" a discourse marker
 * contrasts with. So <MARKER> <GREETING> with no completing clause is
 * always malformed.
 *
 * The check is conservative: we allow marker+greeting when the greeting
 * is followed by a copula-style verb that turns the greeting into a
 * subject of a real opinion ("tbh morning IS the worst"). */
static int g9_ieq(const char *a, const char *b, size_t n) {
    /* Case-insensitive byte-wise compare. ASCII only — matches Seth's
     * texting register (no localized markers). */
    for (size_t i = 0; i < n; i++) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb + 32);
        if (ca != cb)
            return 0;
    }
    return 1;
}

static int g9_is_word_boundary(char c) {
    /* Non-alphanumeric chars (including punctuation, whitespace, EOL)
     * are word boundaries. Matches the substring-classifier-pitfalls
     * rule's _word_ci convention. */
    return !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'));
}

bool hu_response_is_naked_discourse_opener(const char *s, size_t len) {
    if (!s || len == 0)
        return false;

    /* Skip leading whitespace — some providers emit "\n\n" before the
     * actual reply, and the rest of the guard runs on `for_repetition_check`
     * which is pre-whitespace-trimmed by Phase 0 but legacy callers may
     * pass untrimmed text. */
    size_t i = 0;
    while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
        i++;
    if (i >= len)
        return false;

    /* Match a discourse marker at the start. Ordered longest-first so
     * "imho" doesn't get partial-matched as "imo" + "ho". */
    static const char *marker_list[] = {
        "deadass", "honestly", "highkey", "lowkey", "frfr", "imho", "lmao",
        "fwiw",    "tbh",      "ngl",     "imo",    "fr",   "lol",  NULL,
    };
    size_t marker_len = 0;
    for (const char **m = marker_list; *m; m++) {
        size_t mlen = strlen(*m);
        if (i + mlen > len)
            continue;
        if (g9_ieq(s + i, *m, mlen) && (i + mlen == len || g9_is_word_boundary(s[i + mlen]))) {
            marker_len = mlen;
            break;
        }
    }
    if (marker_len == 0)
        return false;

    /* Skip the marker + any whitespace/comma between marker and greeting. */
    i += marker_len;
    while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == ',')) {
        i++;
    }
    if (i >= len)
        return false; /* "tbh" alone — not a naked-opener case */

    /* Match a bare greeting noun at the new position. */
    static const char *greeting_list[] = {
        "afternoon", "evening", "morning", "howdy", "hello", "hola",
        "night",     "sup",     "hey",     "yo",    "hi",    NULL,
    };
    size_t greet_len = 0;
    for (const char **g = greeting_list; *g; g++) {
        size_t glen = strlen(*g);
        if (i + glen > len)
            continue;
        if (g9_ieq(s + i, *g, glen) && (i + glen == len || g9_is_word_boundary(s[i + glen]))) {
            greet_len = glen;
            break;
        }
    }
    if (greet_len == 0)
        return false; /* marker not followed by a greeting → not this pattern */

    /* If the greeting ends the message, that's the canonical naked case. */
    size_t after = i + greet_len;
    if (after >= len)
        return true;

    /* Punctuation immediately after the greeting (".", "!", "?", ",") +
     * follow-up content is the production failure shape — "tbh morning.
     * you awake yet?". REJECT. */
    if (s[after] == '.' || s[after] == '!' || s[after] == '?' || s[after] == ',')
        return true;

    /* Otherwise the greeting is followed by whitespace + more text. Skip
     * the whitespace and look at the next token: if it's a copula-style
     * verb, the marker is pragmatically valid ("tbh morning IS the worst").
     * Anything else is a malformed opener ("tbh morning you up?"). */
    size_t k = after;
    while (k < len && (s[k] == ' ' || s[k] == '\t'))
        k++;
    if (k >= len)
        return true; /* trailing whitespace only — same as bare greeting */

    static const char *clause_verb_list[] = {
        "sounded", "sounds", "seemed", "seems", "sucks", "feels", "felt",
        "is",      "was",    "are",    "were",  "ain't", NULL,
    };
    for (const char **v = clause_verb_list; *v; v++) {
        size_t vlen = strlen(*v);
        if (k + vlen > len)
            continue;
        if (g9_ieq(s + k, *v, vlen) && (k + vlen == len || g9_is_word_boundary(s[k + vlen])))
            return false; /* valid: "tbh morning is the worst" */
    }
    return true; /* no clause verb → naked opener */
}

void hu_guard_log_selection_audit(const void *observer, const char *contact_key,
                                  size_t contact_key_len, size_t candidate_count, size_t best_idx,
                                  int best_quality, size_t response_len, const char *response,
                                  size_t response_text_len) {
    hu_observer_t *obs = (hu_observer_t *)observer;
    int preview_n = (int)(response_text_len < 48 ? response_text_len : 48);
    hu_log_info("response_guard", obs,
                "selection_audit contact=%.*s candidates=%zu best=%zu quality=%d len=%zu",
                (int)(contact_key_len < 32 ? contact_key_len : 32), contact_key ? contact_key : "",
                candidate_count, best_idx, best_quality, response_len);
    if (response && response_text_len > 0) {
        if (hu_guard_has_numbered_analysis_dump(response, response_text_len))
            hu_log_warn("response_guard", obs,
                        "selection_audit: shipped response has G1 numbered-analysis pattern "
                        "(contact=%.*s best=%zu preview=%.*s)",
                        (int)(contact_key_len < 32 ? contact_key_len : 32),
                        contact_key ? contact_key : "", best_idx, preview_n, response);
        if (hu_guard_has_self_talk_pattern(response, response_text_len))
            hu_log_warn("response_guard", obs,
                        "selection_audit: shipped response has G2 self-talk pattern "
                        "(contact=%.*s best=%zu)",
                        (int)(contact_key_len < 32 ? contact_key_len : 32),
                        contact_key ? contact_key : "", best_idx);
    }
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
    while (*out_len > 0 && (out[*out_len - 1] == ' ' || out[*out_len - 1] == '\t' ||
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
    /* Sprint 31 — thin wrapper. ctx=NULL disables context-aware
     * detections so legacy callers see byte-identical behavior. */
    return hu_response_guard_check_ex(alloc, response, response_len, NULL, out_response, out_len,
                                      out_outcome, report);
}

hu_error_t hu_response_guard_check_ex(hu_allocator_t *alloc, const char *response,
                                      size_t response_len, const hu_guard_context_t *ctx,
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
    while (skip_ws < response_len && (response[skip_ws] == ' ' || response[skip_ws] == '\t' ||
                                      response[skip_ws] == '\n' || response[skip_ws] == '\r'))
        skip_ws++;

    bool stripped_reasoning_prefix = false;

    if (skip_ws < response_len && response[skip_ws] == '*' && skip_ws + 1 < response_len &&
        response[skip_ws + 1] == ' ') {
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
        report->bytes_stripped = prefix_bytes + (needs_strip ? (effective_len - cleaned_len) : 0);
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
        hu_guard_record_reject_stats(report);
        return HU_OK;
    }

    /* Phase 4 (Sprint 31 + 35 + 36) — context-aware detections. Skipped
     * when `ctx` is NULL (legacy `hu_response_guard_check` callers).
     *
     * Phase 4a: length anomaly (response_len >> recent_avg_len).
     * Phase 4b: director-string echo (verbatim quote of upstream
     *           scene-direction text).
     * Phase 4c: persona-PII echo (loaded persona name in third-person
     *           profile construct — Sprint 35).
     * Phase 4d: persona identity echo (verbatim 25+ byte substring of
     *           loaded persona's identity / core_anchor — Sprint 36).
     *
     * All run on the cleaned text (post-Phase 1 strip). The length
     * anomaly check uses the original `response_len` because the
     * cleaning only removes markup, not user-visible content — the
     * "what got sent on the wire" length is what matters relative
     * to the recipient's avg. */
    if (ctx) {
        bool length_anomaly = hu_guard_has_length_anomaly(ctx, response_len);
        bool director_echo = hu_guard_has_director_echo(ctx, for_repetition_check, check_len);
        bool persona_echo = hu_guard_has_persona_pii_echo(ctx, for_repetition_check, check_len);
        bool identity_echo =
            hu_guard_has_persona_identity_echo(ctx, for_repetition_check, check_len);
        if (length_anomaly || director_echo || persona_echo || identity_echo) {
            if (cleaned)
                alloc->free(alloc->ctx, cleaned, effective_len + 1);
            *out_response = NULL;
            *out_len = 0;
            *out_outcome = HU_GUARD_REJECT;
            if (report) {
                report->detected_length_anomaly = length_anomaly;
                report->detected_director_echo = director_echo;
                report->detected_persona_pii_echo = persona_echo;
                report->detected_persona_identity_echo = identity_echo;
            }
            hu_guard_record_reject_stats(report);
            return HU_OK;
        }
    }

    /* Phase 5 (Sprint 41 — 2026-05-26 Jordan incident) — naked
     * discourse-marker opener (G9). Runs on the cleaned text (post-
     * Phase 1 strip) and by default for every caller — the failure
     * mode is intrinsic to the response, not contextual. Two escape
     * hatches: (1) per-call `ctx->naked_opener_disabled` for channels
     * that legitimately use short interjections (voice, debug); (2)
     * process-wide kill switch via
     * hu_response_guard_set_naked_opener_globally_disabled() for
     * runtime operator override without recompile.
     *
     * The production trigger was a proactive iMessage sent to a real
     * human contact: "tbh morning. you awake yet?" — pragmatically
     * incoherent (no proposition for "tbh" to mark) but surface-level
     * Seth-styled enough to slip past every prior validator. */
    bool g9_skip_per_call = (ctx && ctx->naked_opener_disabled);
    bool g9_skip_global = hu_response_guard_naked_opener_globally_disabled();
    if (!g9_skip_per_call && !g9_skip_global &&
        hu_response_is_naked_discourse_opener(for_repetition_check, check_len)) {
        if (cleaned)
            alloc->free(alloc->ctx, cleaned, effective_len + 1);
        *out_response = NULL;
        *out_len = 0;
        *out_outcome = HU_GUARD_REJECT;
        if (report)
            report->detected_naked_discourse_opener = true;
        hu_guard_record_reject_stats(report);
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
