/* F1 spoken memos: S2 cleanup + S3 drift guard. Pure; see
 * include/human/tts/speech_text.h. */
#include "human/tts/speech_text.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

#define SPEECH_WORK_CAP  4096
#define SPEECH_MAX_WORDS 512

/* ── helpers ──────────────────────────────────────────────────────────── */

static bool word_in(const char *w, size_t n, const char *const *list, size_t count) {
    for (size_t i = 0; i < count; i++)
        if (strlen(list[i]) == n && strncasecmp(w, list[i], n) == 0)
            return true;
    return false;
}

/* Narrated actions: dropped whole when they are the entire *...* / (...) body. */
static const char *const ACTION_WORDS[] = {
    "laughs", "laughing", "laugh", "smiles", "smiling",  "grins",   "grinning", "sighs",
    "sigh",   "shrugs",   "winks", "nods",   "chuckles", "giggles", "cries",    "hugs",
};
#define ACTION_WORD_COUNT (sizeof(ACTION_WORDS) / sizeof(ACTION_WORDS[0]))

static bool is_action_body(const char *s, size_t n) {
    while (n > 0 && isspace((unsigned char)*s)) {
        s++;
        n--;
    }
    while (n > 0 && isspace((unsigned char)s[n - 1]))
        n--;
    return n > 0 && word_in(s, n, ACTION_WORDS, ACTION_WORD_COUNT);
}

static size_t emoji_len(const unsigned char *p, size_t remain) {
    if (remain >= 4 && p[0] == 0xF0)
        return 4; /* U+10000.. : pictographs, symbols */
    if (remain >= 3 && p[0] == 0xE2 && p[1] >= 0x98 && p[1] <= 0x9E)
        return 3; /* U+2600..U+27BF : misc symbols, dingbats */
    if (remain >= 3 && p[0] == 0xEF && p[1] == 0xB8 && p[2] == 0x8F)
        return 3; /* U+FE0F variation selector */
    if (remain >= 3 && p[0] == 0xE2 && p[1] == 0x80 && p[2] == 0x8D)
        return 3; /* U+200D zero-width joiner */
    return 0;
}

static bool starts_url(const char *s, size_t n) {
    return (n >= 7 && strncasecmp(s, "http://", 7) == 0) ||
           (n >= 8 && strncasecmp(s, "https://", 8) == 0) ||
           (n >= 4 && strncasecmp(s, "www.", 4) == 0);
}

/* Pass 1: drop URLs, narrated actions, brackets and emoji; keep emphasis words
 * and parenthetical content. */
static size_t strip_structure(const char *in, size_t n, char *w, size_t cap, bool *had_url) {
    size_t o = 0;
    *had_url = false;
    for (size_t i = 0; i < n && o + 1 < cap;) {
        bool at_word_start = i == 0 || isspace((unsigned char)in[i - 1]);
        if (at_word_start && starts_url(in + i, n - i)) {
            *had_url = true;
            while (i < n && !isspace((unsigned char)in[i]))
                i++;
            continue;
        }
        char c = in[i];
        if (c == '*' || c == '(' || c == '[') {
            char close = (char)(c == '*' ? '*' : (c == '(' ? ')' : ']'));
            const char *end = memchr(in + i + 1, close, n - i - 1 < 80 ? n - i - 1 : 80);
            if (end) {
                const char *body = in + i + 1;
                size_t blen = (size_t)(end - body);
                bool drop = c == '[' || is_action_body(body, blen);
                if (!drop)
                    for (size_t k = 0; k < blen && o + 1 < cap; k++)
                        w[o++] = body[k];
                i = (size_t)(end - in) + 1;
                continue;
            }
        }
        size_t el = emoji_len((const unsigned char *)in + i, n - i);
        if (el) {
            i += el;
            continue;
        }
        w[o++] = c;
        i++;
    }
    w[o] = '\0';
    return o;
}

/* Pass 2 tables. */
typedef struct {
    const char *from;
    const char *to;
} shorthand_t;

static const shorthand_t SHORTHAND[] = {
    {"lmk", "let me know"}, {"tmrw", "tomorrow"}, {"tmr", "tomorrow"},      {"idk", "I don't know"},
    {"rn", "right now"},    {"bc", "because"},    {"ngl", "not gonna lie"}, {"tbh", "honestly"},
    {"omw", "on my way"},   {"u", "you"},         {"thx", "thanks"},        {"pls", "please"},
    {"plz", "please"},
};
#define SHORTHAND_COUNT (sizeof(SHORTHAND) / sizeof(SHORTHAND[0]))

/* "ur" -> "you're" before these (or an -ing word); otherwise "your". TTS says
 * the two almost the same, so a short list is enough. */
static const char *const YOURE_NEXT[] = {
    "a",       "an",   "the",   "so",   "not",   "gonna", "going", "free", "done",   "right",
    "welcome", "good", "ok",    "okay", "being", "still", "too",   "very", "really", "always",
    "never",   "here", "there", "home", "back",  "late",  "early", "all",  "such",   "sweet",
};
#define YOURE_NEXT_COUNT (sizeof(YOURE_NEXT) / sizeof(YOURE_NEXT[0]))

