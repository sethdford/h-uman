/* The one self-harm detector. See include/human/security/self_harm.h. */
#include "human/security/self_harm.h"

#include <stdlib.h>
#include <string.h>

/* ── Phrase table ─────────────────────────────────────────────────────── */

/* Phrases are written in canonical form: lowercase, apostrophes dropped,
 * single spaces. INHERENT_THIRD phrases name someone else in the phrase itself
 * ("kill himself"); the others look back at the subject. KILL marks phrases
 * the violence check must not read as violence against others. Order matters:
 * the first accepted match covers its words, so "died by suicide" must come
 * before the bare "suicide". */
enum { F_THIRD = 1u << 0, F_KILL = 1u << 1, F_NO_NUMBER = 1u << 2 };

typedef struct {
    const char *words;
    hu_self_harm_tier_t tier;
    unsigned flags;
} phrase_t;

static const phrase_t PHRASES[] = {
    /* someone else, by the phrase itself */
    {"wants to kill himself", HU_SELF_HARM_THIRD_PERSON, F_THIRD | F_KILL},
    {"wants to kill herself", HU_SELF_HARM_THIRD_PERSON, F_THIRD | F_KILL},
    {"wants to kill themselves", HU_SELF_HARM_THIRD_PERSON, F_THIRD | F_KILL},
    {"kill himself", HU_SELF_HARM_THIRD_PERSON, F_THIRD | F_KILL},
    {"kill herself", HU_SELF_HARM_THIRD_PERSON, F_THIRD | F_KILL},
    {"kill themselves", HU_SELF_HARM_THIRD_PERSON, F_THIRD | F_KILL},
    {"killed himself", HU_SELF_HARM_THIRD_PERSON, F_THIRD | F_KILL},
    {"killed herself", HU_SELF_HARM_THIRD_PERSON, F_THIRD | F_KILL},
    {"killed themselves", HU_SELF_HARM_THIRD_PERSON, F_THIRD | F_KILL},
    {"killing himself", HU_SELF_HARM_THIRD_PERSON, F_THIRD | F_KILL},
    {"killing herself", HU_SELF_HARM_THIRD_PERSON, F_THIRD | F_KILL},
    {"killing themselves", HU_SELF_HARM_THIRD_PERSON, F_THIRD | F_KILL},
    {"wants to die", HU_SELF_HARM_THIRD_PERSON, F_THIRD},
    {"died by suicide", HU_SELF_HARM_THIRD_PERSON, F_THIRD},
    {"committed suicide", HU_SELF_HARM_THIRD_PERSON, F_THIRD},
    /* first-person intent, ranked before any violence keyword */
    {"kill myself", HU_SELF_HARM_EXPLICIT, F_KILL},
    {"killing myself", HU_SELF_HARM_EXPLICIT, F_KILL},
    {"kms", HU_SELF_HARM_EXPLICIT, F_NO_NUMBER},
    {"unalive myself", HU_SELF_HARM_EXPLICIT, 0},
    {"unalive", HU_SELF_HARM_EXPLICIT, 0},
    {"end my life", HU_SELF_HARM_EXPLICIT, 0},
    {"ending my life", HU_SELF_HARM_EXPLICIT, 0},
    {"take my own life", HU_SELF_HARM_EXPLICIT, 0},
    {"take my life", HU_SELF_HARM_EXPLICIT, 0},
    {"harm myself", HU_SELF_HARM_EXPLICIT, 0},
    {"want to die", HU_SELF_HARM_EXPLICIT, 0},
    {"wanna die", HU_SELF_HARM_EXPLICIT, 0},
    {"wanting to die", HU_SELF_HARM_EXPLICIT, 0},
    {"wish i was dead", HU_SELF_HARM_EXPLICIT, 0},
    {"wish i were dead", HU_SELF_HARM_EXPLICIT, 0},
    {"end it all", HU_SELF_HARM_EXPLICIT, 0},
    {"ending it all", HU_SELF_HARM_EXPLICIT, 0},
    {"better off without me", HU_SELF_HARM_EXPLICIT, 0},
    {"better off dead", HU_SELF_HARM_EXPLICIT, 0},
    {"dont want to be here anymore", HU_SELF_HARM_EXPLICIT, 0},
    {"not want to be here anymore", HU_SELF_HARM_EXPLICIT, 0},
    {"dont want to live", HU_SELF_HARM_EXPLICIT, 0},
    {"dont wanna live", HU_SELF_HARM_EXPLICIT, 0},
    {"no reason to go on", HU_SELF_HARM_EXPLICIT, 0},
    {"nothing to live for", HU_SELF_HARM_EXPLICIT, 0},
    {"suicidal", HU_SELF_HARM_EXPLICIT, 0},
    {"suicide", HU_SELF_HARM_EXPLICIT, 0},
    {"self harm", HU_SELF_HARM_EXPLICIT, 0},
    {"self harming", HU_SELF_HARM_EXPLICIT, 0},
    {"selfharm", HU_SELF_HARM_EXPLICIT, 0},
    /* ambiguous, often venting: a gentle check-in */
    {"whats the point", HU_SELF_HARM_LOW, 0},
    {"what is the point", HU_SELF_HARM_LOW, 0},
    {"cant do this anymore", HU_SELF_HARM_LOW, 0},
    {"cannot do this anymore", HU_SELF_HARM_LOW, 0},
    {"cant go on", HU_SELF_HARM_LOW, 0},
    {"cannot go on", HU_SELF_HARM_LOW, 0},
    {"cant take it anymore", HU_SELF_HARM_LOW, 0},
    {"cant take this anymore", HU_SELF_HARM_LOW, 0},
};
#define N_PHRASES (sizeof(PHRASES) / sizeof(PHRASES[0]))

