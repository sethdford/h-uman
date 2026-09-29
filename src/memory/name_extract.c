/* name_extract.c — zero-model per-turn name catcher (spec 2026-09-29 §4.2).
 * See include/human/memory/name_extract.h. Precision over recall: a missed
 * name is caught by the nightly typed pass; a wrong one would ground a reply. */
#include "human/memory/name_extract.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

bool hu_name_entity_is_nameable(hu_entity_type_t type, const char *name, size_t len) {
    if (!name || len == 0)
        return false;
    switch (type) {
    case HU_ENTITY_PERSON:
    case HU_ENTITY_PLACE:
    case HU_ENTITY_ORGANIZATION:
    case HU_ENTITY_EVENT:
        return true;
    case HU_ENTITY_UNKNOWN:
        return name[0] >= 'A' && name[0] <= 'Z';
    default:
        return false; /* TOPIC, EMOTION, out of range */
    }
}

/* Capitalized words that are not names: pronoun contractions, days, months,
 * greetings, and the filler that iOS autocapitalizes after an emoji or at a
 * clause start. "Mom" and "Dad" are deliberately absent: they are how people
 * name their parents. Kept in strcmp order for the binary search below. */
static const char *const k_ne_stop[] = {
    "Also",    "And",      "Anyway",    "April",    "August",    "Birthday", "But",      "Congrats",
    "Cool",    "Damn",     "December",  "February", "Friday",    "God",      "Going",    "Good",
    "Great",   "Haha",     "Happy",     "Hello",    "Hey",       "Hi",       "I",        "I'd",
    "I'll",    "I'm",      "I've",      "Id",       "Ill",       "Im",       "Ive",      "January",
    "July",    "June",     "Just",      "Let",      "Lets",      "Lmao",     "Lmk",      "Lol",
    "Love",    "March",    "May",       "Maybe",    "Merry",     "Monday",   "Morning",  "Nice",
    "Night",   "No",       "Nope",      "November", "October",   "Oh",       "Ok",       "Okay",
    "Omg",     "Omw",      "Please",    "Saturday", "September", "So",       "Sorry",    "Sunday",
    "Sure",    "Thank",    "Thanks",    "The",      "Thursday",  "Today",    "Tomorrow", "Tonight",
    "Tuesday", "Ty",       "Wednesday", "Welcome",  "Well",      "Wow",      "Yeah",     "Yep",
    "Yes",     "Yesterday"};

static bool ne_is_stopword(const char *w, size_t len) {
    size_t lo = 0, hi = sizeof(k_ne_stop) / sizeof(k_ne_stop[0]);
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const char *k = k_ne_stop[mid];
        size_t kl = strlen(k);
        int c = memcmp(k, w, kl < len ? kl : len);
        if (c == 0)
            c = kl < len ? -1 : (kl > len ? 1 : 0);
        if (c == 0)
            return true;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return false;
}

/* Byte length of a known NON-letter UTF-8 sequence at t[i], else 0:
 * U+2000-U+203F punctuation (E2 80 xx: ’ … ZWJ), U+FE0F, U+FFFC, and any
 * 4-byte F0 sequence (emoji). Everything else >= 0x80 counts as a letter. */
static size_t ne_nonletter_len(const char *t, size_t len, size_t i) {
    const unsigned char *u = (const unsigned char *)t + i;
    if (i + 3 <= len && u[0] == 0xE2 && u[1] == 0x80 && (u[2] & 0xC0) == 0x80)
        return 3;
    if (i + 3 <= len && u[0] == 0xEF &&
        ((u[1] == 0xBF && u[2] == 0xBC) || (u[1] == 0xB8 && u[2] == 0x8F)))
        return 3;
    if (i + 4 <= len && u[0] == 0xF0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80 &&
        (u[3] & 0xC0) == 0x80)
        return 4;
    return 0;
}

/* An emoji, U+FFFC or U+2026 ends a sentence: iOS capitalizes what follows. */
static bool ne_mb_ends_sentence(const char *t, size_t i, size_t n) {
    const unsigned char *u = (const unsigned char *)t + i;
    return n == 4 || (u[0] == 0xEF && u[1] == 0xBF) || (u[0] == 0xE2 && u[2] == 0xA6);
}

/* Is the ASCII word at [s, e) glued to a non-ASCII letter on that side? */
static bool ne_glued_left(const char *t, size_t s) {
    if (s == 0 || (unsigned char)t[s - 1] < 0x80)
        return false;
    return !((s >= 3 && ne_nonletter_len(t, s, s - 3) == 3) ||
             (s >= 4 && ne_nonletter_len(t, s, s - 4) == 4));
}

