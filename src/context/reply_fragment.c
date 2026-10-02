/* Reply fragment detection. See include/human/context/reply_fragment.h. */
#include "human/context/reply_fragment.h"
#include "human/core/log.h"
#include "human/observer.h"

#include <ctype.h>
#include <stdatomic.h>
#include <string.h>
#include <strings.h>

/* Words a complete utterance never ends on. Conservative on purpose: words
 * English lets a clause strand ("what's it about", "i'd love to", "where you
 * at", "not yet", "i like it though") are absent, because a false positive
 * here keeps a worse reply or logs a phantom fragment. Contracted auxiliaries
 * are listed because English forbids them clause-finally ("*yeah it's"). */
static const char *const k_dangling[] = {
    "a",     "an",      "the",      "and",  "but",     "or",      "nor",     "because", "bc",
    "cuz",   "cos",     "although", "than", "whether", "very",    "just",    "my",      "your",
    "our",   "their",   "i'm",      "it's", "that's",  "there's", "you're",  "we're",   "they're",
    "he's",  "she's",   "what's",   "i've", "you've",  "we've",   "they've", "i'll",    "you'll",
    "we'll", "they'll", "it'll",    "i'd",  "you'd",   "we'd",    "they'd",  NULL};

/* Auxiliaries that open an inverted yes/no question ("did we ...", "are you"). */
static const char *const k_aux[] = {"did",  "do",  "does",  "are",  "is",    "was",
                                    "were", "can", "could", "will", "would", "should",
                                    "have", "has", "shall", "am",   NULL};
static const char *const k_subj[] = {"i", "we", "you", "they", "he", "she", "it", "u", "ya", NULL};

static bool rf_in_list(const char *const *list, const char *w, size_t n) {
    for (size_t i = 0; list[i]; i++)
        if (strlen(list[i]) == n && strncasecmp(list[i], w, n) == 0)
            return true;
    return false;
}

static bool rf_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* Word characters: ASCII letters, digits, apostrophes. A curly apostrophe
 * (U+2019, E2 80 99) is normalized by rf_copy_word. */
static bool rf_is_word_byte(unsigned char c) {
    return isalnum(c) || c == '\'';
}

/* Copy word[0..len) into buf, lowercased, with U+2019 folded to '\''.
 * Returns the copied length, 0 when the word does not fit. */
static size_t rf_copy_word(const char *w, size_t len, char *buf, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        if (o + 1 >= cap)
            return 0;
        unsigned char c = (unsigned char)w[i];
        if (c == 0xE2 && i + 2 < len && (unsigned char)w[i + 1] == 0x80 &&
            (unsigned char)w[i + 2] == 0x99) {
            buf[o++] = '\'';
            i += 2;
            continue;
        }
        buf[o++] = (char)tolower(c);
    }
    buf[o] = '\0';
    return o;
}

bool hu_reply_word_is_dangling(const char *word, size_t len) {
    char buf[16];
    if (!word || len == 0)
        return false;
    size_t n = rf_copy_word(word, len, buf, sizeof(buf));
    return n > 0 && rf_in_list(k_dangling, buf, n);
}

static size_t rf_trim_end(const char *t, size_t len) {
    while (len > 0 && rf_is_space(t[len - 1]))
        len--;
    return len;
}

/* Start of the word ending at `end` (word bytes plus curly-apostrophe bytes). */
static size_t rf_word_start(const char *t, size_t end) {
    size_t s = end;
    while (s > 0) {
        unsigned char c = (unsigned char)t[s - 1];
        if (rf_is_word_byte(c)) {
            s--;
        } else if (s >= 3 && c == 0x99 && (unsigned char)t[s - 2] == 0x80 &&
                   (unsigned char)t[s - 3] == 0xE2) {
            s -= 3;
        } else {
            break;
        }
    }
    return s;
}