/* Spaced-letter evasion ("s e l f h a r m"): the joined letters are checked
 * only when the text has a run of single-letter tokens, so ordinary words
 * that happen to concatenate ("friend my life") never match. */
static const char *const JOINED[] = {
    "killmyself", "suicide", "selfharm", "unalive", "enditall", "wanttodie", "betteroffwithoutme",
};
#define N_JOINED (sizeof(JOINED) / sizeof(JOINED[0]))

static const char *const NEGATIONS[] = {"dont",   "not",   "never", "wont",  "wouldnt",
                                        "doesnt", "didnt", "isnt",  "arent", NULL};
static const char *const FIRST_PERSON[] = {"i",  "im", "id",     "ill", "ive",
                                           "me", "my", "myself", NULL};
static const char *const THIRD_PERSON[] = {
    "he",      "she",      "they",    "hes",     "shes",      "theyre",     "hed",      "theyd",
    "him",     "her",      "them",    "friend",  "buddy",     "brother",    "bro",      "sister",
    "sis",     "mom",      "mother",  "dad",     "father",    "son",        "daughter", "cousin",
    "kid",     "wife",     "husband", "partner", "boyfriend", "girlfriend", "roommate", "coworker",
    "someone", "somebody", "uncle",   "aunt",    "grandma",   "grandpa",    NULL};
/* Words skipped while looking back for the subject. */
static const char *const FILLER[] = {
    "just",     "really",   "literally", "actually",  "kinda",     "kind",    "of",     "lowkey",
    "honestly", "so",       "still",     "sometimes", "seriously", "totally", "gonna",  "going",
    "to",       "want",     "wants",     "could",     "would",     "might",   "should", "about",
    "thinking", "think",    "thinks",    "feel",      "feels",     "feeling", "like",   "is",
    "was",      "are",      "am",        "been",      "being",     "be",      "seems",  "gets",
    "getting",  "has",      "have",      "had",       "says",      "said",    "keeps",  "kept",
    "do",       "does",     "did",       "a",         "bit",       "lil",     "little", "low",
    "key",      "everyone", "everyones", "all",       "probably",  "maybe",   "also",   "too",
    NULL};

/* ── Canonical tokens ─────────────────────────────────────────────────── */

#define WIN_BYTES   4096
#define WIN_OVERLAP 256
#define MAX_TOK     1024

typedef struct {
    char canon[WIN_BYTES * 2];
    size_t tok_off[MAX_TOK]; /* offset into canon */
    size_t tok_len[MAX_TOK];
    size_t raw_start[MAX_TOK]; /* byte offsets into the raw window */
    size_t raw_end[MAX_TOK];
    size_t n;
} toks_t;

