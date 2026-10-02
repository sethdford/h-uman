/* The one self-harm detector. See include/human/security/self_harm.h.
 *
 * Pipeline per 4 KB window:
 *   1. tokenize: lowercase words, apostrophes dropped ("can't", "can’t",
 *      "canʼt" all become "cant"), leet mapped only inside words that also have
 *      letters ("k1ll", not "5"), and a clause break recorded after any token
 *      followed by . , ! ? ; : ( ) or a newline;
 *   2. normalise: split joined verb+reflexive ("killmyself"), join "my self",
 *      expand contractions/slang ("dont" -> "do not", "wanna" -> "want to"), so
 *      one phrase covers every spelling;
 *   3. match: the verb+reflexive rule (kill/unalive/harm/hurt/cut + myself /
 *      himself / yourself, any tense), then the phrase table in order. The first
 *      match covers its tokens, so exclusions ("suicide squad", the 988 line)
 *      shadow the bare word;
 *   4. subject look-back (within the clause, skipping filler): negation drops
 *      the match, "you" (quoting the contact) drops it, a third-person subject
 *      makes it THIRD_PERSON unless the phrase's object is the speaker
 *      ("they'd be better off without me").
 * Explicit outranks low outranks third person. */
#include "human/security/self_harm.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Word lists ───────────────────────────────────────────────────────── */

static const char *const NEGATIONS[] = {"not", "never", NULL};
static const char *const FIRST_PERSON[] = {"i",  "im", "id",     "ill", "ive",
                                           "me", "my", "myself", NULL};
static const char *const SECOND_PERSON[] = {"you",  "youre", "youd", "youve",
                                            "your", "u",     "ya",   NULL};
static const char *const THIRD_PERSON[] = {
    "he",       "she",     "they",     "hes",     "shes",    "theyre",    "hed",        "shed",
    "theyd",    "him",     "her",      "them",    "friend",  "buddy",     "brother",    "bro",
    "sister",   "sis",     "mom",      "mother",  "dad",     "father",    "son",        "daughter",
    "cousin",   "kid",     "wife",     "husband", "partner", "boyfriend", "girlfriend", "roommate",
    "coworker", "someone", "somebody", "uncle",   "aunt",    "grandma",   "grandpa",    NULL};
/* Words that say the speaker means to do it ("going to", "want to"). */
static const char *const INTENT[] = {"want",     "wants",  "wanted",   "going", "will",    "ill",
                                     "about",    "ready",  "planning", "plan",  "decided", "need",
                                     "thinking", "think",  "should",   "might", "could",   "gotta",
                                     "try",      "trying", NULL};
/* Skipped while looking back for the subject. */
static const char *const FILLER[] = {
    "just",     "really",   "literally", "actually", "kinda",     "kind",      "of",
    "lowkey",   "honestly", "so",        "still",    "sometimes", "seriously", "totally",
    "going",    "to",       "want",      "wants",    "wanted",    "will",      "would",
    "could",    "might",    "should",    "about",    "thinking",  "think",     "thinks",
    "feel",     "feels",    "feeling",   "like",     "is",        "was",       "were",
    "are",      "am",       "been",      "being",    "be",        "seems",     "gets",
    "getting",  "has",      "have",      "had",      "says",      "said",      "say",
    "saying",   "told",     "keeps",     "keep",     "kept",      "do",        "does",
    "did",      "a",        "bit",       "lil",      "little",    "everyone",  "everyones",
    "all",      "probably", "maybe",     "also",     "too",       "even",      "ready",
    "planning", "plan",     "decided",   "need",     "gotta",     "got",       "try",
    "trying",   "tried",    "almost",    "nearly",   "fucking",   "fr",        "rn",
    NULL};
/* A clause may end in one of these without punctuation ("can't go on like this"). */
static const char *const TAIL[] = {
    "anymore", "any",   "longer", "like", "lol",     "honestly", "tbh",  "though", "really",
    "tonight", "today", "now",    "soon", "already", "man",      "dude", "bro",    "ugh",
    "haha",    "idk",   "smh",    "i",    "im",      "fr",       "rn",   "again",  NULL};