static size_t rf_word_count(const char *t, size_t len) {
    size_t n = 0;
    bool in = false;
    for (size_t i = 0; i < len; i++) {
        bool w = !rf_is_space(t[i]);
        if (w && !in)
            n++;
        in = w;
    }
    return n;
}

/* Ends on a dangling word, with the "just because" exception (a complete
 * idiom: "why?" "just because"). */
static bool rf_ends_dangling(const char *t, size_t end) {
    size_t s = rf_word_start(t, end);
    if (s == end || !hu_reply_word_is_dangling(t + s, end - s))
        return false;
    char last[16];
    size_t ln = rf_copy_word(t + s, end - s, last, sizeof(last));
    if (ln > 0 && (strcmp(last, "because") == 0 || strcmp(last, "bc") == 0 ||
                   strcmp(last, "cuz") == 0 || strcmp(last, "cos") == 0)) {
        size_t pe = s;
        while (pe > 0 && rf_is_space(t[pe - 1]))
            pe--;
        size_t ps = rf_word_start(t, pe);
        if (pe - ps == 4 && strncasecmp(t + ps, "just", 4) == 0)
            return false;
    }
    return true;
}

static bool rf_unbalanced(const char *t, size_t len) {
    int paren = 0;
    size_t dq = 0, curly_open = 0, curly_close = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)t[i];
        if (c == '(')
            paren++;
        else if (c == ')')
            paren--;
        else if (c == '"')
            dq++;
        else if (c == 0xE2 && i + 2 < len && (unsigned char)t[i + 1] == 0x80) {
            if ((unsigned char)t[i + 2] == 0x9C)
                curly_open++;
            else if ((unsigned char)t[i + 2] == 0x9D)
                curly_close++;
        }
    }
    return paren > 0 || (dq % 2) == 1 || curly_open > curly_close;
}

/* Sentence-case text ("Wait, ...") whose final clause is an inverted question
 * with no "?" anywhere: a writer who capitalizes and punctuates would close
 * the question, so the missing "?" means the text was cut. Lowercase casual
 * texts ("did you eat") never trip this. */
static bool rf_unclosed_question(const char *t, size_t len) {
    if (memchr(t, '?', len))
        return false;
    size_t i = 0;
    while (i < len && !isalpha((unsigned char)t[i]))
        i++;
    if (i >= len || !isupper((unsigned char)t[i]))
        return false;
    size_t c = len; /* start of the final clause */
    while (c > 0 && !strchr(".!;,", t[c - 1]))
        c--;
    while (c < len && !isalpha((unsigned char)t[c]))
        c++;
    size_t a = c;
    while (a < len && isalpha((unsigned char)t[a]))
        a++;
    if (!rf_in_list(k_aux, t + c, a - c))
        return false;
    while (a < len && rf_is_space(t[a]))
        a++;
    size_t b = a;
    while (b < len && isalpha((unsigned char)t[b]))
        b++;
    return b > a && rf_in_list(k_subj, t + a, b - a) && b < len;
}

bool hu_reply_is_fragment(const char *text, size_t len) {
    if (!text)
        return false;
    len = rf_trim_end(text, len);
    if (len == 0)
        return false;
    if (rf_unbalanced(text, len))
        return true;
    unsigned char last = (unsigned char)text[len - 1];
    if (strchr(",;:-(&/", last))
        return true;
    /* En/em dash (E2 80 93 / E2 80 94) promise more too. */
    if (len >= 3 && (unsigned char)text[len - 3] == 0xE2 && (unsigned char)text[len - 2] == 0x80 &&
        ((unsigned char)text[len - 1] == 0x93 || (unsigned char)text[len - 1] == 0x94))
        return true;
    if (strchr(".!?)\"*~", last))
        return false;
    if (last >= 0x80) {
        /* Emoji / ellipsis / curly closer ends a text. A curly apostrophe
         * ends a contraction word ("it’s"), handled below. */
        bool curly_apos = len >= 3 && last == 0x99 && (unsigned char)text[len - 2] == 0x80 &&
                          (unsigned char)text[len - 3] == 0xE2;
        if (!curly_apos)
            return false;
    }
    if (rf_ends_dangling(text, len))
        return true;
    return rf_unclosed_question(text, len);
}