static bool is_alpha(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static bool is_digit(unsigned char c) {
    return c >= '0' && c <= '9';
}
/* Leetspeak digits and symbols, and the letter each stands for. */
static const char LEET_FROM[] = "013457@$";
static const char LEET_TO[] = "oieastas";

static bool is_leet(unsigned char c) {
    return c != 0 && strchr(LEET_FROM, (int)c) != NULL;
}
static char leet_map(unsigned char c) {
    const char *p = c ? strchr(LEET_FROM, (int)c) : NULL;
    return p ? LEET_TO[p - LEET_FROM] : (char)c;
}

/* Bytes of an apostrophe at p: ' ` U+2018 U+2019 (dropped, joining the word). */
static size_t apostrophe_len(const unsigned char *p, size_t rem) {
    if (p[0] == '\'' || p[0] == '`')
        return 1;
    if (rem >= 3 && p[0] == 0xE2 && p[1] == 0x80 && (p[2] == 0x98 || p[2] == 0x99))
        return 3;
    return 0;
}

static void tokenize(const char *text, size_t len, toks_t *t) {
    const unsigned char *s = (const unsigned char *)text;
    size_t i = 0, c = 0;
    t->n = 0;
    while (i < len && t->n < MAX_TOK) {
        /* skip separators */
        while (i < len && !is_alpha(s[i]) && !is_digit(s[i]) && s[i] != '@' && s[i] != '$')
            i++;
        if (i >= len)
            break;
        size_t start = i, tok_c = c;
        bool letters = false, leet = false;
        size_t j = i;
        for (;;) {
            if (j >= len)
                break;
            size_t ap = apostrophe_len(s + j, len - j);
            if (ap > 0 && j > start && j + ap < len && is_alpha(s[j + ap])) {
                j += ap; /* "can't" -> "cant" */
                continue;
            }
            unsigned char ch = s[j];
            if (!is_alpha(ch) && !is_digit(ch) && ch != '@' && ch != '$')
                break;
            letters |= is_alpha(ch);
            leet |= is_leet(ch);
            j++;
        }
        /* second pass: write the token; leet chars map only inside tokens that
         * also carry letters ("k1ll"), so "5" stays a number */
        for (size_t k = start; k < j && c + 1 < sizeof(t->canon); k++) {
            size_t ap = apostrophe_len(s + k, j - k);
            if (ap > 0) {
                k += ap - 1;
                continue;
            }
            unsigned char ch = s[k];
            char out;
            if (is_alpha(ch))
                out = (char)(ch | 0x20);
            else if (letters && leet)
                out = leet_map(ch);
            else
                out = (char)ch;
            t->canon[c++] = out;
        }
        t->tok_off[t->n] = tok_c;
        t->tok_len[t->n] = c - tok_c;
        t->raw_start[t->n] = start;
        t->raw_end[t->n] = j;
        t->n++;
        i = j;
    }
}

static bool tok_is(const toks_t *t, size_t k, const char *w, size_t wlen) {
    return t->tok_len[k] == wlen && memcmp(t->canon + t->tok_off[k], w, wlen) == 0;
}

static bool tok_in(const toks_t *t, size_t k, const char *const *list) {
    for (size_t i = 0; list[i]; i++)
        if (tok_is(t, k, list[i], strlen(list[i])))
            return true;
    return false;
}

static bool tok_numeric(const toks_t *t, size_t k) {
    if (t->tok_len[k] == 0)
        return false;
    for (size_t i = 0; i < t->tok_len[k]; i++)
        if (!is_digit((unsigned char)t->canon[t->tok_off[k] + i]))
            return false;
    return true;
}

/* Does phrase p match the tokens starting at k? Returns its word count, or 0. */
static size_t phrase_match(const toks_t *t, size_t k, const char *p) {
    size_t n = 0;
    while (*p) {
        const char *sp = strchr(p, ' ');
        size_t wlen = sp ? (size_t)(sp - p) : strlen(p);
        if (k + n >= t->n || !tok_is(t, k + n, p, wlen))
            return 0;
        n++;
        p += wlen;
        if (*p == ' ')
            p++;
    }
    return n;
}

typedef enum { SUBJ_FIRST, SUBJ_THIRD, SUBJ_NEGATED } subject_t;

/* Look back from token k for the subject, skipping filler words. */
static subject_t subject_before(const toks_t *t, size_t k) {
    for (size_t back = 0; back < 6 && k > back; back++) {
        size_t w = k - 1 - back;
        if (tok_in(t, w, NEGATIONS))
            return SUBJ_NEGATED;
        if (tok_in(t, w, THIRD_PERSON))
            return SUBJ_THIRD;
        if (tok_in(t, w, FIRST_PERSON))
            return SUBJ_FIRST;
        if (!tok_in(t, w, FILLER))
            break;
    }
    return SUBJ_FIRST; /* texts drop the subject: "wanna die lol" */
}

/* One match callback: tier and the raw byte span of the phrase. */
typedef void (*on_match_fn)(void *ud, hu_self_harm_tier_t tier, unsigned flags, size_t raw_start,
                            size_t raw_end);

static hu_self_harm_tier_t scan_window(const char *text, size_t len, on_match_fn cb, void *ud) {
    toks_t *t = (toks_t *)malloc(sizeof(toks_t));
    if (!t)
        return HU_SELF_HARM_NONE;
    tokenize(text, len, t);
    bool covered[MAX_TOK];
    memset(covered, 0, sizeof(covered));
    bool explicit_hit = false, low_hit = false, third_hit = false;

    for (size_t pi = 0; pi < N_PHRASES; pi++) {
        const phrase_t *ph = &PHRASES[pi];
        for (size_t k = 0; k < t->n; k++) {
            size_t n = phrase_match(t, k, ph->words);
            if (n == 0)
                continue;
            bool overlap = false;
            for (size_t j = k; j < k + n; j++)
                overlap |= covered[j];
            if (overlap)
                continue;
            if ((ph->flags & F_NO_NUMBER) && k > 0 && tok_numeric(t, k - 1))
                continue; /* "5 kms" is a distance */
            for (size_t j = k; j < k + n; j++)
                covered[j] = true;
            subject_t subj = subject_before(t, k);
            if (cb)
                cb(ud, ph->tier, ph->flags, t->raw_start[k], t->raw_end[k + n - 1]);
            if (subj == SUBJ_NEGATED)
                continue;
            if ((ph->flags & F_THIRD) || subj == SUBJ_THIRD)
                third_hit = true;
            else if (ph->tier == HU_SELF_HARM_EXPLICIT)
                explicit_hit = true;
            else
                low_hit = true;
        }
    }

    /* Spaced-letter evasion: a run of 4+ single-letter tokens. */
    if (!explicit_hit) {
        size_t run = 0, best = 0;
        for (size_t k = 0; k < t->n; k++) {
            run = (t->tok_len[k] == 1) ? run + 1 : 0;
            if (run > best)
                best = run;
        }
        if (best >= 4) {
            char joined[WIN_BYTES * 2];
            size_t jn = 0;
            for (size_t k = 0; k < t->n && jn + t->tok_len[k] < sizeof(joined); k++) {
                memcpy(joined + jn, t->canon + t->tok_off[k], t->tok_len[k]);
                jn += t->tok_len[k];
            }
            joined[jn] = '\0';
            for (size_t i = 0; i < N_JOINED; i++)
                if (strstr(joined, JOINED[i]))
                    explicit_hit = true;
        }
    }
    free(t);
    if (explicit_hit)
        return HU_SELF_HARM_EXPLICIT;
    if (low_hit)
        return HU_SELF_HARM_LOW;
    if (third_hit)
        return HU_SELF_HARM_THIRD_PERSON;
    return HU_SELF_HARM_NONE;
}

/* Walk the text in overlapping 4 KB windows so a phrase at the end of a long
 * batch is still read; the highest tier wins. */
static hu_self_harm_tier_t scan(const char *text, size_t len, on_match_fn cb, void *ud) {
    hu_self_harm_tier_t best = HU_SELF_HARM_NONE;
    size_t off = 0;
    for (;;) {
        size_t n = len - off < WIN_BYTES ? len - off : WIN_BYTES;
        hu_self_harm_tier_t w = scan_window(text + off, n, cb, ud);
        if (w > best)
            best = w;
        if (off + n >= len)
            break;
        off += WIN_BYTES - WIN_OVERLAP;
    }
    return best;
}

hu_self_harm_tier_t hu_self_harm_classify(const char *text, size_t len) {
    if (!text || len == 0)
        return HU_SELF_HARM_NONE;
    return scan(text, len, NULL, NULL);
}

/* ── Kill-phrase mask (for the violence check) ────────────────────────── */

typedef struct {
    char *out;
    size_t out_len;
    size_t win_off;
} mask_ud_t;

static void mask_cb(void *ud, hu_self_harm_tier_t tier, unsigned flags, size_t rs, size_t re) {
    (void)tier;
    mask_ud_t *m = (mask_ud_t *)ud;
    if (!(flags & F_KILL))
        return;
    for (size_t i = m->win_off + rs; i < m->win_off + re && i < m->out_len; i++)
        m->out[i] = ' ';
}

size_t hu_self_harm_mask_kill_phrases(const char *text, size_t len, char *out, size_t cap) {
    if (!out || cap == 0)
        return 0;
    if (!text)
        len = 0;
    size_t n = len < cap ? len : cap - 1;
    if (n)
        memcpy(out, text, n);
    out[n] = '\0';
    mask_ud_t m = {.out = out, .out_len = n, .win_off = 0};
    size_t off = 0;
    while (off < n) {
        size_t w = n - off < WIN_BYTES ? n - off : WIN_BYTES;
        m.win_off = off;
        (void)scan_window(text + off, w, mask_cb, &m);
        if (off + w >= n)
            break;
        off += WIN_BYTES - WIN_OVERLAP;
    }
    return n;
}

/* ── Names, directives, gate ──────────────────────────────────────────── */

const char *hu_self_harm_tier_name(hu_self_harm_tier_t tier) {
    switch (tier) {
    case HU_SELF_HARM_THIRD_PERSON:
        return "third_person";
    case HU_SELF_HARM_LOW:
        return "low";
    case HU_SELF_HARM_EXPLICIT:
        return "explicit";
    default:
        return "none";
    }
}

/* The SHIELD-005 directive, byte-identical to the one daemon.c carried inline. */
static const char DIRECTIVE_EXPLICIT[] = "[CRISIS SUPPORT]: The user may be in distress. "
                                         "Respond with empathy and care. Include crisis resources: "
                                         "988 Suicide & Crisis Lifeline (call/text 988), "
                                         "Crisis Text Line (text HOME to 741741). "
                                         "Do not dismiss their feelings. Do not give advice. "
                                         "Listen and validate.\n";
static const char DIRECTIVE_LOW[] =
    "[CHECK-IN]: They sound low or worn down. Gently check in on how they're really doing, in "
    "your own words, and take it seriously without sounding alarmed. Stay with them; don't "
    "brush it off or change the subject. Don't list hotlines unless they say more.\n";
static const char DIRECTIVE_THIRD[] =
    "[SUPPORT]: They may be worried about someone else who could be at risk. Support them as "
    "the helper: ask what's happening and how they're holding up. If that person may be in "
    "danger, mention that 988 (call/text) helps people supporting someone too. Don't treat "
    "them as the one in crisis.\n";

const char *hu_self_harm_directive(hu_self_harm_tier_t tier, size_t *len_out) {
    const char *d = NULL;
    size_t n = 0;
    switch (tier) {
    case HU_SELF_HARM_EXPLICIT:
        d = DIRECTIVE_EXPLICIT;
        n = sizeof(DIRECTIVE_EXPLICIT) - 1;
        break;
    case HU_SELF_HARM_LOW:
        d = DIRECTIVE_LOW;
        n = sizeof(DIRECTIVE_LOW) - 1;
        break;
    case HU_SELF_HARM_THIRD_PERSON:
        d = DIRECTIVE_THIRD;
        n = sizeof(DIRECTIVE_THIRD) - 1;
        break;
    default:
        break;
    }
    if (len_out)
        *len_out = n;
    return d;
}

static int g_mode_override = -1;

hu_gate_mode_t hu_crisis_tiers_mode(void) {
    if (g_mode_override >= 0)
        return (hu_gate_mode_t)g_mode_override;
    return hu_gate_mode_from_env("HU_CRISIS_TIERS", HU_GATE_LIVE);
}

void hu_crisis_tiers_mode_set_for_test(int mode_or_minus1) {
    g_mode_override = mode_or_minus1;
}