/* "what's the point of …" reads as despair only with these objects. */
static const char *const DESPAIR_OBJ[] = {
    "it",  "anything", "everything", "living", "life",  "trying", "me",   "going",
    "any", "even",     "getting",    "waking", "being", "myself", "this", NULL};
/* Words that make "kms" a distance. */
static const char *const QUANTITY[] = {"few",      "many",  "several", "some",  "couple",
                                       "more",     "less",  "fewer",   "extra", "hundred",
                                       "thousand", "dozen", "the",     "a",     NULL};
static const char *const KMS_AFTER[] = {"away",  "from", "per",   "of",    "left",   "long",
                                        "run",   "walk", "drive", "ride",  "to",     "north",
                                        "south", "east", "west",  "today", "uphill", NULL};
/* After "cut myself": an accident, not self-harm. */
static const char *const ACCIDENT[] = {"shaving",      "cooking",  "on",      "by",
                                       "accidentally", "chopping", "opening", "while",
                                       "with",         "slicing",  NULL};

/* ── Phrase table ─────────────────────────────────────────────────────── */

enum {
    F_THIRD = 1u << 0,        /* someone else by the phrase itself */
    F_KILL = 1u << 1,         /* masked before the violence check */
    F_SELF = 1u << 2,         /* the speaker is the object: never demoted to third */
    F_NO_NUMBER = 1u << 3,    /* "kms": not a distance */
    F_CLAUSE_END = 1u << 4,   /* must end its clause ("can't go on" not "go on the trip") */
    F_DESPAIR = 1u << 5,      /* "what's the point of <despair object>" also counts */
    F_NEEDS_INTENT = 1u << 6, /* "end it": only with "want to / going to …" */
    F_PERSONAL = 1u << 7,     /* "suicide": EXPLICIT only with a first-person subject */
};

typedef struct {
    const char *words; /* normalised form, single spaces */
    hu_self_harm_tier_t tier;
    unsigned flags;
} phrase_t;