size_t hu_reply_trim_to_sentence(const char *text, size_t len, size_t cap) {
    if (!text || len == 0)
        return 0;
    size_t hi = cap < len ? cap : len;
    for (size_t i = hi; i > 0; i--) {
        bool at_end = i == len || rf_is_space(text[i]);
        if (!at_end)
            continue;
        unsigned char c = (unsigned char)text[i - 1];
        if (c == '.' || c == '!' || c == '?')
            return i;
        /* U+2026 horizontal ellipsis */
        if (i >= 3 && c == 0xA6 && (unsigned char)text[i - 2] == 0x80 &&
            (unsigned char)text[i - 3] == 0xE2)
            return i;
    }
    return 0;
}

size_t hu_reply_drop_dangling_tail(const char *text, size_t len) {
    if (!text)
        return len;
    size_t end = rf_trim_end(text, len);
    size_t s = rf_word_start(text, end);
    if (s == end || !rf_ends_dangling(text, end))
        return len;
    size_t p = s;
    while (p > 0 && rf_is_space(text[p - 1]))
        p--;
    if (p == 0 || !strchr(".!?,;", text[p - 1]))
        return len; /* mid-clause, or the whole text: dropping would not help */
    if (text[p - 1] == ',' || text[p - 1] == ';')
        p--;
    while (p > 0 && rf_is_space(text[p - 1]))
        p--;
    return p > 0 ? p : len;
}

/* A bubble break may also not land after a preposition, pronoun or bare
 * auxiliary ("talking about the best of" | "the summers"). These complete
 * plenty of utterances, so they are not fragment evidence, but a splitter can
 * always pick another space, so for choosing a cut they cost nothing. */
static const char *const k_weak_cut[] = {
    "of",     "to",       "in",    "on",    "at",    "for",   "with", "from", "about", "into",
    "by",     "as",       "so",    "that",  "this",  "is",    "was",  "are",  "were",  "be",
    "i",      "we",       "you",   "they",  "he",    "she",   "it",   "if",   "when",  "like",
    "really", "actually", "gonna", "wanna", "kinda", "sorta", NULL};

bool hu_reply_cut_is_clean(const char *text, size_t len, size_t cut) {
    if (!text || cut == 0 || cut >= len)
        return true;
    size_t left = rf_trim_end(text, cut);
    if (left > 0 && rf_is_word_byte((unsigned char)text[left - 1])) {
        if (rf_ends_dangling(text, left))
            return false;
        size_t s = rf_word_start(text, left);
        char w[16];
        size_t n = rf_copy_word(text + s, left - s, w, sizeof(w));
        if (n > 0 && rf_in_list(k_weak_cut, w, n))
            return false;
    }
    return rf_word_count(text + cut, len - cut) >= 2;
}

static atomic_uint_fast64_t s_final_fragments;

size_t hu_reply_final_check(const char *text, size_t len, void *observer) {
    if (!text || !hu_reply_is_fragment(text, len))
        return len;
    uint64_t n =
        (uint64_t)atomic_fetch_add_explicit(&s_final_fragments, 1, memory_order_relaxed) + 1;
    size_t keep = hu_reply_drop_dangling_tail(text, len);
    hu_log_warn("outbound", (hu_observer_t *)observer,
                "[reply_fragment] outbound ends mid-thought (len=%zu dropped=%zu count=%llu)", len,
                len - keep, (unsigned long long)n);
    return keep;
}

uint64_t hu_reply_final_fragment_count(void) {
    return (uint64_t)atomic_load_explicit(&s_final_fragments, memory_order_relaxed);
}