static bool ne_glued_right(const char *t, size_t len, size_t e) {
    return e < len && (unsigned char)t[e] >= 0x80 && ne_nonletter_len(t, len, e) == 0;
}

/* Word edges for KNOWN matching: ASCII alnum and non-ASCII letters are word
 * bytes ("Ren" is not in "René"); spaces, punctuation and emoji are not. */
static bool ne_open_edge(const char *t, size_t at) {
    if (at == 0)
        return true;
    unsigned char c = (unsigned char)t[at - 1];
    return c < 0x80 ? !isalnum(c) : !ne_glued_left(t, at);
}

static bool ne_close_edge(const char *t, size_t len, size_t end) {
    if (end == len)
        return true;
    unsigned char c = (unsigned char)t[end];
    return c < 0x80 ? !isalnum(c) : !ne_glued_right(t, len, end);
}

/* Occurrence of a known name bounded by word edges on both sides:
 * "Al" is not in "Also" (substring-classifier-pitfalls). Case-insensitive
 * unless the ref is exact_case. */
static bool ne_has_word(const char *hay, size_t hay_len, const hu_name_ref_t *k) {
    if (k->len == 0 || k->len > hay_len)
        return false;
    for (size_t at = 0; at + k->len <= hay_len; at++) {
        if (!ne_open_edge(hay, at) || !ne_close_edge(hay, hay_len, at + k->len))
            continue;
        if (k->exact_case ? memcmp(hay + at, k->name, k->len) == 0
                          : strncasecmp(hay + at, k->name, k->len) == 0)
            return true;
    }
    return false;
}

/* Length of an apostrophe (' or U+2019) at t[i], else 0. */
static size_t ne_apos_at(const char *t, size_t len, size_t i) {
    if (i < len && t[i] == '\'')
        return 1;
    if (i + 3 <= len && (unsigned char)t[i] == 0xE2 && (unsigned char)t[i + 1] == 0x80 &&
        (unsigned char)t[i + 2] == 0x99)
        return 3;
    return 0;
}

/* Length of an apostrophe ending right before t[e], within [s, e). */
static size_t ne_apos_before(const char *t, size_t s, size_t e) {
    if (e > s && t[e - 1] == '\'')
        return 1;
    return (e - s >= 3 && ne_apos_at(t, e, e - 3) == 3) ? 3 : 0;
}

static bool ne_cap_at(const char *t, size_t len, size_t p) {
    return p + 1 < len && isupper((unsigned char)t[p]) && islower((unsigned char)t[p + 1]);
}

/* End of the token starting at t[i]: letters and apostrophes, with a hyphen
 * joining two Capitalized parts ("Mary-Kate"; not "Mary-kate"). */
static size_t ne_scan_token(const char *t, size_t len, size_t i) {
    size_t part = i;
    for (;;) {
        while (i < len) {
            size_t a = ne_apos_at(t, len, i);
            if (a == 0 && !isalpha((unsigned char)t[i]))
                break;
            i += a ? a : 1;
        }
        if (i + 1 < len && t[i] == '-' && ne_cap_at(t, len, part) && ne_cap_at(t, len, i + 1)) {
            part = ++i;
            continue;
        }
        return i;
    }
}

/* Token length without a trailing apostrophe or possessive: "Priya's",
 * "Priya’s", "Chris'" and "'call Priya'" all yield "Priya"/"Chris". */
static size_t ne_trim_len(const char *t, size_t s, size_t e) {
    for (;;) {
        size_t a = 0;
        if (e - s >= 2 && (t[e - 1] == 's' || t[e - 1] == 'S'))
            a = ne_apos_before(t, s, e - 1);
        if (a) {
            e -= a + 1;
            continue;
        }
        a = ne_apos_before(t, s, e);
        if (a == 0)
            return e - s;
        e -= a;
    }
}

/* Append unless full or already present case-insensitively (KNOWN goes first,
 * so a KNOWN match wins over the same Capitalized run). */
static size_t ne_add(hu_name_candidate_t *out, size_t n, size_t cap, const char *name, size_t len,
                     hu_name_kind_t kind) {
    if (n >= cap)
        return n;
    for (size_t i = 0; i < n; i++) {
        if (out[i].len == len && strncasecmp(out[i].name, name, len) == 0)
            return n;
    }
    out[n].name = name;
    out[n].len = len;
    out[n].kind = kind;
    return n + 1;
}