static const phrase_t PHRASES[] = {
    /* exclusions: titles, services, and the resource line the directive asks for */
    {"suicide squad", HU_SELF_HARM_NONE, 0},
    {"suicide crisis lifeline", HU_SELF_HARM_NONE, 0},
    {"suicide and crisis lifeline", HU_SELF_HARM_NONE, 0},
    {"suicide prevention", HU_SELF_HARM_NONE, 0},
    {"suicide hotline", HU_SELF_HARM_NONE, 0},
    {"suicide lifeline", HU_SELF_HARM_NONE, 0},
    {"suicide rate", HU_SELF_HARM_NONE, 0},
    {"suicide rates", HU_SELF_HARM_NONE, 0},
    {"suicide awareness", HU_SELF_HARM_NONE, 0},
    /* someone else, by the phrase itself */
    {"wants to die", HU_SELF_HARM_THIRD_PERSON, F_THIRD},
    {"died by suicide", HU_SELF_HARM_THIRD_PERSON, F_THIRD},
    {"committed suicide", HU_SELF_HARM_THIRD_PERSON, F_THIRD},
    /* the speaker as object: whoever the grammatical subject is */
    {"better off without me", HU_SELF_HARM_EXPLICIT, F_SELF},
    {"end my life", HU_SELF_HARM_EXPLICIT, F_SELF},
    {"ending my life", HU_SELF_HARM_EXPLICIT, F_SELF},
    {"take my own life", HU_SELF_HARM_EXPLICIT, F_SELF},
    {"take my life", HU_SELF_HARM_EXPLICIT, F_SELF},
    {"wish i was dead", HU_SELF_HARM_EXPLICIT, F_SELF},
    {"wish i were dead", HU_SELF_HARM_EXPLICIT, F_SELF},
    {"kms", HU_SELF_HARM_EXPLICIT, F_SELF | F_NO_NUMBER},
    /* first-person intent */
    {"want to die", HU_SELF_HARM_EXPLICIT, 0},
    {"wanting to die", HU_SELF_HARM_EXPLICIT, 0},
    {"end it all", HU_SELF_HARM_EXPLICIT, 0},
    {"ending it all", HU_SELF_HARM_EXPLICIT, 0},
    {"better off dead", HU_SELF_HARM_EXPLICIT, 0},
    {"do not want to be here anymore", HU_SELF_HARM_EXPLICIT, 0},
    {"do not want to be alive", HU_SELF_HARM_EXPLICIT, 0},
    {"do not want to live", HU_SELF_HARM_EXPLICIT, 0},
    {"do not want to exist", HU_SELF_HARM_EXPLICIT, 0},
    {"no reason to live", HU_SELF_HARM_EXPLICIT, 0},
    {"no reason to go on", HU_SELF_HARM_EXPLICIT, 0},
    {"nothing to live for", HU_SELF_HARM_EXPLICIT, 0},
    {"suicide ideation", HU_SELF_HARM_EXPLICIT, 0},
    {"suicidal", HU_SELF_HARM_EXPLICIT, 0},
    {"self harm", HU_SELF_HARM_EXPLICIT, 0},
    {"self harming", HU_SELF_HARM_EXPLICIT, 0},
    {"selfharm", HU_SELF_HARM_EXPLICIT, 0},
    {"unalive", HU_SELF_HARM_EXPLICIT, 0},
    {"end it", HU_SELF_HARM_EXPLICIT, F_CLAUSE_END | F_NEEDS_INTENT},
    {"suicide", HU_SELF_HARM_EXPLICIT, F_PERSONAL},
    /* ambiguous, often venting: a gentle check-in */
    {"do not want to be here", HU_SELF_HARM_LOW, 0},
    {"want to disappear", HU_SELF_HARM_LOW, 0},
    {"whats the point", HU_SELF_HARM_LOW, F_CLAUSE_END | F_DESPAIR},
    {"what is the point", HU_SELF_HARM_LOW, F_CLAUSE_END | F_DESPAIR},
    {"can not do this anymore", HU_SELF_HARM_LOW, 0},
    {"can not go on", HU_SELF_HARM_LOW, F_CLAUSE_END},
    {"can not take it anymore", HU_SELF_HARM_LOW, 0},
    {"can not take this anymore", HU_SELF_HARM_LOW, 0},
};
#define N_PHRASES (sizeof(PHRASES) / sizeof(PHRASES[0]))

/* Verb forms for the reflexive rule. */
enum { V_KILL, V_UNALIVE, V_HARM, V_HURT, V_CUT };
typedef struct {
    const char *form;
    int family;
    bool progressive; /* "-ing": habitual, "i've been cutting myself" */
} verb_form_t;
static const verb_form_t VERBS[] = {
    {"kill", V_KILL, false},        {"kills", V_KILL, false},       {"killed", V_KILL, false},
    {"killing", V_KILL, true},      {"unalive", V_UNALIVE, false},  {"unalives", V_UNALIVE, false},
    {"unalived", V_UNALIVE, false}, {"unaliving", V_UNALIVE, true}, {"harm", V_HARM, false},
    {"harms", V_HARM, false},       {"harmed", V_HARM, false},      {"harming", V_HARM, true},
    {"hurt", V_HURT, false},        {"hurts", V_HURT, false},       {"hurting", V_HURT, true},
    {"cut", V_CUT, false},          {"cuts", V_CUT, false},         {"cutting", V_CUT, true},
};
#define N_VERBS (sizeof(VERBS) / sizeof(VERBS[0]))
enum { R_SELF, R_THIRD, R_SECOND };
static const struct {
    const char *word;
    int who;
} REFLEXIVES[] = {
    {"myself", R_SELF},      {"himself", R_THIRD},  {"herself", R_THIRD},
    {"themselves", R_THIRD}, {"themself", R_THIRD}, {"yourself", R_SECOND},
};
#define N_REFLEXIVES (sizeof(REFLEXIVES) / sizeof(REFLEXIVES[0]))

