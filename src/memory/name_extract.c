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

/* Capitalized words that are not names. "Mom" and "Dad" are deliberately
 * absent: they are how people name their parents. */
static bool ne_is_stopword(const char *w, size_t len) {
    static const char *const k_stop[] = {
        "I",         "I'm",      "I'll",     "I've",     "I'd",    "Monday",  "Tuesday",
        "Wednesday", "Thursday", "Friday",   "Saturday", "Sunday", "January", "February",
        "March",     "April",    "May",      "June",     "July",   "August",  "September",
        "October",   "November", "December", "Hey",      "Hi",     "Hello",   "Ok",
        "Okay",      "Lol",      "Lmao",     "Yeah",     "Yes",    "Yep",     "No",
        "Nope",      "Thanks",   "Thank",    "God",      "Omg",    "Oh",      "Haha",
        "Sorry",     "Please"};
    for (size_t i = 0; i < sizeof(k_stop) / sizeof(k_stop[0]); i++) {
        if (strlen(k_stop[i]) == len && memcmp(k_stop[i], w, len) == 0)
            return true;
    }
    return false;
}

/* Case-insensitive occurrence of needle bounded by non-alnum bytes (or the
 * ends) on both sides: "Al" is not in "Also" (substring-classifier-pitfalls). */
static bool ne_has_word(const char *hay, size_t hay_len, const char *needle, size_t nl) {
    if (nl == 0 || nl > hay_len)
        return false;
    for (size_t at = 0; at + nl <= hay_len; at++) {
        bool open_edge = at == 0 || !isalnum((unsigned char)hay[at - 1]);
        bool close_edge = at + nl == hay_len || !isalnum((unsigned char)hay[at + nl]);
        if (open_edge && close_edge && strncasecmp(hay + at, needle, nl) == 0)
            return true;
    }
    return false;
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

typedef struct ne_run {
    size_t start;
    size_t end;
    size_t tokens;
    bool at_sentence_start;
} ne_run_t;

/* Emit the open Capitalized run if it qualifies, then close it. */
static size_t ne_flush(const char *text, ne_run_t *run, hu_name_candidate_t *out, size_t n,
                       size_t cap) {
    size_t run_len = run->end - run->start;
    if (run->tokens >= 1 && run->tokens <= HU_NAME_MAX_TOKENS && !run->at_sentence_start &&
        run_len >= HU_NAME_MIN_LEN && run_len <= HU_NAME_MAX_LEN)
        n = ne_add(out, n, cap, text + run->start, run_len, HU_NAME_CAPITALIZED);
    run->tokens = 0;
    return n;
}

size_t hu_name_extract(const char *text, size_t len, const hu_name_ref_t *known, size_t known_count,
                       hu_name_candidate_t *out, size_t out_cap) {
    if (!text || len == 0 || !out || out_cap == 0)
        return 0;
    size_t n = 0;
    for (size_t k = 0; known && k < known_count; k++) {
        if (known[k].name && known[k].len > 0 &&
            ne_has_word(text, len, known[k].name, known[k].len))
            n = ne_add(out, n, out_cap, known[k].name, known[k].len, HU_NAME_KNOWN);
    }

    ne_run_t run = {0, 0, 0, false};
    bool sentence_start = true;
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)text[i];
        if (!isalpha(c)) {
            if (c == '.' || c == '!' || c == '?' || c == '\n')
                sentence_start = true;
            else if (isdigit(c) || c >= 0x80)
                sentence_start = false; /* content, not punctuation */
            /* A run continues only across exactly one space. */
            if (run.tokens > 0 && !(c == ' ' && i == run.end))
                n = ne_flush(text, &run, out, n, out_cap);
            i++;
            continue;
        }
        size_t s = i;
        while (i < len && (isalpha((unsigned char)text[i]) || text[i] == '\''))
            i++;
        size_t tl = i - s;
        if (tl > 2 && text[s + tl - 2] == '\'' &&
            (text[s + tl - 1] == 's' || text[s + tl - 1] == 'S'))
            tl -= 2; /* possessive: Priya's -> Priya */
        bool starts_sentence = sentence_start;
        sentence_start = false;
        /* Letters glued to a non-ASCII byte belong to a word this scanner
         * cannot read ("José" would become "Jos"): never a candidate. */
        bool clean = (s == 0 || (unsigned char)text[s - 1] < 0x80) &&
                     (i == len || (unsigned char)text[i] < 0x80);
        bool cap = clean && tl >= 2 && isupper((unsigned char)text[s]) &&
                   islower((unsigned char)text[s + 1]) && !ne_is_stopword(text + s, tl);
        if (!cap) {
            if (run.tokens > 0)
                n = ne_flush(text, &run, out, n, out_cap);
            continue;
        }
        if (run.tokens > 0 && s == run.end + 1 && text[run.end] == ' ') {
            run.end = s + tl;
            run.tokens++;
            continue;
        }
        if (run.tokens > 0)
            n = ne_flush(text, &run, out, n, out_cap);
        run.start = s;
        run.end = s + tl;
        run.tokens = 1;
        run.at_sentence_start = starts_sentence;
    }
    if (run.tokens > 0)
        n = ne_flush(text, &run, out, n, out_cap);
    return n;
}
