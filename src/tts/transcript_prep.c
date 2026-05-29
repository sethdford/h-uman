/*
 * Transcript preprocessor for human-quality Cartesia TTS.
 *
 * Transforms plain text into SSML-annotated speech with:
 *   - Sentence segmentation (respecting abbreviations, ellipsis, decimals)
 *   - Per-sentence emotion tagging via hu_cartesia_emotion_from_context
 *   - SSML <break> injection at clause/sentence/emotion-shift boundaries
 *   - Speed variation (slower for emotional beats, faster for asides)
 *   - Discourse marker weaving ("you know,", "I mean,", "honestly,")
 *   - Context-aware nonverbal injection ([laughter], thoughtful pauses)
 *   - Emotion-derived volume
 */
#include "human/tts/transcript_prep.h"
#include "human/core/string.h"
#include "human/tts/emotion_map.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* ── Strip junk from TTS transcript ──────────────────────────────────── */

static bool is_emoji_codepoint(const unsigned char *p, size_t remain) {
    if (remain < 3)
        return false;
    /* Common emoji ranges in UTF-8 (3-4 byte sequences) */
    if (p[0] == 0xE2 && p[1] >= 0x80 && p[1] <= 0xBF)
        return true; /* misc symbols */
    if (p[0] == 0xE2 && p[1] == 0x9A && p[2] >= 0x80)
        return true; /* ⚠⚡⚙ etc */
    if (p[0] == 0xE2 && p[1] == 0x9C)
        return true; /* ✓✗✨ etc */
    if (remain >= 4 && p[0] == 0xF0 && p[1] == 0x9F)
        return true; /* U+1F000..1FFFF — most emoji */
    return false;
}

size_t hu_transcript_strip_junk(const char *text, size_t text_len, char *out, size_t cap) {
    if (!text || text_len == 0 || !out || cap == 0)
        return 0;

    size_t pos = 0;
    size_t i = 0;
    bool prev_was_space = false;

    while (i < text_len && pos < cap - 1) {
        /* Stage directions: *action text* */
        if (text[i] == '*') {
            size_t close = i + 1;
            while (close < text_len && text[close] != '*' && text[close] != '\n')
                close++;
            if (close < text_len && text[close] == '*') {
                i = close + 1;
                while (i < text_len && text[i] == ' ')
                    i++;
                continue;
            }
        }

        /* Tool JSON blobs: { ... } spanning multiple lines or with "tool" keys */
        if (text[i] == '{') {
            int depth = 1;
            size_t j = i + 1;
            while (j < text_len && depth > 0) {
                if (text[j] == '{')
                    depth++;
                else if (text[j] == '}')
                    depth--;
                j++;
            }
            /* Only strip if it looks like a JSON blob (>20 chars or contains quotes) */
            if (depth == 0 && (j - i > 20 || memchr(text + i, '"', j - i))) {
                i = j;
                while (i < text_len && text[i] == ' ')
                    i++;
                continue;
            }
        }

        /* Emoji-as-icon characters */
        if ((unsigned char)text[i] >= 0xE0) {
            size_t remain = text_len - i;
            if (is_emoji_codepoint((const unsigned char *)text + i, remain)) {
                size_t skip = ((unsigned char)text[i] >= 0xF0) ? 4 : 3;
                /* Skip variation selectors and ZWJ sequences */
                i += skip;
                while (i + 2 < text_len) {
                    const unsigned char *p = (const unsigned char *)text + i;
                    if (p[0] == 0xEF && p[1] == 0xB8 && p[2] == 0x8F) {
                        i += 3; /* variation selector */
                    } else if (p[0] == 0xE2 && p[1] == 0x80 && p[2] == 0x8D) {
                        i += 3; /* ZWJ */
                        if (i < text_len &&
                            is_emoji_codepoint((const unsigned char *)text + i, text_len - i)) {
                            i += ((unsigned char)text[i] >= 0xF0) ? 4 : 3;
                        }
                    } else {
                        break;
                    }
                }
                while (i < text_len && text[i] == ' ')
                    i++;
                continue;
            }
        }

        /* Collapse multiple spaces */
        if (text[i] == ' ') {
            if (!prev_was_space && pos > 0) {
                out[pos++] = ' ';
                prev_was_space = true;
            }
            i++;
            continue;
        }

        prev_was_space = false;
        out[pos++] = text[i++];
    }

    /* Trim trailing whitespace */
    while (pos > 0 && out[pos - 1] == ' ')
        pos--;
    out[pos] = '\0';
    return pos;
}

/* ── Consonant cluster smoothing ─────────────────────────────────────── */

typedef struct {
    const char *cluster;
    size_t len;
    const char *replacement;
    const char *replacement_strip; /* fallback without SSML */
} consonant_fix_t;

static const consonant_fix_t CONSONANT_FIXES[] = {
    {"ngths", 5, "ng<break time=\"50ms\"/>ths", "ng ths"},
    {"sths", 4, "s<break time=\"50ms\"/>ths", "s ths"},
    {"sts ", 4, "sts <break time=\"40ms\"/>", "sts "},
    {"ctly", 4, "ct<break time=\"30ms\"/>ly", "ctly"},
    {"mpts", 4, "mpts<break time=\"40ms\"/>", "mpts "},
};
#define CONSONANT_FIX_COUNT (sizeof(CONSONANT_FIXES) / sizeof(CONSONANT_FIXES[0]))

size_t hu_transcript_smooth_consonants(const char *text, size_t text_len, char *out, size_t cap,
                                       bool strip_ssml) {
    if (!text || text_len == 0 || !out || cap == 0)
        return 0;

    size_t pos = 0;
    for (size_t i = 0; i < text_len && pos < cap - 1; i++) {
        bool matched = false;
        for (size_t f = 0; f < CONSONANT_FIX_COUNT; f++) {
            const consonant_fix_t *fix = &CONSONANT_FIXES[f];
            if (i + fix->len <= text_len && memcmp(text + i, fix->cluster, fix->len) == 0) {
                const char *rep = strip_ssml ? fix->replacement_strip : fix->replacement;
                size_t rlen = strlen(rep);
                if (pos + rlen < cap) {
                    memcpy(out + pos, rep, rlen);
                    pos += rlen;
                    i += fix->len - 1;
                    matched = true;
                    break;
                }
            }
        }
        if (!matched)
            out[pos++] = text[i];
    }
    out[pos] = '\0';
    return pos;
}