/* Contractions and slang, expanded so one phrase covers each spelling. */
static const struct {
    const char *from;
    const char *a;
    const char *b;
} EXPANSIONS[] = {
    {"wanna", "want", "to"},   {"gonna", "going", "to"},  {"dont", "do", "not"},
    {"doesnt", "does", "not"}, {"didnt", "did", "not"},   {"cant", "can", "not"},
    {"cannot", "can", "not"},  {"wont", "will", "not"},   {"wouldnt", "would", "not"},
    {"isnt", "is", "not"},     {"arent", "are", "not"},   {"wasnt", "was", "not"},
    {"aint", "am", "not"},     {"havent", "have", "not"},
};
#define N_EXPANSIONS (sizeof(EXPANSIONS) / sizeof(EXPANSIONS[0]))

/* Spaced-letter evasion ("s e l f h a r m"), checked only across a run of
 * single-letter tokens so ordinary words never concatenate into a match. */
static const char *const JOINED[] = {
    "killmyself", "suicide", "selfharm", "unalive", "enditall", "wanttodie", "betteroffwithoutme",
};
#define N_JOINED (sizeof(JOINED) / sizeof(JOINED[0]))

/* ── Tokens ───────────────────────────────────────────────────────────── */

#define WIN_BYTES   4096
#define WIN_OVERLAP 256
#define MAX_TOK     1400
#define TOK_CAP     32

typedef struct {
    char w[TOK_CAP];
    size_t len;
    size_t raw_start, raw_end; /* byte span in the window */
    bool brk;                  /* a clause break follows this token */
} tok_t;