/* An open run of single-space-separated Capitalized tokens. Stopwords are
 * run members: they count toward the token limit ("Happy New Year Everyone"
 * is 4 tokens, never "New Year Everyone") and are trimmed from the edges
 * ("Hey Salim" -> "Salim"); one in the middle voids the run. */
typedef struct ne_run {
    size_t tokens; /* all tokens, stopwords included */
    size_t end;    /* end of the last token */
    size_t tok_start[HU_NAME_MAX_TOKENS];
    size_t tok_end[HU_NAME_MAX_TOKENS];
    bool tok_stop[HU_NAME_MAX_TOKENS];
    bool at_sentence_start; /* the first token opens a sentence */
} ne_run_t;

static void ne_run_push(ne_run_t *run, size_t s, size_t e, bool stop, bool starts_sentence) {
    if (run->tokens == 0)
        run->at_sentence_start = starts_sentence;
    if (run->tokens < HU_NAME_MAX_TOKENS) {
        run->tok_start[run->tokens] = s;
        run->tok_end[run->tokens] = e;
        run->tok_stop[run->tokens] = stop;
    }
    run->tokens++;
    run->end = e;
}

/* Emit the open run if it qualifies, then close it. */
static size_t ne_flush(const char *text, ne_run_t *run, hu_name_candidate_t *out, size_t n,
                       size_t cap) {
    size_t a = 0, b = run->tokens;
    if (b > HU_NAME_MAX_TOKENS)
        b = 0; /* too long to be a name */
    while (a < b && run->tok_stop[a])
        a++;
    while (b > a && run->tok_stop[b - 1])
        b--;
    bool ok = a < b && !(a == 0 && run->at_sentence_start);
    for (size_t k = a; ok && k < b; k++)
        ok = !run->tok_stop[k];
    if (ok) {
        size_t len = run->tok_end[b - 1] - run->tok_start[a];
        if (len >= HU_NAME_MIN_LEN && len <= HU_NAME_MAX_LEN)
            n = ne_add(out, n, cap, text + run->tok_start[a], len, HU_NAME_CAPITALIZED);
    }
    run->tokens = 0;
    return n;
}

size_t hu_name_extract(const char *text, size_t len, const hu_name_ref_t *known, size_t known_count,
                       hu_name_candidate_t *out, size_t out_cap) {
    if (!text || len == 0 || !out || out_cap == 0)
        return 0;
    size_t n = 0;
    for (size_t k = 0; known && k < known_count; k++) {
        if (known[k].name && ne_has_word(text, len, &known[k]))
            n = ne_add(out, n, out_cap, known[k].name, known[k].len, HU_NAME_KNOWN);
    }

    ne_run_t run;
    memset(&run, 0, sizeof(run));
    bool sentence_start = true;
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)text[i];
        if (!isalpha(c)) {
            size_t step = c >= 0x80 ? ne_nonletter_len(text, len, i) : 0;
            if (c == '.' || c == '!' || c == '?' || c == '\n')
                sentence_start = true;
            else if (step > 0)
                sentence_start = sentence_start || ne_mb_ends_sentence(text, i, step);
            else if (isdigit(c) || c >= 0x80)
                sentence_start = false; /* a digit or non-ASCII letter: content */
            /* A run continues only across exactly one space. */
            if (run.tokens > 0 && !(c == ' ' && i == run.end))
                n = ne_flush(text, &run, out, n, out_cap);
            i += step ? step : 1;
            continue;
        }
        size_t s = i;
        i = ne_scan_token(text, len, i);
        size_t tl = ne_trim_len(text, s, i);
        bool starts_sentence = sentence_start;
        sentence_start = false;
        /* Letters glued to a non-ASCII letter belong to a word this scanner
         * cannot read ("José" would become "Jos"): never a candidate. */
        bool clean = !ne_glued_left(text, s) && !ne_glued_right(text, len, i);
        bool cap = clean && tl >= 2 && ne_cap_at(text, len, s);
        bool joins = run.tokens > 0 && s == run.end + 1 && text[run.end] == ' ';
        if (run.tokens > 0 && (!cap || !joins))
            n = ne_flush(text, &run, out, n, out_cap);
        if (cap)
            ne_run_push(&run, s, s + tl, ne_is_stopword(text + s, tl), starts_sentence);
    }
    if (run.tokens > 0)
        n = ne_flush(text, &run, out, n, out_cap);
    return n;
}