/* haha, hahaha, hehe, heehee… (at least two syllables, only h/a/e letters). */
static bool is_laugh_syllables(const char *core, size_t n) {
    if (n < 4 || tolower((unsigned char)core[0]) != 'h')
        return false;
    size_t i = 0;
    for (; i < n; i++) {
        int ch = tolower((unsigned char)core[i]);
        if (ch != 'h' && ch != 'a' && ch != 'e')
            break;
    }
    return i == n;
}

/* Seth-owned rule (spec F1 open point 1): what a laugh token becomes when
 * spoken. Default: say nothing, but let transcript prep add a real laugh. */
static bool laugh_token_rule(const char *core, size_t n) {
    static const char *const k_laughs[] = {"lol", "lmao", "lmfao", "rofl"};
    return word_in(core, n, k_laughs, sizeof(k_laughs) / sizeof(k_laughs[0])) ||
           is_laugh_syllables(core, n);
}

bool hu_speech_has_laugh_token(const char *s, size_t n) {
    if (!s)
        return false;
    for (size_t i = 0; i < n;) {
        while (i < n && !isalnum((unsigned char)s[i]))
            i++;
        size_t b = i;
        while (i < n && isalnum((unsigned char)s[i]))
            i++;
        if (i > b && laugh_token_rule(s + b, i - b))
            return true;
    }
    return false;
}

static bool append(char *out, size_t cap, size_t *o, const char *s, size_t n) {
    if (*o + n + 1 > cap) {
        n = cap > *o + 1 ? cap - *o - 1 : 0;
        memcpy(out + *o, s, n);
        *o += n;
        out[*o] = '\0';
        return false;
    }
    memcpy(out + *o, s, n);
    *o += n;
    out[*o] = '\0';
    return true;
}

typedef struct {
    const char *p; /* whole token */
    size_t n;
    size_t core_off; /* alnum/apostrophe core inside the token */
    size_t core_n;
} tok_t;

static void split_core(tok_t *t) {
    size_t b = 0, e = t->n;
    while (b < e && !isalnum((unsigned char)t->p[b]) && t->p[b] != '\'')
        b++;
    while (e > b && !isalnum((unsigned char)t->p[e - 1]) && t->p[e - 1] != '\'')
        e--;
    t->core_off = b;
    t->core_n = e - b;
}

size_t hu_speech_cleanup(const char *in, size_t in_len, char *out, size_t cap, bool *laughter_cue) {
    if (laughter_cue)
        *laughter_cue = false;
    if (!out || cap == 0)
        return 0;
    out[0] = '\0';
    if (!in || in_len == 0)
        return 0;

    char work[SPEECH_WORK_CAP];
    bool had_url = false;
    size_t wn = strip_structure(in, in_len, work, sizeof(work), &had_url);

    tok_t toks[SPEECH_MAX_WORDS];
    size_t nt = 0;
    for (size_t i = 0; i < wn && nt < SPEECH_MAX_WORDS;) {
        while (i < wn && isspace((unsigned char)work[i]))
            i++;
        size_t s = i;
        while (i < wn && !isspace((unsigned char)work[i]))
            i++;
        if (i > s) {
            toks[nt].p = work + s;
            toks[nt].n = i - s;
            split_core(&toks[nt]);
            nt++;
        }
    }

    size_t o = 0;
    for (size_t t = 0; t < nt; t++) {
        const tok_t *k = &toks[t];
        const char *core = k->p + k->core_off;
        size_t cn = k->core_n;
        const char *lead = k->p;
        size_t lead_n = k->core_off;
        const char *trail = core + cn;
        size_t trail_n = k->n - k->core_off - cn;

        if (cn > 0 && laugh_token_rule(core, cn)) {
            if (laughter_cue)
                *laughter_cue = true;
            /* keep punctuation that followed it, attached to the previous word */
            if (o > 0 && trail_n > 0 && !append(out, cap, &o, trail, trail_n))
                break;
            continue;
        }

        const char *rep = NULL;
        if (cn == 2 && strncasecmp(core, "ur", 2) == 0) {
            bool youre = false;
            if (t + 1 < nt) {
                const tok_t *nx = &toks[t + 1];
                const char *nc = nx->p + nx->core_off;
                size_t nn = nx->core_n;
                youre = word_in(nc, nn, YOURE_NEXT, YOURE_NEXT_COUNT) ||
                        (nn > 4 && strncasecmp(nc + nn - 3, "ing", 3) == 0);
            }
            rep = youre ? "you're" : "your";
        } else {
            for (size_t m = 0; m < SHORTHAND_COUNT; m++)
                if (strlen(SHORTHAND[m].from) == cn &&
                    strncasecmp(core, SHORTHAND[m].from, cn) == 0)
                    rep = SHORTHAND[m].to;
        }

        if (o > 0 && !append(out, cap, &o, " ", 1))
            break;
        if (!append(out, cap, &o, lead, lead_n))
            break;
        if (!(rep ? append(out, cap, &o, rep, strlen(rep)) : append(out, cap, &o, core, cn)))
            break;
        if (!append(out, cap, &o, trail, trail_n))
            break;
    }

    /* A leading laugh token can leave the text starting with its punctuation. */
    size_t skip = 0;
    while (skip < o && (out[skip] == ',' || out[skip] == ' '))
        skip++;
    if (skip) {
        memmove(out, out + skip, o - skip + 1);
        o -= skip;
    }
    while (o > 0 && isspace((unsigned char)out[o - 1]))
        out[--o] = '\0';

    if (o == 0 && had_url) {
        static const char link[] = "I'll send you the link";
        (void)append(out, cap, &o, link, sizeof(link) - 1);
    }
    return o;
}