typedef struct {
    tok_t t[MAX_TOK];
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
static bool is_word_byte(unsigned char c) {
    return is_alpha(c) || is_digit(c) || c == '@' || c == '$';
}

/* Bytes of an apostrophe at p: ' ` U+2018 U+2019 U+2032 U+02BC U+FF07. */
static size_t apostrophe_len(const unsigned char *p, size_t rem) {
    if (p[0] == '\'' || p[0] == '`')
        return 1;
    if (rem >= 2 && p[0] == 0xCA && p[1] == 0xBC)
        return 2;
    if (rem >= 3 && p[0] == 0xE2 && p[1] == 0x80 && (p[2] == 0x98 || p[2] == 0x99 || p[2] == 0xB2))
        return 3;
    if (rem >= 3 && p[0] == 0xEF && p[1] == 0xBC && p[2] == 0x87)
        return 3;
    return 0;
}

static bool is_break(unsigned char c) {
    return c == '.' || c == ',' || c == '!' || c == '?' || c == ';' || c == ':' || c == '\n' ||
           c == '\r' || c == '(' || c == ')';
}

static tok_t *push(toks_t *t, const char *w, size_t len, size_t rs, size_t re) {
    if (t->n >= MAX_TOK)
        return NULL;
    tok_t *k = &t->t[t->n++];
    if (len >= TOK_CAP)
        len = TOK_CAP - 1;
    memcpy(k->w, w, len);
    k->w[len] = '\0';
    k->len = len;
    k->raw_start = rs;
    k->raw_end = re;
    k->brk = false;
    return k;
}

static void tokenize_raw(const char *text, size_t len, toks_t *t) {
    const unsigned char *s = (const unsigned char *)text;
    size_t i = 0;
    t->n = 0;
    while (i < len) {
        if (!is_word_byte(s[i])) {
            if (is_break(s[i]) && t->n > 0)
                t->t[t->n - 1].brk = true;
            i++;
            continue;
        }
        size_t start = i, j = i;
        bool letters = false, leet = false;
        while (j < len) {
            size_t ap = apostrophe_len(s + j, len - j);
            if (ap > 0 && j > start && j + ap < len && is_alpha(s[j + ap])) {
                j += ap;
                continue;
            }
            if (!is_word_byte(s[j]))
                break;
            letters |= is_alpha(s[j]);
            leet |= is_leet(s[j]);
            j++;
        }
        char w[TOK_CAP];
        size_t wl = 0;
        for (size_t k = start; k < j && wl + 1 < sizeof(w); k++) {
            size_t ap = apostrophe_len(s + k, j - k);
            if (ap > 0) {
                k += ap - 1;
                continue;
            }
            unsigned char ch = s[k];
            w[wl++] = is_alpha(ch)        ? (char)(ch | 0x20)
                      : (letters && leet) ? leet_map(ch)
                                          : (char)ch;
        }
        if (!push(t, w, wl, start, j))
            return;
        i = j;
    }
}

static bool word_is(const tok_t *k, const char *w) {
    size_t n = strlen(w);
    return k->len == n && memcmp(k->w, w, n) == 0;
}

static bool word_in(const tok_t *k, const char *const *list) {
    for (size_t i = 0; list[i]; i++)
        if (word_is(k, list[i]))
            return true;
    return false;
}

static int verb_index(const char *w, size_t n) {
    for (size_t i = 0; i < N_VERBS; i++)
        if (strlen(VERBS[i].form) == n && memcmp(VERBS[i].form, w, n) == 0)
            return (int)i;
    return -1;
}

/* Normalise: join "my self", split "killmyself", expand contractions. */
static void normalise(const toks_t *in, toks_t *out) {
    out->n = 0;
    for (size_t k = 0; k < in->n; k++) {
        const tok_t *a = &in->t[k];
        tok_t *pushed = NULL;
        if (k + 1 < in->n && !a->brk && word_is(&in->t[k + 1], "self") &&
            (word_is(a, "my") || word_is(a, "him") || word_is(a, "her") || word_is(a, "your"))) {
            char w[TOK_CAP];
            int n = snprintf(w, sizeof(w), "%sself", a->w);
            pushed = push(out, w, n > 0 ? (size_t)n : 0, a->raw_start, in->t[k + 1].raw_end);
            if (pushed)
                pushed->brk = in->t[k + 1].brk;
            k++;
            continue;
        }
        for (size_t r = 0; r < N_REFLEXIVES && !pushed; r++) {
            size_t rl = strlen(REFLEXIVES[r].word);
            if (a->len > rl && memcmp(a->w + a->len - rl, REFLEXIVES[r].word, rl) == 0 &&
                verb_index(a->w, a->len - rl) >= 0) {
                (void)push(out, a->w, a->len - rl, a->raw_start, a->raw_end);
                pushed = push(out, REFLEXIVES[r].word, rl, a->raw_start, a->raw_end);
            }
        }
        for (size_t e = 0; e < N_EXPANSIONS && !pushed; e++) {
            if (word_is(a, EXPANSIONS[e].from)) {
                (void)push(out, EXPANSIONS[e].a, strlen(EXPANSIONS[e].a), a->raw_start, a->raw_end);
                pushed =
                    push(out, EXPANSIONS[e].b, strlen(EXPANSIONS[e].b), a->raw_start, a->raw_end);
            }
        }
        if (!pushed)
            pushed = push(out, a->w, a->len, a->raw_start, a->raw_end);
        if (pushed)
            pushed->brk = a->brk;
    }
}

/* ── Look-back ────────────────────────────────────────────────────────── */

typedef enum { SUBJ_NONE, SUBJ_FIRST, SUBJ_SECOND, SUBJ_THIRD, SUBJ_NEGATED } subject_t;

typedef struct {
    subject_t subject;
    bool intent; /* "want to", "going to", … between subject and phrase */
    bool almost; /* "almost killed myself": an accident */
} lookback_t;

static lookback_t look_back(const toks_t *t, size_t k) {
    lookback_t r = {SUBJ_NONE, false, false};
    for (size_t back = 0; back < 8 && k > back; back++) {
        const tok_t *w = &t->t[k - 1 - back];
        if (w->brk)
            break; /* the previous clause ended here */
        if (word_in(w, NEGATIONS)) {
            r.subject = SUBJ_NEGATED;
            return r;
        }
        if (word_in(w, SECOND_PERSON)) {
            r.subject = SUBJ_SECOND;
            return r;
        }
        if (word_in(w, THIRD_PERSON)) {
            r.subject = SUBJ_THIRD;
            return r;
        }
        if (word_in(w, FIRST_PERSON)) {
            r.subject = SUBJ_FIRST;
            return r;
        }
        if (word_in(w, INTENT))
            r.intent = true;
        if (word_is(w, "almost") || word_is(w, "nearly"))
            r.almost = true;
        if (!word_in(w, FILLER))
            break;
    }
    return r;
}

/* Does the phrase at k (n tokens) end its clause? */
static bool ends_clause(const toks_t *t, size_t k, size_t n) {
    size_t next = k + n;
    return next >= t->n || t->t[next - 1].brk || word_in(&t->t[next], TAIL);
}

/* "what's the point of even trying" / "… of this?" (not "… of this meeting"). */
static bool despair_object_follows(const toks_t *t, size_t k, size_t n) {
    size_t p = k + n;
    if (p + 1 >= t->n || t->t[p - 1].brk)
        return false;
    if (!word_is(&t->t[p], "of") && !word_is(&t->t[p], "in"))
        return false;
    const tok_t *obj = &t->t[p + 1];
    if (!word_in(obj, DESPAIR_OBJ))
        return false;
    if (word_is(obj, "this") || word_is(obj, "it"))
        return ends_clause(t, p + 1, 1);
    return true;
}

static bool tok_numeric(const tok_t *k) {
    if (k->len == 0)
        return false;
    for (size_t i = 0; i < k->len; i++)
        if (!is_digit((unsigned char)k->w[i]))
            return false;
    return true;
}

/* Does phrase p match the tokens starting at k? Returns its word count, or 0. */
static size_t phrase_match(const toks_t *t, size_t k, const char *p) {
    size_t n = 0;
    while (*p) {
        const char *sp = strchr(p, ' ');
        size_t wlen = sp ? (size_t)(sp - p) : strlen(p);
        if (k + n >= t->n)
            return 0;
        const tok_t *w = &t->t[k + n];
        if (w->len != wlen || memcmp(w->w, p, wlen) != 0)
            return 0;
        if (sp && w->brk)
            return 0; /* a phrase never spans a clause break */
        n++;
        p += wlen;
        if (*p == ' ')
            p++;
    }
    return n;
}

/* ── Scan ─────────────────────────────────────────────────────────────── */

typedef void (*on_mask_fn)(void *ud, size_t raw_start, size_t raw_end);

typedef struct {
    bool explicit_hit, low_hit, third_hit;
} hits_t;

static void record(hits_t *h, hu_self_harm_tier_t tier) {
    if (tier == HU_SELF_HARM_EXPLICIT)
        h->explicit_hit = true;
    else if (tier == HU_SELF_HARM_LOW)
        h->low_hit = true;
    else if (tier == HU_SELF_HARM_THIRD_PERSON)
        h->third_hit = true;
}

/* The tier of "<verb> myself" for the speaker. */
static hu_self_harm_tier_t reflexive_self_tier(const toks_t *t, size_t k, const verb_form_t *v,
                                               const lookback_t *lb) {
    const tok_t *after = k + 2 < t->n && !t->t[k + 1].brk ? &t->t[k + 2] : NULL;
    switch (v->family) {
    case V_KILL:
    case V_UNALIVE:
    case V_HARM:
        return lb->almost ? HU_SELF_HARM_LOW : HU_SELF_HARM_EXPLICIT;
    case V_CUT:
        return (!v->progressive && !lb->intent && after && word_in(after, ACCIDENT))
                   ? HU_SELF_HARM_LOW
                   : HU_SELF_HARM_EXPLICIT;
    default: /* hurt: an accident unless intent, habit or "again" */
        return (v->progressive || lb->intent || (after && word_is(after, "again")))
                   ? HU_SELF_HARM_EXPLICIT
                   : HU_SELF_HARM_LOW;
    }
}

/* Verb + reflexive, any tense: one rule instead of a phrase per form. */
static void scan_reflexives(const toks_t *t, bool *covered, hits_t *h, on_mask_fn mask, void *ud) {
    for (size_t k = 0; k + 1 < t->n; k++) {
        if (covered[k] || covered[k + 1] || t->t[k].brk)
            continue;
        int vi = verb_index(t->t[k].w, t->t[k].len);
        int who = -1;
        for (size_t r = 0; vi >= 0 && r < N_REFLEXIVES; r++)
            if (word_is(&t->t[k + 1], REFLEXIVES[r].word))
                who = REFLEXIVES[r].who;
        if (who < 0)
            continue;
        covered[k] = covered[k + 1] = true;
        const verb_form_t *v = &VERBS[vi];
        lookback_t lb = look_back(t, k);
        bool killish = v->family == V_KILL || v->family == V_UNALIVE;
        /* Quoting the contact ("you said you wanted to kill yourself") is not
         * violence; "go kill yourself" stays with the violence check. */
        bool quoted = who == R_SECOND && lb.subject == SUBJ_SECOND;
        if (mask && ((killish && who != R_SECOND) || quoted))
            mask(ud, t->t[k].raw_start, t->t[k + 1].raw_end);
        if (who == R_SECOND || lb.subject == SUBJ_NEGATED)
            continue;
        record(h, who == R_THIRD ? HU_SELF_HARM_THIRD_PERSON : reflexive_self_tier(t, k, v, &lb));
    }
}

static bool kms_is_distance(const toks_t *t, size_t k) {
    if (k > 0 && !t->t[k - 1].brk && (tok_numeric(&t->t[k - 1]) || word_in(&t->t[k - 1], QUANTITY)))
        return true;
    return k + 1 < t->n && !t->t[k].brk && word_in(&t->t[k + 1], KMS_AFTER);
}

/* Can phrase ph stand at k (n tokens), given its context flags? */
static bool phrase_applies(const toks_t *t, const phrase_t *ph, size_t k, size_t n,
                           const lookback_t *lb) {
    if ((ph->flags & F_NO_NUMBER) && kms_is_distance(t, k))
        return false;
    if ((ph->flags & F_CLAUSE_END) && !ends_clause(t, k, n) &&
        !((ph->flags & F_DESPAIR) && despair_object_follows(t, k, n)))
        return false;
    if ((ph->flags & F_NEEDS_INTENT) &&
        (!lb->intent || lb->subject == SUBJ_THIRD || lb->subject == SUBJ_SECOND))
        return false;
    return true;
}

/* The tier a matched phrase contributes, after the subject look-back. */
static hu_self_harm_tier_t phrase_tier(const phrase_t *ph, const lookback_t *lb) {
    if (ph->tier == HU_SELF_HARM_NONE || lb->subject == SUBJ_NEGATED)
        return HU_SELF_HARM_NONE;
    if (ph->flags & F_THIRD)
        return HU_SELF_HARM_THIRD_PERSON;
    if (ph->flags & F_SELF)
        return ph->tier;
    if (lb->subject == SUBJ_SECOND)
        return HU_SELF_HARM_NONE; /* quoting the contact */
    if (lb->subject == SUBJ_THIRD)
        return HU_SELF_HARM_THIRD_PERSON;
    if ((ph->flags & F_PERSONAL) && lb->subject != SUBJ_FIRST)
        return HU_SELF_HARM_LOW;
    return ph->tier;
}

static void scan_phrases(const toks_t *t, bool *covered, hits_t *h, on_mask_fn mask, void *ud) {
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
            lookback_t lb = look_back(t, k);
            if (!phrase_applies(t, ph, k, n, &lb))
                continue;
            for (size_t j = k; j < k + n; j++)
                covered[j] = true;
            if ((ph->flags & F_KILL) && mask)
                mask(ud, t->t[k].raw_start, t->t[k + n - 1].raw_end);
            record(h, phrase_tier(ph, &lb));
        }
    }
}

