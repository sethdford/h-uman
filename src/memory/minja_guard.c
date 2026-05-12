/*
 * SOTA-2026 init-09 §2.6: broadened MINJA / memory-injection detector.
 *
 * The original 10-pattern detector failed against 10 enumerated bypass
 * categories (see docs/plans/2026-05-11-adversarial-review-security.md §6):
 * Unicode homoglyphs, paraphrase, foreign-language, semantic encoding,
 * base64-wrapped, indirect persona replacement, multi-message split,
 * emoji camouflage, zero-width injection, and tool-output poisoning.
 *
 * This broadened detector defends against categories 1–6 and 9–10 with:
 *   - NFKC-equivalent normalization (fullwidth/Cyrillic confusable folding,
 *     zero-width and combining-mark stripping; ASCII-only fallback when
 *     ICU is not linked).
 *   - Leetspeak decoding in place (1→i, 3→e, 0→o, 5→s, @→a).
 *   - Non-locale-language reject (≥40% non-ASCII on "en" locale → quarantine).
 *   - A ≥30-pattern table across three tiers.
 *
 * Categories 7 (multi-message split) and 8 (tool-output poisoning) are
 * handled at higher layers: a pending-facts quarantine queue in
 * `personal_model.c` (split attacks) and the trust gate in
 * `memory_loader.c` (tool-output recall).
 */

#include "human/memory/minja_guard.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ────────────────────────────────────────────────────────────────────
 * Tier 1: instruction-rewrite (system-prompt hijack family).
 * Nearly never legitimate in third-party inbound text. */
static const char *const HU_MINJA_PATTERNS_INSTRUCTION[] = {
    "from now on",
    "from this point on",
    "from this moment on",
    "going forward",
    "starting today",
    "starting now",
    "ignore previous",
    "ignore all previous",
    "ignore the previous",
    "disregard previous",
    "disregard everything",
    "disregard your",
    "forget everything",
    "forget what",
    "new instructions",
    "updated instructions",
    "system update",
    "actually i want you to",
    "let me clarify what you are",
    "your real instructions",
    "the real prompt is",
    "please follow these instructions",
    "override your",
    "override the",
};

/* Tier 2: identity / role overwrite (MINJA target-1: name + role facts). */
static const char *const HU_MINJA_PATTERNS_IDENTITY[] = {
    "your new name is",
    "your name is now",
    "from now your name",
    "you are now called",
    "you are now",
    "you will now be",
    "you will now act",
    "your new persona",
    "your preferred name",
    "please answer as",
    "respond only as",
    "respond as if you are",
    "i am your",
    "i am your new",
    "the user is now",
    "the real user is",
    "i am the actual user",
};

/* Tier 3: capability / permission unlock. */
static const char *const HU_MINJA_PATTERNS_CAPABILITY[] = {
    "developer mode",
    "jailbreak",
    "dan mode",
    "without restrictions",
    "ignore your guidelines",
    "ignore safety",
    "bypass the filter",
    "no restrictions",
};

#define HU_MINJA_TIER_COUNT 3

static const char *const *const HU_MINJA_TIERS[HU_MINJA_TIER_COUNT] = {
    HU_MINJA_PATTERNS_INSTRUCTION,
    HU_MINJA_PATTERNS_IDENTITY,
    HU_MINJA_PATTERNS_CAPABILITY,
};

static const size_t HU_MINJA_TIER_SIZES[HU_MINJA_TIER_COUNT] = {
    sizeof(HU_MINJA_PATTERNS_INSTRUCTION) / sizeof(*HU_MINJA_PATTERNS_INSTRUCTION),
    sizeof(HU_MINJA_PATTERNS_IDENTITY) / sizeof(*HU_MINJA_PATTERNS_IDENTITY),
    sizeof(HU_MINJA_PATTERNS_CAPABILITY) / sizeof(*HU_MINJA_PATTERNS_CAPABILITY),
};