/* ── S3 drift guard ───────────────────────────────────────────────────── */

/* Openers a spoken rewrite must never start with (Ferni bans them). */
static const char *const k_openers[] = {"well", "so", "hmm", "hmmm", "um", "uh"};

typedef struct {
    const char *p;
    size_t n;
    bool sentence_initial;
} dword_t;

static size_t words_of(const char *s, size_t n, dword_t *w, size_t cap) {
    size_t c = 0;
    bool initial = true;
    for (size_t i = 0; i < n && c < cap;) {
        if (!isalnum((unsigned char)s[i]) && s[i] != '\'') {
            if (s[i] == '.' || s[i] == '!' || s[i] == '?')
                initial = true;
            i++;
            continue;
        }
        size_t b = i;
        while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '\''))
            i++;
        w[c].p = s + b;
        w[c].n = i - b;
        w[c].sentence_initial = initial;
        initial = false;
        c++;
    }
    return c;
}

static bool has_word_ci(const dword_t *w, size_t n, const char *p, size_t len) {
    for (size_t i = 0; i < n; i++)
        if (w[i].n == len && strncasecmp(w[i].p, p, len) == 0)
            return true;
    return false;
}

static bool has_digit(const char *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (isdigit((unsigned char)p[i]))
            return true;
    return false;
}

static bool is_first_person_i(const char *p, size_t n) {
    return (n == 1 && p[0] == 'I') || (n >= 2 && p[0] == 'I' && p[1] == '\'');
}

static bool contains_ci(const char *h, size_t hn, const char *needle) {
    size_t nn = strlen(needle);
    for (size_t i = 0; i + nn <= hn; i++)
        if (strncasecmp(h + i, needle, nn) == 0)
            return true;
    return false;
}

hu_speech_drift_t hu_speech_drift_check(const char *orig, size_t on, const char *rew, size_t rn) {
    dword_t ow[SPEECH_MAX_WORDS], rw[SPEECH_MAX_WORDS];
    size_t oc = orig ? words_of(orig, on, ow, SPEECH_MAX_WORDS) : 0;
    size_t rc = rew ? words_of(rew, rn, rw, SPEECH_MAX_WORDS) : 0;

    for (size_t i = 0; i < rc; i++) /* digits are facts: times, amounts, counts */
        if (has_digit(rw[i].p, rw[i].n) && !has_word_ci(ow, oc, rw[i].p, rw[i].n))
            return HU_SPEECH_DRIFT_NEW_NUMBER;

    for (size_t i = 0; i < rc; i++) /* a new proper noun is a new fact */
        if (!rw[i].sentence_initial && isupper((unsigned char)rw[i].p[0]) &&
            !is_first_person_i(rw[i].p, rw[i].n) && !has_word_ci(ow, oc, rw[i].p, rw[i].n))
            return HU_SPEECH_DRIFT_NEW_NAME;

    bool oq = orig && memchr(orig, '?', on) != NULL;
    bool rq = rew && memchr(rew, '?', rn) != NULL;
    if (oq != rq)
        return HU_SPEECH_DRIFT_QUESTION;

    if (oc > 0 && (rc * 10 < oc * 5 || rc * 10 > oc * 16))
        return HU_SPEECH_DRIFT_LENGTH;

    if (rc > 0 && word_in(rw[0].p, rw[0].n, k_openers, sizeof(k_openers) / sizeof(k_openers[0])))
        return HU_SPEECH_DRIFT_BANNED;
    if (rew && (memchr(rew, '*', rn) || memchr(rew, '[', rn) || memchr(rew, '(', rn) ||
                contains_ci(rew, rn, "good question") || contains_ci(rew, rn, "great question")))
        return HU_SPEECH_DRIFT_BANNED;
    return HU_SPEECH_DRIFT_OK;
}

const char *hu_speech_drift_name(hu_speech_drift_t d) {
    switch (d) {
    case HU_SPEECH_DRIFT_OK:
        return "ok";
    case HU_SPEECH_DRIFT_NEW_NUMBER:
        return "new_number";
    case HU_SPEECH_DRIFT_NEW_NAME:
        return "new_name";
    case HU_SPEECH_DRIFT_QUESTION:
        return "question";
    case HU_SPEECH_DRIFT_LENGTH:
        return "length";
    case HU_SPEECH_DRIFT_BANNED:
        return "banned";
    }
    return "unknown";
}