static bool spaced_evasion(const toks_t *t) {
    size_t run = 0, best = 0;
    for (size_t k = 0; k < t->n; k++) {
        run = (t->t[k].len == 1) ? run + 1 : 0;
        if (run > best)
            best = run;
    }
    if (best < 4)
        return false;
    char joined[WIN_BYTES];
    size_t jn = 0;
    for (size_t k = 0; k < t->n && jn + t->t[k].len < sizeof(joined); k++) {
        memcpy(joined + jn, t->t[k].w, t->t[k].len);
        jn += t->t[k].len;
    }
    joined[jn] = '\0';
    for (size_t i = 0; i < N_JOINED; i++)
        if (strstr(joined, JOINED[i]))
            return true;
    return false;
}

static hu_self_harm_tier_t scan_window(const char *text, size_t len, on_mask_fn mask, void *ud) {
    toks_t *raw = (toks_t *)malloc(sizeof(toks_t));
    toks_t *t = (toks_t *)malloc(sizeof(toks_t));
    bool *covered = (bool *)calloc(MAX_TOK, sizeof(bool));
    hu_self_harm_tier_t out = HU_SELF_HARM_NONE;
    if (raw && t && covered) {
        tokenize_raw(text, len, raw);
        normalise(raw, t);
        hits_t h = {false, false, false};
        scan_reflexives(t, covered, &h, mask, ud);
        scan_phrases(t, covered, &h, mask, ud);
        if (!h.explicit_hit && spaced_evasion(t))
            h.explicit_hit = true;
        out = h.explicit_hit ? HU_SELF_HARM_EXPLICIT
              : h.low_hit    ? HU_SELF_HARM_LOW
              : h.third_hit  ? HU_SELF_HARM_THIRD_PERSON
                             : HU_SELF_HARM_NONE;
    }
    free(raw);
    free(t);
    free(covered);
    return out;
}