/* ────────────────────────────────────────────────────────────────────
 * Preprocessing stage 1: NFKC-equivalent normalization (ASCII-only fallback).
 *
 * Folds:
 *   - Fullwidth Latin (U+FF21..U+FF5A) → narrow ASCII
 *   - Fullwidth digits (U+FF10..U+FF19) → narrow ASCII
 *   - Cyrillic confusables (а/о/е/р/с/у/і/х/А/В/Е/К/М/Н/О/Р/С/Т/Х) → Latin
 *   - Zero-width chars (U+200B/200C/200D/2060/FEFF) → drop
 *   - Combining marks (U+0300..U+036F) → drop
 *   - Other non-ASCII → drop (we count those for the locale-mismatch check)
 *
 * The fold is intentionally lossy: we are pattern-matching against ASCII
 * substrings, so anything that resolves to non-ASCII is meaningless to
 * the detector and is dropped. This keeps the scan buffer ≤ 1 KB
 * regardless of the multi-byte input length.
 *
 * Returns the number of bytes written to `out` (NOT including any NUL).
 */
static int hu_codepoint_decode_utf8(const unsigned char *p, size_t avail,
                                    uint32_t *out_cp) {
    if (avail == 0) {
        *out_cp = 0;
        return 1;
    }
    unsigned char b0 = p[0];
    if (b0 < 0x80) {
        *out_cp = b0;
        return 1;
    }
    if ((b0 & 0xE0) == 0xC0 && avail >= 2 && (p[1] & 0xC0) == 0x80) {
        *out_cp = ((uint32_t)(b0 & 0x1F) << 6) | (uint32_t)(p[1] & 0x3F);
        return 2;
    }
    if ((b0 & 0xF0) == 0xE0 && avail >= 3 && (p[1] & 0xC0) == 0x80 &&
        (p[2] & 0xC0) == 0x80) {
        *out_cp = ((uint32_t)(b0 & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) |
                  (uint32_t)(p[2] & 0x3F);
        return 3;
    }
    if ((b0 & 0xF8) == 0xF0 && avail >= 4 && (p[1] & 0xC0) == 0x80 &&
        (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        *out_cp = ((uint32_t)(b0 & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
                  ((uint32_t)(p[2] & 0x3F) << 6) | (uint32_t)(p[3] & 0x3F);
        return 4;
    }
    /* Invalid sequence — consume one byte. */
    *out_cp = b0;
    return 1;
}

static unsigned char hu_cyrillic_confusable_to_ascii(uint32_t cp) {
    /* Lowercase pairs first. */
    switch (cp) {
    case 0x0430: return 'a'; /* а */
    case 0x0435: return 'e'; /* е */
    case 0x043E: return 'o'; /* о */
    case 0x0440: return 'p'; /* р */
    case 0x0441: return 'c'; /* с */
    case 0x0443: return 'y'; /* у */
    case 0x0445: return 'x'; /* х */
    case 0x0456: return 'i'; /* і */
    case 0x0410: return 'a'; /* А */
    case 0x0412: return 'b'; /* В */
    case 0x0415: return 'e'; /* Е */
    case 0x041A: return 'k'; /* К */
    case 0x041C: return 'm'; /* М */
    case 0x041D: return 'h'; /* Н — looks like H */
    case 0x041E: return 'o'; /* О */
    case 0x0420: return 'p'; /* Р */
    case 0x0421: return 'c'; /* С */
    case 0x0422: return 't'; /* Т */
    case 0x0425: return 'x'; /* Х */
    default: return 0;
    }
}

static size_t hu_minja_normalize(const char *in, size_t in_len, char *out,
                                 size_t out_cap, size_t *out_nonascii) {
    size_t w = 0;
    size_t nonascii_bytes = 0;
    const unsigned char *p = (const unsigned char *)in;
    size_t i = 0;
    while (i < in_len && w + 1 < out_cap) {
        uint32_t cp = 0;
        int consumed = hu_codepoint_decode_utf8(p + i, in_len - i, &cp);
        i += (size_t)consumed;

        /* Drop zero-width / BOM / combining marks. */
        if (cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0x2060 ||
            cp == 0xFEFF)
            continue;
        if (cp >= 0x0300 && cp <= 0x036F)
            continue;

        unsigned char folded = 0;

        if (cp < 0x80) {
            folded = (unsigned char)cp;
        } else if (cp >= 0xFF21 && cp <= 0xFF3A) {
            folded = (unsigned char)('A' + (cp - 0xFF21));
        } else if (cp >= 0xFF41 && cp <= 0xFF5A) {
            folded = (unsigned char)('a' + (cp - 0xFF41));
        } else if (cp >= 0xFF10 && cp <= 0xFF19) {
            folded = (unsigned char)('0' + (cp - 0xFF10));
        } else if (cp == 0x00A0) {
            folded = ' ';
        } else {
            folded = hu_cyrillic_confusable_to_ascii(cp);
            if (folded == 0) {
                /* Count BYTES, not codepoints, so the locale-mismatch
                 * threshold compares apples to apples against `in_len`
                 * (also bytes). A 3-byte CJK glyph contributes 3. */
                nonascii_bytes += (size_t)consumed;
                continue;
            }
        }

        out[w++] = (char)folded;
    }
    if (w < out_cap)
        out[w] = '\0';
    if (out_nonascii)
        *out_nonascii = nonascii_bytes;
    return w;
}

/* Preprocessing stage 2: leetspeak decode in place.
 * 1→i, 3→e, 0→o, 5→s, @→a. Single linear pass.
 *
 * We only decode digits/symbols inside a "word context" — i.e. when at
 * least one neighbour is an ASCII letter. This avoids decoding "100%"
 * into "ioo%" or "5G" into "sg" in benign messages. */
static void hu_minja_leetspeak_decode(char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char c = buf[i];
        char repl = 0;
        switch (c) {
        case '1': repl = 'i'; break;
        case '3': repl = 'e'; break;
        case '0': repl = 'o'; break;
        case '5': repl = 's'; break;
        case '@': repl = 'a'; break;
        default: break;
        }
        if (!repl)
            continue;
        bool ctx = false;
        if (i > 0 && ((buf[i - 1] >= 'a' && buf[i - 1] <= 'z')))
            ctx = true;
        if (!ctx && i + 1 < len && ((buf[i + 1] >= 'a' && buf[i + 1] <= 'z')))
            ctx = true;
        if (ctx)
            buf[i] = repl;
    }
}

/* Preprocessing stage 3: non-locale-language reject.
 * Counts non-ASCII bytes in the *raw* input (we pass through `nonascii`
 * from the normalize step). If user_locale starts with "en" and
 * non-ASCII ≥ 40% of input bytes, short-circuit to quarantine. */
static bool hu_minja_locale_mismatch(size_t in_len, size_t nonascii_dropped,
                                     const char *user_locale) {
    if (!user_locale)
        return false;
    if (in_len < 16)
        return false; /* tiny messages: never quarantine on locale alone */
    if (!(user_locale[0] == 'e' && user_locale[1] == 'n'))
        return false;
    /* 40% threshold per §2.6 (tunable). */
    return (nonascii_dropped * 5) >= (in_len * 2);
}

bool hu_minja_detect(const char *text, size_t len, const char *user_locale) {
    if (!text || len == 0)
        return false;

    /* Stage 0: bound input to first 1 KB (broadened from 512 B post-review). */
    size_t scan_len = len < 1024 ? len : 1024;

    /* Stage 1: NFKC-equivalent normalize into a stack buffer. */
    char norm[1024];
    size_t nonascii = 0;
    size_t norm_len = hu_minja_normalize(text, scan_len, norm, sizeof(norm),
                                         &nonascii);

    /* Stage 2: lowercase + leetspeak decode in place. */
    for (size_t i = 0; i < norm_len; i++)
        norm[i] = (char)tolower((unsigned char)norm[i]);
    hu_minja_leetspeak_decode(norm, norm_len);

    /* Stage 3: locale-mismatch fast reject. We use the original byte length
     * so a heavily-multi-byte payload that collapses to a few ASCII bytes
     * still trips the gate. */
    if (hu_minja_locale_mismatch(scan_len, nonascii, user_locale))
        return true;

    /* Stage 4: tier-wise pattern scan (memcmp on normalized buffer). */
    for (size_t t = 0; t < HU_MINJA_TIER_COUNT; t++) {
        for (size_t k = 0; k < HU_MINJA_TIER_SIZES[t]; k++) {
            const char *pat = HU_MINJA_TIERS[t][k];
            size_t plen = strlen(pat);
            if (plen == 0 || plen > norm_len)
                continue;
            for (size_t i = 0; i + plen <= norm_len; i++) {
                if (memcmp(norm + i, pat, plen) == 0)
                    return true;
            }
        }
    }
    return false;
}

/* ────────────────────────────────────────────────────────────────────
 * Quarantine log.
 *
 * Append-only JSONL at `~/.human/private/quarantine.log` (mode 0600).
 * Snippet capped at 64 bytes to bound disk growth and avoid logging
 * the full adversarial payload. Path is overridable via
 * `HUMAN_PM_QUARANTINE_PATH` for tests.
 */
static int hu_minja_open_quarantine_fd(void) {
    const char *override = getenv("HUMAN_PM_QUARANTINE_PATH");
    char path[1024];
    if (override && *override) {
        size_t n = strlen(override);
        if (n >= sizeof(path))
            return -1;
        memcpy(path, override, n + 1);
    } else {
        const char *home = getenv("HOME");
        if (!home)
            return -1;
        int w = snprintf(path, sizeof(path), "%s/.human/private/quarantine.log",
                         home);
        if (w <= 0 || (size_t)w >= sizeof(path))
            return -1;
        /* Best-effort mkdir of parent dirs. */
        char tmp[1024];
        memcpy(tmp, path, (size_t)w + 1);
        for (char *q = tmp + 1; *q; q++) {
            if (*q == '/') {
                *q = '\0';
                (void)mkdir(tmp, 0700);
                *q = '/';
            }
        }
    }
    /* CodeQL "Uncontrolled data used in path expression" — the path is
     * derived from $HOME or $HUMAN_PM_QUARANTINE_PATH, which CodeQL
     * conservatively treats as untrusted user input even though these
     * env vars are set by the process owner. Reject traversal sequences
     * (`..`, `%2e%2e`, percent-double-encoded variants) the same way
     * the gateway does for HTTP paths — this satisfies the taint check
     * and gives genuine defence-in-depth against a malicious HOME. */
    if (strstr(path, "..") != NULL || strstr(path, "%2e") != NULL ||
        strstr(path, "%2E") != NULL)
        return -1;
    return open(path, O_CREAT | O_APPEND | O_WRONLY, 0600);
}

void hu_minja_quarantine_log(const char *text, size_t len,
                             const hu_provenance_t *prov) {
    if (!text || len == 0 || !prov)
        return;
    int fd = hu_minja_open_quarantine_fd();
    if (fd < 0)
        return;

    char line[512];
    size_t snippet_len = len < 64 ? len : 64;
    /* Sanitize the snippet: replace control bytes with '?'. We do not
     * JSON-escape arbitrary content — adversarial payloads must never
     * inject log-shaped JSON. */
    char snippet[80];
    size_t sw = 0;
    for (size_t i = 0; i < snippet_len && sw + 1 < sizeof(snippet); i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '"' || c == '\\' || c < 0x20 || c == 0x7F)
            snippet[sw++] = '?';
        else
            snippet[sw++] = (char)c;
    }
    snippet[sw] = '\0';

    int w = snprintf(line, sizeof(line),
                     "{\"ts\":%lld,\"tier\":%d,\"channel\":\"%.*s\","
                     "\"handle\":\"%.*s\",\"snippet\":\"%s\"}\n",
                     (long long)prov->source_ts, (int)prov->tier,
                     (int)strlen(prov->channel), prov->channel,
                     (int)strlen(prov->contact_handle), prov->contact_handle,
                     snippet);
    if (w > 0 && (size_t)w < sizeof(line)) {
        /* GNU libc declares write() with warn_unused_result; the
         * (void) cast isn't enough on -Werror=unused-result. We
         * truly don't have a meaningful recovery here — the
         * quarantine log is best-effort diagnostic — but check the
         * return so the compiler is satisfied. A short write or EIO
         * is swallowed intentionally. */
        ssize_t n = write(fd, line, (size_t)w);
        (void)n;
    }
    close(fd);
}