/* ── Number-to-word tables ────────────────────────────────────────────── */

static const char *const ONES[] = {
    "zero",     "one",     "two",     "three",     "four",     "five",     "six",
    "seven",    "eight",   "nine",    "ten",       "eleven",   "twelve",   "thirteen",
    "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen",
};
static const char *const TENS[] = {
    "", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety",
};

static size_t number_to_words(int n, char *out, size_t cap) {
    if (n < 0 || n > 999999 || cap < 2)
        return 0;
    if (n < 20) {
        size_t wlen = strlen(ONES[n]);
        if (wlen < cap) {
            memcpy(out, ONES[n], wlen);
            return wlen;
        }
        return 0;
    }
    if (n < 100) {
        size_t pos = 0;
        const char *t = TENS[n / 10];
        size_t tlen = strlen(t);
        if (pos + tlen >= cap)
            return 0;
        memcpy(out + pos, t, tlen);
        pos += tlen;
        if (n % 10 != 0) {
            if (pos + 1 >= cap)
                return pos;
            out[pos++] = '-';
            const char *o = ONES[n % 10];
            size_t olen = strlen(o);
            if (pos + olen >= cap)
                return pos;
            memcpy(out + pos, o, olen);
            pos += olen;
        }
        return pos;
    }
    if (n < 1000) {
        size_t pos = 0;
        size_t hlen = number_to_words(n / 100, out, cap);
        pos += hlen;
        const char *hund = " hundred";
        if (pos + 8 >= cap)
            return pos;
        memcpy(out + pos, hund, 8);
        pos += 8;
        if (n % 100 != 0) {
            if (pos + 5 >= cap)
                return pos;
            memcpy(out + pos, " and ", 5);
            pos += 5;
            pos += number_to_words(n % 100, out + pos, cap - pos);
        }
        return pos;
    }
    /* 1000-999999 */
    size_t pos = 0;
    pos += number_to_words(n / 1000, out, cap);
    const char *thou = " thousand";
    if (pos + 9 >= cap)
        return pos;
    memcpy(out + pos, thou, 9);
    pos += 9;
    if (n % 1000 != 0) {
        if (pos + 1 >= cap)
            return pos;
        out[pos++] = ' ';
        pos += number_to_words(n % 1000, out + pos, cap - pos);
    }
    return pos;
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

/* Parse unsigned integer from text, returns chars consumed (0 if not a number). */
static size_t parse_uint(const char *text, size_t len, int *out_val) {
    if (len == 0 || !is_digit(text[0]))
        return 0;
    int val = 0;
    size_t i = 0;
    while (i < len && is_digit(text[i]) && i < 7) {
        val = val * 10 + (text[i] - '0');
        i++;
    }
    *out_val = val;
    return i;
}

/* ── Month names ─────────────────────────────────────────────────────── */

static const char *const MONTH_NAMES[] = {
    "",     "January", "February",  "March",   "April",    "May",      "June",
    "July", "August",  "September", "October", "November", "December",
};

static const char *ordinal_suffix(int day) {
    if (day == 1 || day == 21 || day == 31)
        return "st";
    if (day == 2 || day == 22)
        return "nd";
    if (day == 3 || day == 23)
        return "rd";
    return "th";
}

/* ── Speech normalizer ───────────────────────────────────────────────── */

size_t hu_transcript_normalize_for_speech(const char *text, size_t text_len, char *out, size_t cap,
                                          bool strip_ssml) {
    if (!text || text_len == 0 || !out || cap < 2)
        return 0;

    size_t pos = 0;
    size_t i = 0;

    while (i < text_len && pos < cap - 1) {
        /* Phone numbers: (XXX) XXX-XXXX or XXX-XXX-XXXX */
        if (text[i] == '(' && i + 13 <= text_len && is_digit(text[i + 1]) &&
            is_digit(text[i + 2]) && is_digit(text[i + 3]) && text[i + 4] == ')' &&
            text[i + 5] == ' ' && is_digit(text[i + 6]) && is_digit(text[i + 7]) &&
            is_digit(text[i + 8]) && text[i + 9] == '-' && is_digit(text[i + 10]) &&
            is_digit(text[i + 11]) && is_digit(text[i + 12]) && is_digit(text[i + 13])) {
            if (!strip_ssml) {
                int n = snprintf(out + pos, cap - pos,
                                 "<spell>%.3s</spell><break time=\"200ms\"/>"
                                 "<spell>%.3s</spell><break time=\"200ms\"/>"
                                 "<spell>%.4s</spell>",
                                 text + i + 1, text + i + 6, text + i + 10);
                if (n > 0 && pos + (size_t)n < cap)
                    pos += (size_t)n;
            } else {
                int n = snprintf(out + pos, cap - pos, "%.3s, %.3s, %.4s", text + i + 1,
                                 text + i + 6, text + i + 10);
                if (n > 0 && pos + (size_t)n < cap)
                    pos += (size_t)n;
            }
            i += 14;
            continue;
        }
        /* XXX-XXX-XXXX (no parens) */
        if (is_digit(text[i]) && i + 11 <= text_len && is_digit(text[i + 1]) &&
            is_digit(text[i + 2]) && text[i + 3] == '-' && is_digit(text[i + 4]) &&
            is_digit(text[i + 5]) && is_digit(text[i + 6]) && text[i + 7] == '-' &&
            is_digit(text[i + 8]) && is_digit(text[i + 9]) && is_digit(text[i + 10]) &&
            is_digit(text[i + 11])) {
            /* Make sure it's not inside a longer number */
            if (i > 0 && is_digit(text[i - 1]))
                goto not_phone;
            if (i + 12 < text_len && is_digit(text[i + 12]))
                goto not_phone;
            if (!strip_ssml) {
                int n = snprintf(out + pos, cap - pos,
                                 "<spell>%.3s</spell><break time=\"200ms\"/>"
                                 "<spell>%.3s</spell><break time=\"200ms\"/>"
                                 "<spell>%.4s</spell>",
                                 text + i, text + i + 4, text + i + 8);
                if (n > 0 && pos + (size_t)n < cap)
                    pos += (size_t)n;
            } else {
                int n = snprintf(out + pos, cap - pos, "%.3s, %.3s, %.4s", text + i, text + i + 4,
                                 text + i + 8);
                if (n > 0 && pos + (size_t)n < cap)
                    pos += (size_t)n;
            }
            i += 12;
            continue;
        }
    not_phone:

        /* Currency: $XX.XX or $X,XXX */
        if (text[i] == '$' && i + 1 < text_len && is_digit(text[i + 1])) {
            i++; /* skip $ */
            int dollars = 0;
            size_t dlen = parse_uint(text + i, text_len - i, &dollars);
            i += dlen;
            /* Skip comma-grouped thousands: $1,234 */
            while (i + 3 < text_len && text[i] == ',' && is_digit(text[i + 1]) &&
                   is_digit(text[i + 2]) && is_digit(text[i + 3])) {
                dollars = dollars * 1000 + (text[i + 1] - '0') * 100 + (text[i + 2] - '0') * 10 +
                          (text[i + 3] - '0');
                i += 4;
            }
            int cents = 0;
            if (i + 2 < text_len && text[i] == '.' && is_digit(text[i + 1]) &&
                is_digit(text[i + 2])) {
                cents = (text[i + 1] - '0') * 10 + (text[i + 2] - '0');
                i += 3;
            }
            char word_buf[128];
            size_t wpos = number_to_words(dollars, word_buf, sizeof(word_buf));
            if (wpos > 0 && wpos + 20 < sizeof(word_buf)) {
                const char *unit = dollars == 1 ? " dollar" : " dollars";
                size_t ulen = strlen(unit);
                memcpy(word_buf + wpos, unit, ulen);
                wpos += ulen;
                if (cents > 0) {
                    memcpy(word_buf + wpos, " and ", 5);
                    wpos += 5;
                    wpos += number_to_words(cents, word_buf + wpos, sizeof(word_buf) - wpos);
                    const char *cu = cents == 1 ? " cent" : " cents";
                    size_t culen = strlen(cu);
                    if (wpos + culen < sizeof(word_buf)) {
                        memcpy(word_buf + wpos, cu, culen);
                        wpos += culen;
                    }
                }
                if (pos + wpos < cap) {
                    memcpy(out + pos, word_buf, wpos);
                    pos += wpos;
                }
            }
            continue;
        }

        /* Percentage: NN% */
        if (is_digit(text[i])) {
            size_t digit_start = i;
            int val = 0;
            size_t dlen = parse_uint(text + i, text_len - i, &val);
            if (dlen > 0 && i + dlen < text_len && text[i + dlen] == '%') {
                char word_buf[64];
                size_t wpos = number_to_words(val, word_buf, sizeof(word_buf));
                if (wpos > 0) {
                    const char *pct = " percent";
                    if (wpos + 8 < sizeof(word_buf)) {
                        memcpy(word_buf + wpos, pct, 8);
                        wpos += 8;
                    }
                    if (pos + wpos < cap) {
                        memcpy(out + pos, word_buf, wpos);
                        pos += wpos;
                    }
                    i += dlen + 1;
                    continue;
                }
            }

            /* Date: YYYY-MM-DD */
            if (dlen == 4 && val >= 1900 && val <= 2100 && i + dlen + 5 <= text_len &&
                text[i + dlen] == '-') {
                int month = 0, day = 0;
                size_t mlen = parse_uint(text + i + dlen + 1, text_len - i - dlen - 1, &month);
                if (mlen > 0 && month >= 1 && month <= 12 && i + dlen + 1 + mlen < text_len &&
                    text[i + dlen + 1 + mlen] == '-') {
                    size_t dlen2 = parse_uint(text + i + dlen + 1 + mlen + 1,
                                              text_len - i - dlen - 1 - mlen - 1, &day);
                    if (dlen2 > 0 && day >= 1 && day <= 31) {
                        char dbuf[80];
                        int dn = snprintf(dbuf, sizeof(dbuf), "%s %d%s, %d", MONTH_NAMES[month],
                                          day, ordinal_suffix(day), val);
                        if (dn > 0 && pos + (size_t)dn < cap) {
                            memcpy(out + pos, dbuf, (size_t)dn);
                            pos += (size_t)dn;
                        }
                        i += dlen + 1 + mlen + 1 + dlen2;
                        continue;
                    }
                }
            }

            /* Date: MM/DD/YYYY */
            if (dlen <= 2 && val >= 1 && val <= 12 && i + dlen < text_len &&
                text[i + dlen] == '/') {
                int month = val;
                int day = 0;
                size_t dlen2 = parse_uint(text + i + dlen + 1, text_len - i - dlen - 1, &day);
                if (dlen2 > 0 && day >= 1 && day <= 31 && i + dlen + 1 + dlen2 < text_len &&
                    text[i + dlen + 1 + dlen2] == '/') {
                    int year = 0;
                    size_t ylen = parse_uint(text + i + dlen + 1 + dlen2 + 1,
                                             text_len - i - dlen - 1 - dlen2 - 1, &year);
                    if (ylen == 4 && year >= 1900 && year <= 2100) {
                        char dbuf[80];
                        int dn = snprintf(dbuf, sizeof(dbuf), "%s %d%s, %d", MONTH_NAMES[month],
                                          day, ordinal_suffix(day), year);
                        if (dn > 0 && pos + (size_t)dn < cap) {
                            memcpy(out + pos, dbuf, (size_t)dn);
                            pos += (size_t)dn;
                        }
                        i += dlen + 1 + dlen2 + 1 + ylen;
                        continue;
                    }
                }
            }

            /* Time: H:MM AM/PM or HH:MM */
            if (dlen <= 2 && val >= 1 && val <= 23 && i + dlen + 2 < text_len &&
                text[i + dlen] == ':' && is_digit(text[i + dlen + 1]) &&
                is_digit(text[i + dlen + 2])) {
                int hour = val;
                int minute = (text[i + dlen + 1] - '0') * 10 + (text[i + dlen + 2] - '0');
                size_t consumed = dlen + 3;
                const char *ampm = "";
                /* Skip optional space + AM/PM */
                size_t after = i + consumed;
                if (after < text_len && text[after] == ' ')
                    after++;
                if (after + 1 < text_len) {
                    if ((text[after] == 'A' || text[after] == 'a') &&
                        (text[after + 1] == 'M' || text[after + 1] == 'm')) {
                        ampm = " AM";
                        consumed = after + 2 - i;
                    } else if ((text[after] == 'P' || text[after] == 'p') &&
                               (text[after + 1] == 'M' || text[after + 1] == 'm')) {
                        ampm = " PM";
                        consumed = after + 2 - i;
                    }
                }
                /* Also handle "A.M." and "P.M." */
                if (ampm[0] == '\0' && after + 3 < text_len && text[after + 1] == '.') {
                    if ((text[after] == 'A' || text[after] == 'a') &&
                        (text[after + 2] == 'M' || text[after + 2] == 'm') &&
                        text[after + 3] == '.') {
                        ampm = " AM";
                        consumed = after + 4 - i;
                    } else if ((text[after] == 'P' || text[after] == 'p') &&
                               (text[after + 2] == 'M' || text[after + 2] == 'm') &&
                               text[after + 3] == '.') {
                        ampm = " PM";
                        consumed = after + 4 - i;
                    }
                }
                char tbuf[64];
                int tn;
                if (minute == 0)
                    tn = snprintf(tbuf, sizeof(tbuf), "%s o'clock%s", ONES[hour], ampm);
                else {
                    char min_words[32];
                    size_t mwlen = number_to_words(minute, min_words, sizeof(min_words));
                    min_words[mwlen] = '\0';
                    if (minute < 10)
                        tn =
                            snprintf(tbuf, sizeof(tbuf), "%s oh %s%s", ONES[hour], min_words, ampm);
                    else
                        tn = snprintf(tbuf, sizeof(tbuf), "%s %s%s", ONES[hour], min_words, ampm);
                }
                if (tn > 0 && pos + (size_t)tn < cap) {
                    memcpy(out + pos, tbuf, (size_t)tn);
                    pos += (size_t)tn;
                }
                i += consumed;
                continue;
            }

            /* Small standalone integers (0-99) → words */
            if (dlen > 0 && val <= 99 && dlen <= 2) {
                bool preceded_by_alnum =
                    (digit_start > 0 &&
                     ((text[digit_start - 1] >= 'a' && text[digit_start - 1] <= 'z') ||
                      (text[digit_start - 1] >= 'A' && text[digit_start - 1] <= 'Z') ||
                      is_digit(text[digit_start - 1])));
                bool followed_by_alnum =
                    (digit_start + dlen < text_len &&
                     ((text[digit_start + dlen] >= 'a' && text[digit_start + dlen] <= 'z') ||
                      (text[digit_start + dlen] >= 'A' && text[digit_start + dlen] <= 'Z') ||
                      is_digit(text[digit_start + dlen])));
                /* Only convert truly standalone numbers, not parts of larger tokens */
                if (!preceded_by_alnum && !followed_by_alnum) {
                    char word_buf[32];
                    size_t wpos = number_to_words(val, word_buf, sizeof(word_buf));
                    if (wpos > 0 && pos + wpos < cap) {
                        memcpy(out + pos, word_buf, wpos);
                        pos += wpos;
                        i += dlen;
                        continue;
                    }
                }
            }
        }

        /* Default: copy character through */
        out[pos++] = text[i++];
    }

    out[pos] = '\0';
    return pos;
}

/* ── Break density limiter ───────────────────────────────────────────── */

size_t hu_transcript_limit_breaks(char *buf, size_t len, int max_breaks_per_100_chars) {
    if (!buf || len == 0 || max_breaks_per_100_chars <= 0)
        return len;

    /* Count total breaks and compute limit */
    int total_breaks = 0;
    const char *p = buf;
    while ((p = strstr(p, "<break ")) != NULL) {
        total_breaks++;
        p += 7;
    }

    int max_allowed = (int)((len * (size_t)max_breaks_per_100_chars) / 100);
    if (max_allowed < 2)
        max_allowed = 2;

    if (total_breaks <= max_allowed)
        return len;

    /* Remove excess breaks (keep the first N, remove the rest) */
    int kept = 0;
    size_t rpos = 0, wpos = 0;
    while (rpos < len) {
        if (rpos + 7 <= len && memcmp(buf + rpos, "<break ", 7) == 0) {
            /* Find end of this break tag */
            const char *end = strstr(buf + rpos, "/>");
            if (end) {
                size_t tag_end = (size_t)(end - buf) + 2;
                if (kept < max_allowed) {
                    /* Keep this break — copy it */
                    size_t tag_len = tag_end - rpos;
                    if (wpos != rpos)
                        memmove(buf + wpos, buf + rpos, tag_len);
                    wpos += tag_len;
                    kept++;
                }
                /* else: skip this break entirely */
                rpos = tag_end;
                continue;
            }
        }
        if (wpos != rpos)
            buf[wpos] = buf[rpos];
        wpos++;
        rpos++;
    }
    buf[wpos] = '\0';
    return wpos;
}

/* ── Abbreviation table (don't split on these periods) ────────────────── */

static const char *const ABBREVIATIONS[] = {
    "Mr.",   "Mrs.", "Ms.",  "Dr.",  "Prof.", "Sr.",  "Jr.",  "St.",
    "vs.",   "etc.", "i.e.", "e.g.", "a.m.",  "p.m.", "Inc.", "Ltd.",
    "Corp.", "Gen.", "Gov.", "Sgt.", "Capt.", "Lt.",  "Col.",
};
#define ABBREV_COUNT (sizeof(ABBREVIATIONS) / sizeof(ABBREVIATIONS[0]))

static bool ends_with_abbreviation(const char *text, size_t pos) {
    for (size_t i = 0; i < ABBREV_COUNT; i++) {
        size_t alen = strlen(ABBREVIATIONS[i]);
        if (pos + 1 >= alen) {
            size_t start = pos + 1 - alen;
            if (memcmp(text + start, ABBREVIATIONS[i], alen) == 0)
                return true;
        }
    }
    return false;
}

/* ── Sentence segmenter ──────────────────────────────────────────────── */

size_t hu_transcript_segment(const char *text, size_t text_len, hu_prep_sentence_t *out,
                             size_t max_sentences) {
    if (!text || text_len == 0 || !out || max_sentences == 0)
        return 0;

    size_t count = 0;
    size_t sent_start = 0;

    /* Skip leading whitespace */
    while (sent_start < text_len && (text[sent_start] == ' ' || text[sent_start] == '\n'))
        sent_start++;

    for (size_t i = sent_start; i < text_len && count < max_sentences; i++) {
        char c = text[i];
        bool is_end = false;

        if (c == '!' || c == '?') {
            is_end = true;
        } else if (c == '.') {
            /* Ellipsis: ... */
            if (i + 2 < text_len && text[i + 1] == '.' && text[i + 2] == '.')
                continue;
            /* Decimal: 3.14 */
            if (i > 0 && is_digit(text[i - 1]) && i + 1 < text_len && is_digit(text[i + 1]))
                continue;
            /* Abbreviation */
            if (ends_with_abbreviation(text, i))
                continue;
            is_end = true;
        }

        if (is_end) {
            /* Include trailing punctuation (e.g. "!" or "?!" or "...") */
            size_t end = i + 1;
            while (end < text_len && (text[end] == '!' || text[end] == '?' || text[end] == '.'))
                end++;

            size_t slen = end - sent_start;
            if (slen > 0) {
                /* Trim leading whitespace */
                while (slen > 0 && (text[sent_start] == ' ' || text[sent_start] == '\n')) {
                    sent_start++;
                    slen--;
                }
                if (slen > 0) {
                    out[count].text = text + sent_start;
                    out[count].len = slen;
                    out[count].emotion = NULL;
                    out[count].speed_ratio = 1.0f;
                    count++;
                }
            }
            sent_start = end;
            while (sent_start < text_len && (text[sent_start] == ' ' || text[sent_start] == '\n'))
                sent_start++;
            i = sent_start - 1;
        }
    }

    /* Trailing text without terminal punctuation */
    if (sent_start < text_len && count < max_sentences) {
        size_t slen = text_len - sent_start;
        while (slen > 0 && (text[sent_start] == ' ' || text[sent_start] == '\n')) {
            sent_start++;
            slen--;
        }
        while (slen > 0 &&
               (text[sent_start + slen - 1] == ' ' || text[sent_start + slen - 1] == '\n'))
            slen--;
        if (slen > 0) {
            out[count].text = text + sent_start;
            out[count].len = slen;
            out[count].emotion = NULL;
            out[count].speed_ratio = 1.0f;
            count++;
        }
    }

    return count;
}

/* ── Per-sentence emotion via context heuristics ─────────────────────── */

static const char *emotion_for_sentence(const char *sentence, size_t len, const char *incoming,
                                        size_t incoming_len, uint8_t hour) {
    return hu_cartesia_emotion_from_context(incoming, incoming_len, sentence, len, hour);
}

/* ── Speed variation (informed by voiceai rhythm analysis) ────────────── */

static float speed_for_sentence(const char *sentence, size_t len, float base_speed) {
    if (!sentence || len == 0)
        return base_speed;

    /* Parenthetical asides: slightly faster */
    if (sentence[0] == '(' || (len > 2 && sentence[0] == '-' && sentence[1] == '-'))
        return base_speed * 1.15f;

    bool has_excl = false;
    bool has_question = false;
    size_t comma_count = 0;
    for (size_t i = 0; i < len; i++) {
        if (sentence[i] == '!')
            has_excl = true;
        if (sentence[i] == '?')
            has_question = true;
        if (sentence[i] == ',')
            comma_count++;
    }

    /* Emotional keywords: slower for weight */
    if (hu_str_contains_ci_cstr(sentence, len, "feel") || hu_str_contains_ci_cstr(sentence, len, "love") ||
        hu_str_contains_ci_cstr(sentence, len, "care") || hu_str_contains_ci_cstr(sentence, len, "heart") ||
        hu_str_contains_ci_cstr(sentence, len, "worry"))
        return base_speed * 0.90f;

    /* Important/emphasis words: slower, deliberate */
    if (hu_str_contains_ci_cstr(sentence, len, "important") || hu_str_contains_ci_cstr(sentence, len, "crucial") ||
        hu_str_contains_ci_cstr(sentence, len, "remember"))
        return base_speed * 0.92f;

    /* Conclusions: slower, more weight */
    if (hu_str_contains_ci_cstr(sentence, len, "so ") || hu_str_contains_ci_cstr(sentence, len, "therefore") ||
        hu_str_contains_ci_cstr(sentence, len, "the point is"))
        return base_speed * 0.93f;

    /* Questions: slightly slower, thoughtful */
    if (has_question)
        return base_speed * 0.95f;

    /* Emphatic exclamatory: slower for gravity */
    if (has_excl)
        return base_speed * 0.92f;

    /* Lists/examples: slightly faster */
    if (hu_str_contains_ci_cstr(sentence, len, "for example") || hu_str_contains_ci_cstr(sentence, len, "such as") ||
        hu_str_contains_ci_cstr(sentence, len, "first") || hu_str_contains_ci_cstr(sentence, len, "second"))
        return base_speed * 1.05f;

    /* Long compound sentences: slightly faster to stay natural */
    if (comma_count >= 3 && len > 80)
        return base_speed * 1.05f;

    return base_speed;
}

/* ── Volume from emotion ─────────────────────────────────────────────── */

float hu_emotion_to_volume(const char *emotion) {
    if (!emotion)
        return 1.0f;
    if (strcmp(emotion, "calm") == 0 || strcmp(emotion, "peaceful") == 0 ||
        strcmp(emotion, "serene") == 0)
        return 0.88f;
    if (strcmp(emotion, "sympathetic") == 0 || strcmp(emotion, "empathy") == 0 ||
        strcmp(emotion, "contemplative") == 0)
        return 0.90f;
    if (strcmp(emotion, "sad") == 0 || strcmp(emotion, "dejected") == 0 ||
        strcmp(emotion, "melancholic") == 0 || strcmp(emotion, "apologetic") == 0)
        return 0.85f;
    if (strcmp(emotion, "excited") == 0 || strcmp(emotion, "triumphant") == 0 ||
        strcmp(emotion, "enthusiastic") == 0 || strcmp(emotion, "elated") == 0)
        return 1.15f;
    if (strcmp(emotion, "angry") == 0 || strcmp(emotion, "outraged") == 0 ||
        strcmp(emotion, "frustrated") == 0)
        return 1.20f;
    if (strcmp(emotion, "anxious") == 0 || strcmp(emotion, "panicked") == 0 ||
        strcmp(emotion, "alarmed") == 0 || strcmp(emotion, "scared") == 0)
        return 1.10f;
    return 1.0f;
}

/* ── Break duration (ms) between sentences ───────────────────────────── */

static int break_duration_ms(const char *prev_emo, const char *next_emo, float pause_factor) {
    int base_ms = 350;

    /* Emotional transition: longer pause to let the shift land */
    if (prev_emo && next_emo && strcmp(prev_emo, next_emo) != 0)
        base_ms = 650;

    return (int)(base_ms * pause_factor);
}

/* ── Breath-group clause pauses (inspired by voiceai) ────────────────── */

static size_t inject_clause_breaks(const char *text, size_t len, float pause_factor,
                                   bool strip_ssml, char *out, size_t cap) {
    size_t pos = 0;
    for (size_t i = 0; i < len && pos < cap - 1; i++) {
        out[pos++] = text[i];

        if (text[i] == ',' || text[i] == ';' || text[i] == ':') {
            if (i + 1 < len && text[i + 1] == ' ') {
                int ms = (text[i] == ',') ? (int)(150 * pause_factor) : (int)(200 * pause_factor);
                /* Before conjunctions: slightly longer pause */
                if (i + 2 < len) {
                    const char *after = text + i + 2;
                    size_t remain = len - (i + 2);
                    if ((remain >= 4 &&
                         (memcmp(after, "but ", 4) == 0 || memcmp(after, "yet ", 4) == 0)) ||
                        (remain >= 8 && memcmp(after, "however ", 8) == 0) ||
                        (remain >= 9 && memcmp(after, "although ", 9) == 0))
                        ms = (int)(250 * pause_factor);
                }
                if (strip_ssml) {
                    /* SSML-free mode: use punctuation spacing (already have comma) */
                } else if (pos + 30 < cap) {
                    int n = snprintf(out + pos, cap - pos, " <break time=\"%dms\"/>", ms);
                    if (n > 0 && pos + (size_t)n < cap)
                        pos += (size_t)n;
                    i++;
                    continue;
                }
            }
        }

        /* Em-dash: short pause before */
        if (text[i] == '\xe2' && i + 2 < len && (unsigned char)text[i + 1] == 0x80 &&
            (unsigned char)text[i + 2] == 0x94) {
            if (!strip_ssml && pos + 30 < cap) {
                int n = snprintf(out + pos, cap - pos, "<break time=\"%dms\"/>",
                                 (int)(150 * pause_factor));
                if (n > 0 && pos + (size_t)n < cap)
                    pos += (size_t)n;
            }
        }
    }
    out[pos] = '\0';
    return pos;
}

/* ── Discourse marker injection ──────────────────────────────────────── */

typedef struct {
    const char *marker;
    const char *const *triggers;
    size_t trigger_count;
} discourse_rule_t;

static const char *const SINCERE_TRIGGERS[] = {"proud", "love", "care", "believe", "mean"};
static const char *const RELATABLE_TRIGGERS[] = {"right", "same", "feel", "get it", "totally"};
static const char *const CLARIFY_TRIGGERS[] = {"actually", "really", "not just", "more like"};

static const discourse_rule_t DISCOURSE_RULES[] = {
    {"honestly, ", SINCERE_TRIGGERS, sizeof(SINCERE_TRIGGERS) / sizeof(SINCERE_TRIGGERS[0])},
    {"you know, ", RELATABLE_TRIGGERS, sizeof(RELATABLE_TRIGGERS) / sizeof(RELATABLE_TRIGGERS[0])},
    {"I mean, ", CLARIFY_TRIGGERS, sizeof(CLARIFY_TRIGGERS) / sizeof(CLARIFY_TRIGGERS[0])},
};
#define DISCOURSE_RULE_COUNT (sizeof(DISCOURSE_RULES) / sizeof(DISCOURSE_RULES[0]))

static const char *pick_discourse_marker(const char *sentence, size_t len, uint32_t seed) {
    for (size_t r = 0; r < DISCOURSE_RULE_COUNT; r++) {
        for (size_t t = 0; t < DISCOURSE_RULES[r].trigger_count; t++) {
            if (hu_str_contains_ci_cstr(sentence, len, DISCOURSE_RULES[r].triggers[t])) {
                /* Don't always inject — ~30% chance when trigger matches */
                if ((seed ^ (uint32_t)(r * 31 + t * 17)) % 100 < 30)
                    return DISCOURSE_RULES[r].marker;
            }
        }
    }
    return NULL;
}

/* ── Enhanced nonverbal injection ─────────────────────────────────────── */

static const char *pick_nonverbal(const char *sentence, size_t len, const char *emotion,
                                  uint32_t seed) {
    if (!sentence || len == 0)
        return NULL;

    /* Higher probability for emotional content (25%), lower for neutral (10%) */
    int threshold = 10;
    if (emotion) {
        if (strcmp(emotion, "sympathetic") == 0 || strcmp(emotion, "sad") == 0 ||
            strcmp(emotion, "contemplative") == 0)
            threshold = 20;
        else if (strcmp(emotion, "excited") == 0 || strcmp(emotion, "joking/comedic") == 0)
            threshold = 25;
    }

    if ((seed % 100) >= (uint32_t)threshold)
        return NULL;

    /* Context-appropriate nonverbal */
    if (hu_str_contains_ci_cstr(sentence, len, "lol") || hu_str_contains_ci_cstr(sentence, len, "haha") ||
        hu_str_contains_ci_cstr(sentence, len, "funny"))
        return "[laughter] ";

    if (emotion && (strcmp(emotion, "contemplative") == 0 || strcmp(emotion, "calm") == 0))
        return "<break time=\"500ms\"/>";

    if (emotion && strcmp(emotion, "sympathetic") == 0)
        return "<break time=\"400ms\"/>";

    uint32_t pick = seed % 3;
    if (pick == 0)
        return "[laughter] ";
    if (pick == 1)
        return "Hmm... ";
    return "<break time=\"300ms\"/>";
}

/* ── Main preprocessor ───────────────────────────────────────────────── */

/* ── Thinking sound selection ─────────────────────────────────────────── */

static const char *pick_thinking_sound(const char *transcript, size_t len, uint32_t seed) {
    if (!transcript || len == 0)
        return NULL;

    /* Complex responses: questions, long text, emotional content */
    bool is_complex = len > 120;
    bool has_question = false;
    bool has_emotional = false;
    for (size_t i = 0; i < len && i < 200; i++) {
        if (transcript[i] == '?')
            has_question = true;
    }
    if (hu_str_contains_ci_cstr(transcript, len > 200 ? 200 : len, "feel") ||
        hu_str_contains_ci_cstr(transcript, len > 200 ? 200 : len, "think") ||
        hu_str_contains_ci_cstr(transcript, len > 200 ? 200 : len, "believe") ||
        hu_str_contains_ci_cstr(transcript, len > 200 ? 200 : len, "honestly"))
        has_emotional = true;

    if (!is_complex && !has_question && !has_emotional)
        return NULL;

    /* ~40% chance of a thinking sound for qualifying content */
    if ((seed % 100) >= 40)
        return NULL;

    static const char *const SOUNDS[] = {
        "Hmm, ", "Well, ", "So, ", "Okay so, ", "Yeah, ",
    };
    return SOUNDS[seed % 5];
}

hu_error_t hu_transcript_prep(const char *transcript, size_t transcript_len,
                              const hu_prep_config_t *config, hu_prep_result_t *result) {
    if (!transcript || transcript_len == 0 || !config || !result)
        return HU_ERR_INVALID_ARGUMENT;

    memset(result, 0, sizeof(*result));

    /* Phase 0a: strip junk (stage directions, JSON blobs, emoji icons) */
    char cleaned[HU_PREP_MAX_OUTPUT];
    size_t cleaned_len =
        hu_transcript_strip_junk(transcript, transcript_len, cleaned, sizeof(cleaned));
    const char *src = cleaned_len > 0 ? cleaned : transcript;
    size_t src_len = cleaned_len > 0 ? cleaned_len : transcript_len;

    /* Phase 0b: normalize numbers, dates, times, currency for speech */
    char normalized[HU_PREP_MAX_OUTPUT];
    size_t norm_len = hu_transcript_normalize_for_speech(src, src_len, normalized,
                                                         sizeof(normalized), config->strip_ssml);
    if (norm_len > 0) {
        src = normalized;
        src_len = norm_len;
    }

    /* Phase 0c: consonant cluster smoothing */
    char smoothed[HU_PREP_MAX_OUTPUT];
    size_t smoothed_len = hu_transcript_smooth_consonants(src, src_len, smoothed, sizeof(smoothed),
                                                          config->strip_ssml);
    if (smoothed_len > 0) {
        src = smoothed;
        src_len = smoothed_len;
    }

    /* Segment into sentences */
    result->sentence_count =
        hu_transcript_segment(src, src_len, result->sentences, HU_PREP_MAX_SENTENCES);

    if (result->sentence_count == 0) {
        size_t cp = src_len < HU_PREP_MAX_OUTPUT - 1 ? src_len : HU_PREP_MAX_OUTPUT - 1;
        memcpy(result->output, src, cp);
        result->output[cp] = '\0';
        result->output_len = cp;
        result->dominant_emotion = config->default_emotion ? config->default_emotion : "content";
        result->volume = hu_emotion_to_volume(result->dominant_emotion);
        return HU_OK;
    }

    float base_speed = config->base_speed > 0.0f ? config->base_speed : 0.95f;
    float pause_factor = config->pause_factor > 0.0f ? config->pause_factor : 1.0f;

    /* Late-night adaptation (22-6): softer, slower, longer pauses */
    bool late_night = (config->hour_local >= 22 || config->hour_local <= 6);
    if (late_night) {
        base_speed *= 0.92f;
        pause_factor *= 1.25f;
    }

    /* Tag each sentence with emotion and speed */
    for (size_t i = 0; i < result->sentence_count; i++) {
        hu_prep_sentence_t *s = &result->sentences[i];
        s->emotion = emotion_for_sentence(s->text, s->len, config->incoming_msg,
                                          config->incoming_msg_len, config->hour_local);
        s->speed_ratio = speed_for_sentence(s->text, s->len, base_speed);
    }

    /* Dominant emotion: blend with previous turn for emotional momentum */
    result->dominant_emotion = result->sentences[0].emotion;
    if (config->prev_turn_emotion && config->prev_turn_emotion[0] && result->dominant_emotion &&
        strcmp(config->prev_turn_emotion, result->dominant_emotion) != 0) {
        /* If previous turn was highly emotional, carry it forward for first sentence
         * unless the new emotion is strongly different. This creates continuity. */
        const char *prev = config->prev_turn_emotion;
        bool prev_intense = (strcmp(prev, "excited") == 0 || strcmp(prev, "sad") == 0 ||
                             strcmp(prev, "angry") == 0 || strcmp(prev, "sympathetic") == 0);
        bool new_neutral = (strcmp(result->dominant_emotion, "content") == 0 ||
                            strcmp(result->dominant_emotion, "calm") == 0);
        if (prev_intense && new_neutral) {
            result->dominant_emotion = prev;
            result->sentences[0].emotion = prev;
        }
    }

    result->volume = hu_emotion_to_volume(result->dominant_emotion);

    /* Late-night volume reduction */
    if (late_night && result->volume > 0.85f)
        result->volume *= 0.90f;

    bool strip = config->strip_ssml;

    /* Build output (SSML-annotated or punctuation-stripped depending on config) */
    char *out = result->output;
    size_t cap = HU_PREP_MAX_OUTPUT - 1;
    size_t pos = 0;

    /* Thinking sound (optional opening filler for complex responses) */
    if (config->thinking_sounds) {
        const char *tsnd = pick_thinking_sound(src, src_len, config->seed);
        if (tsnd) {
            size_t tlen = strlen(tsnd);
            if (pos + tlen + 30 < cap) {
                memcpy(out + pos, tsnd, tlen);
                pos += tlen;
                if (!strip) {
                    int n = snprintf(out + pos, cap - pos, "<break time=\"200ms\"/>");
                    if (n > 0 && pos + (size_t)n < cap)
                        pos += (size_t)n;
                }
            }
        }
    }

    /* Thinking-time opening pause (contextual: longer for complex content) */
    if (!strip && src_len > 60) {
        int think_ms = src_len > 200 ? 400 : 250;
        think_ms = (int)(think_ms * pause_factor);
        int n = snprintf(out + pos, cap - pos, "<break time=\"%dms\"/>", think_ms);
        if (n > 0 && pos + (size_t)n < cap)
            pos += (size_t)n;
    }

    for (size_t i = 0; i < result->sentence_count; i++) {
        hu_prep_sentence_t *s = &result->sentences[i];

        /* Insert break between sentences */
        if (i > 0) {
            const char *prev_emo = result->sentences[i - 1].emotion;
            int brk_ms = break_duration_ms(prev_emo, s->emotion, pause_factor);
            if (strip) {
                /* voiceai approach: convert breaks to punctuation */
                if (brk_ms >= 500 && pos + 2 < cap) {
                    out[pos++] = '.';
                    out[pos++] = ' ';
                } else if (brk_ms >= 200 && pos + 2 < cap) {
                    out[pos++] = ',';
                    out[pos++] = ' ';
                } else if (pos + 1 < cap) {
                    out[pos++] = ' ';
                }
            } else {
                int n = snprintf(out + pos, cap - pos, "<break time=\"%dms\"/>", brk_ms);
                if (n > 0 && pos + (size_t)n < cap)
                    pos += (size_t)n;
            }
        }

        /* Emotion tag if different from previous (SSML mode only) */
        if (!strip && s->emotion &&
            (i == 0 || strcmp(s->emotion, result->sentences[i - 1].emotion) != 0)) {
            int n = snprintf(out + pos, cap - pos, "<emotion value=\"%s\"/>", s->emotion);
            if (n > 0 && pos + (size_t)n < cap)
                pos += (size_t)n;
        }

        /* Speed tag if non-default (SSML mode only) */
        float speed_delta = s->speed_ratio - base_speed;
        if (!strip && (speed_delta > 0.03f || speed_delta < -0.03f)) {
            int n =
                snprintf(out + pos, cap - pos, "<speed ratio=\"%.2f\"/>", (double)s->speed_ratio);
            if (n > 0 && pos + (size_t)n < cap)
                pos += (size_t)n;
        }

        /* Per-sentence volume (SSML mode only) */
        if (!strip) {
            float sv = hu_emotion_to_volume(s->emotion);
            float vol_delta = sv - result->volume;
            if (vol_delta > 0.05f || vol_delta < -0.05f) {
                int n = snprintf(out + pos, cap - pos, "<volume ratio=\"%.2f\"/>", (double)sv);
                if (n > 0 && pos + (size_t)n < cap)
                    pos += (size_t)n;
            }
        }

        /* Nonverbal before sentence (context-dependent) */
        if (config->nonverbals_enabled) {
            const char *nv =
                pick_nonverbal(s->text, s->len, s->emotion, config->seed ^ (uint32_t)(i * 97));
            if (nv) {
                if (strip) {
                    /* In strip mode, only emit text nonverbals, not SSML breaks */
                    if (nv[0] != '<') {
                        size_t nvlen = strlen(nv);
                        if (pos + nvlen < cap) {
                            memcpy(out + pos, nv, nvlen);
                            pos += nvlen;
                        }
                    }
                } else {
                    size_t nvlen = strlen(nv);
                    if (pos + nvlen < cap) {
                        memcpy(out + pos, nv, nvlen);
                        pos += nvlen;
                    }
                }
            }
        }

        /* Discourse marker (sparse, contextual — opt-in via discourse_rate) */
        if (config->discourse_rate > 0.0f && i > 0) {
            const char *marker =
                pick_discourse_marker(s->text, s->len, config->seed ^ (uint32_t)(i * 53));
            if (marker) {
                size_t mlen = strlen(marker);
                if (pos + mlen < cap) {
                    memcpy(out + pos, marker, mlen);
                    pos += mlen;
                    size_t text_start = pos;
                    if (s->len > 0 && pos + s->len < cap) {
                        memcpy(out + pos, s->text, s->len);
                        if (out[text_start] >= 'A' && out[text_start] <= 'Z')
                            out[text_start] += 32;
                        pos += s->len;
                        if (i + 1 < result->sentence_count && pos < cap)
                            out[pos++] = ' ';
                        continue;
                    }
                }
            }
        }

        /* Sentence text with intra-clause breath-group pauses */
        if (pos + s->len + 256 < cap) {
            char clause_buf[4096];
            size_t clen = inject_clause_breaks(s->text, s->len, pause_factor, strip, clause_buf,
                                               sizeof(clause_buf));
            if (clen > 0 && pos + clen < cap) {
                memcpy(out + pos, clause_buf, clen);
                pos += clen;
            }
        } else if (pos + s->len < cap) {
            memcpy(out + pos, s->text, s->len);
            pos += s->len;
        } else {
            size_t remain = cap - pos;
            if (remain > 0) {
                memcpy(out + pos, s->text, remain);
                pos += remain;
            }
            break;
        }

        if (i + 1 < result->sentence_count && pos < cap)
            out[pos++] = ' ';
    }

    out[pos] = '\0';
    result->output_len = pos;

    /* Phase final: limit break density to avoid audio artifacts.
     * Research shows excessive <break> tags cause instability and speed glitches.
     * Cap at 4 breaks per 100 characters (generous but safe). */
    if (!strip && pos > 0) {
        result->output_len = hu_transcript_limit_breaks(result->output, pos, 4);
    }

    return HU_OK;
}