/* Walk the text in overlapping 4 KB windows so a phrase at the end of a long
 * batch is still read; the highest tier wins. *win_off tracks the window for
 * the mask callback. */
static hu_self_harm_tier_t scan(const char *text, size_t len, on_mask_fn mask, void *ud,
                                size_t *win_off) {
    hu_self_harm_tier_t best = HU_SELF_HARM_NONE;
    size_t off = 0;
    for (;;) {
        size_t n = len - off < WIN_BYTES ? len - off : WIN_BYTES;
        if (win_off)
            *win_off = off;
        hu_self_harm_tier_t w = scan_window(text + off, n, mask, ud);
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
    return scan(text, len, NULL, NULL, NULL);
}

/* ── Kill-phrase mask (for the violence check) ────────────────────────── */

typedef struct {
    char *out;
    size_t out_len;
    size_t win_off;
} mask_ud_t;

static void mask_cb(void *ud, size_t rs, size_t re) {
    mask_ud_t *m = (mask_ud_t *)ud;
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
    if (n == 0)
        return 0;
    mask_ud_t m = {.out = out, .out_len = n, .win_off = 0};
    (void)scan(text, n, mask_cb, &m, &m.win_off);
    return n;
}

/* ── Names, directives, resources, gate ───────────────────────────────── */

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

static const char RESOURCE_LINE[] = "If you're in crisis, please reach out: "
                                    "988 Suicide & Crisis Lifeline (call/text 988), "
                                    "Crisis Text Line (text HOME to 741741)";

const char *hu_self_harm_resource_line(size_t *len_out) {
    if (len_out)
        *len_out = sizeof(RESOURCE_LINE) - 1;
    return RESOURCE_LINE;
}

static bool has_988(const char *s, size_t n) {
    for (size_t i = 0; s && i + 3 <= n; i++)
        if (s[i] == '9' && s[i + 1] == '8' && s[i + 2] == '8')
            return true;
    return false;
}

bool hu_self_harm_reply_needs_resources(const char *inbound, size_t inbound_len, const char *reply,
                                        size_t reply_len) {
    if (!inbound || inbound_len == 0)
        return false;
    if (has_988(reply, reply ? reply_len : 0))
        return false;
    return hu_self_harm_classify(inbound, inbound_len) == HU_SELF_HARM_EXPLICIT;
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
