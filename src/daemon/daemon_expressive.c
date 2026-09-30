#include "human/core/gate_mode.h"
#include "human/core/log.h"
#include "human/core/string.h"
#include "human/daemon/expressive.h"
#include "human/daemon/share_queue.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* snprintf result -> length written, or 0 (and an empty buffer) if cut off. */
static size_t fitted(char *buf, size_t cap, int n) {
    if (n < 0 || (size_t)n >= cap) {
        buf[0] = '\0';
        return 0;
    }
    return (size_t)n;
}

bool hu_expressive_somber(const char *text, size_t len) {
    static const char *const w[] = {
        "passed away", "died",    "funeral",  "hospital",    "cancer",   "chemo",
        "diagnosed",   "divorce", "broke up", "miscarriage", "laid off", "fired",
        "accident",    "surgery", "grief",    "grieving",    "rip",      "sick",
    };
    if (!text || len == 0)
        return false;
    for (size_t i = 0; i < sizeof(w) / sizeof(w[0]); i++)
        if (hu_str_contains_word_ci_n(text, len, w[i]))
            return true;
    return false;
}

bool hu_expressive_effect_allowed(const char *effect, bool somber, bool is_group,
                                  int64_t secs_since_last) {
    static const char *const ids[] = {"impact",       "loud",     "gentle",
                                      "invisibleink", "confetti", "lasers"};
    if (!effect || somber || is_group)
        return false;
    if (secs_since_last >= 0 && secs_since_last < 7 * 86400)
        return false;
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++)
        if (strcmp(effect, ids[i]) == 0)
            return true;
    return false;
}

bool hu_expressive_gif_allowed(bool somber, bool is_group, const char *relationship,
                               int64_t secs_since_last) {
    /* A GIF to your mother or a business contact reads as a bot, not a person. */
    static const char *const formal[] = {"mother",    "father", "professional", "business",
                                         "colleague", "boss",   "client"};
    if (somber || is_group || !relationship || !relationship[0])
        return false;
    if (secs_since_last >= 0 && secs_since_last < 86400)
        return false;
    for (size_t i = 0; i < sizeof(formal) / sizeof(formal[0]); i++)
        if (strstr(relationship, formal[i]))
            return false;
    return true;
}

size_t hu_expressive_situation(char *buf, size_t cap, bool voice_available, bool bridge_up,
                               bool is_group, bool saved_link) {
    if (!buf || cap == 0)
        return 0;
    int n = snprintf(buf, cap, "This turn: voice memo: %s; effects and threaded replies: %s%s%s.",
                     voice_available ? "available" : "not available",
                     bridge_up ? "available" : "not available", is_group ? "; group chat" : "",
                     saved_link ? "; Seth saved a link for them (share:saved)" : "");
    return fitted(buf, cap, n);
}

size_t hu_expressive_shadow_line(const hu_director_result_t *r, const char *inbound,
                                 size_t inbound_len, bool is_group, const char *relationship,
                                 char *buf, size_t cap) {
    if (!buf || cap == 0)
        return 0;
    buf[0] = '\0';
    if (!r)
        return 0;
    bool somber = hu_expressive_somber(inbound, inbound ? inbound_len : 0);
    /* Budgets are not applied here (-1): the shadow line judges the moment. */
    const char *eff_verdict =
        r->effect[0]
            ? (hu_expressive_effect_allowed(r->effect, somber, is_group, -1) ? "(ok)" : "(blocked)")
            : "";
    const char *gif_verdict =
        r->form == HU_DIR_FORM_GIF
            ? (hu_expressive_gif_allowed(somber, is_group, relationship, -1) ? "(ok)" : "(blocked)")
            : "";
    static const char *const share_names[] = {"none", "song", "video", "short", "saved"};
    const char *share_name = (unsigned)r->share < 5 ? share_names[r->share] : "none";
    const char *share_verdict =
        r->share != HU_SHARE_NONE
            ? (hu_expressive_share_allowed(somber, is_group, -1) ? "(ok)" : "(blocked)")
            : "";
    int n = snprintf(buf, cap,
                     "form=%s effect=%s%s gif=\"%s\"%s reply_to=%d somber=%d share=%s%s q=\"%s\"",
                     hu_director_form_name(r->form), r->effect[0] ? r->effect : "none", eff_verdict,
                     r->gif_query, gif_verdict, r->reply_to ? 1 : 0, somber ? 1 : 0, share_name,
                     share_verdict, r->share_query);
    return fitted(buf, cap, n);
}

bool hu_expressive_share_allowed(bool somber, bool is_group, int64_t secs_since_last) {
    return !somber && !is_group && (secs_since_last < 0 || secs_since_last >= 86400);
}

hu_inspiration_medium_t hu_expressive_share_medium(hu_share_kind_t kind, bool have_youtube_key) {
    switch (kind) {
    case HU_SHARE_SONG:
        return HU_INSPIRATION_MUSIC;
    case HU_SHARE_VIDEO:
    case HU_SHARE_SHORT:
        return have_youtube_key ? HU_INSPIRATION_YOUTUBE : HU_INSPIRATION_NONE;
    default:
        return HU_INSPIRATION_NONE; /* saved links go through the share queue */
    }
}

bool hu_expressive_share_should_go(const hu_director_result_t *director, bool forms_live,
                                   bool dice_hit, hu_share_kind_t *kind_out) {
    if (kind_out)
        *kind_out = HU_SHARE_NONE;
    if (director && director->form == HU_DIR_FORM_SHARE && director->share != HU_SHARE_NONE) {
        if (director->share == HU_SHARE_SAVED)
            return false; /* the share queue sends saved links */
        if (kind_out)
            *kind_out = director->share;
        return true;
    }
    return !forms_live && dice_hit;
}

/* Last time a flourish of one kind went to a contact, for this process (a
 * restart forgets: one extra at most). since_out = -1 when never. */
static void budget_since(char kind, const char *key, size_t key_len, int64_t now,
                         int64_t *since_out, size_t *slot_out);
static void budget_mark(char kind, const char *key, size_t key_len, int64_t now, size_t slot);

static struct {
    char kind;
    char key[64];
    size_t len;
    int64_t at;
} s_budget[96];
static pthread_mutex_t s_budget_mu = PTHREAD_MUTEX_INITIALIZER;

static void budget_since(char kind, const char *key, size_t key_len, int64_t now,
                         int64_t *since_out, size_t *slot_out) {
    size_t slot = 0;
    *since_out = -1;
    for (size_t i = 0; i < sizeof(s_budget) / sizeof(s_budget[0]); i++) {
        if (s_budget[i].kind == kind && s_budget[i].len == key_len &&
            memcmp(s_budget[i].key, key, key_len) == 0) {
            slot = i;
            *since_out = now - s_budget[i].at;
            break;
        }
        if (s_budget[i].at < s_budget[slot].at)
            slot = i; /* oldest (or empty) slot, reused for a new pair */
    }
    *slot_out = slot;
}

static void budget_mark(char kind, const char *key, size_t key_len, int64_t now, size_t slot) {
    s_budget[slot].kind = kind;
    memcpy(s_budget[slot].key, key, key_len);
    s_budget[slot].len = key_len;
    s_budget[slot].at = now;
}

const hu_director_result_t *hu_expressive_share_gate(const hu_director_result_t *d, bool valid,
                                                     bool forms_live, const char *inbound,
                                                     size_t inbound_len, bool is_group,
                                                     const char *key, size_t key_len, int64_t now) {
    if (!valid || !forms_live || !d || d->form != HU_DIR_FORM_SHARE || d->share == HU_SHARE_NONE ||
        !key || key_len == 0 || key_len >= sizeof(s_budget[0].key))
        return NULL;
    pthread_mutex_lock(&s_budget_mu);
    int64_t since;
    size_t slot;
    budget_since('s', key, key_len, now, &since, &slot);
    bool ok =
        hu_expressive_share_allowed(hu_expressive_somber(inbound, inbound_len), is_group, since);
    if (ok)
        budget_mark('s', key, key_len, now, slot);
    pthread_mutex_unlock(&s_budget_mu);
    return ok ? d : NULL;
}

/* Next space-delimited word at text[*i..len) -> w (lowercased); advances *i past
 * it and one following space. */
static size_t next_word(const char *text, size_t len, size_t *i, char *w, size_t cap) {
    size_t n = 0;
    while (*i < len && text[*i] != ' ' && text[*i] != '\n') {
        if (n + 1 < cap)
            w[n++] = (char)tolower((unsigned char)text[*i]);
        (*i)++;
    }
    w[n] = '\0';
    if (*i < len && text[*i] == ' ')
        (*i)++;
    return n;
}

static int word_index(const char *w, const char *const *list, size_t count) {
    for (size_t k = 0; k < count; k++)
        if (strcmp(w, list[k]) == 0)
            return (int)k;
    return -1;
}

bool hu_selftest_parse(const char *text, size_t len, hu_selftest_t *out) {
    static const char *const cmds[] = {"#voice", "#share", "#effect", "#tapback", "#gif", "#text"};
    static const char *const kinds[] = {"song", "video", "short", "saved"};
    static const hu_share_kind_t kind_vals[] = {HU_SHARE_SONG, HU_SHARE_VIDEO, HU_SHARE_SHORT,
                                                HU_SHARE_SAVED};
    static const char *const effects[] = {"impact",       "loud",     "gentle",
                                          "invisibleink", "confetti", "lasers"};
    static const char *const taps[] = {"love", "like", "laugh", "emphasize", "question", "dislike"};
    static const hu_reaction_type_t tap_vals[] = {HU_REACTION_HEART,    HU_REACTION_THUMBS_UP,
                                                  HU_REACTION_HAHA,     HU_REACTION_EMPHASIS,
                                                  HU_REACTION_QUESTION, HU_REACTION_THUMBS_DOWN};
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    if (!text || len == 0 || text[0] != '#')
        return false;
    size_t i = 0;
    char w[24], arg[24];
    next_word(text, len, &i, w, sizeof(w));
    switch (word_index(w, cmds, sizeof(cmds) / sizeof(cmds[0]))) {
    case 0:
        out->form = HU_DIR_FORM_VOICE;
        break;
    case 1: {
        int k = next_word(text, len, &i, arg, sizeof(arg))
                    ? word_index(arg, kinds, sizeof(kinds) / sizeof(kinds[0]))
                    : -1;
        if (k < 0)
            return false;
        out->form = HU_DIR_FORM_SHARE;
        out->share = kind_vals[k];
        snprintf(out->query, sizeof(out->query), "%.*s", (int)(len - i), text + i);
        break;
    }
    case 2:
        if (!next_word(text, len, &i, arg, sizeof(arg)) ||
            word_index(arg, effects, sizeof(effects) / sizeof(effects[0])) < 0)
            return false;
        out->form = HU_DIR_FORM_TEXT;
        snprintf(out->effect, sizeof(out->effect), "%s", arg);
        break;
    case 3: {
        int k = next_word(text, len, &i, arg, sizeof(arg))
                    ? word_index(arg, taps, sizeof(taps) / sizeof(taps[0]))
                    : -1;
        if (k < 0)
            return false;
        out->form = HU_DIR_FORM_TAPBACK;
        out->reaction = tap_vals[k];
        break;
    }
    case 4:
        out->form = HU_DIR_FORM_GIF;
        snprintf(out->query, sizeof(out->query), "%.*s", (int)(len - i), text + i);
        break;
    case 5: /* a normal text reply: typing rhythm and pacing, on demand */
        out->form = HU_DIR_FORM_TEXT;
        break;
    default:
        return false;
    }
    out->consumed = i;
    return true;
}

void hu_expressive_selftest_apply(const hu_selftest_t *t, hu_director_result_t *d) {
    if (!t || !d)
        return;
    d->form = t->form;
    d->action = t->form == HU_DIR_FORM_TAPBACK ? DIR_TAPBACK : DIR_TEXT;
    if (t->form == HU_DIR_FORM_TAPBACK)
        d->reaction = t->reaction;
    if (t->form == HU_DIR_FORM_SHARE) {
        d->share = t->share;
        snprintf(d->share_query, sizeof(d->share_query), "%s", t->query);
    }
    if (t->form == HU_DIR_FORM_GIF)
        snprintf(d->gif_query, sizeof(d->gif_query), "%s", t->query);
    if (t->effect[0])
        snprintf(d->effect, sizeof(d->effect), "%s", t->effect);
}

bool hu_expressive_effect_gate(const hu_director_result_t *d, bool valid, bool forms_live,
                               const char *inbound, size_t inbound_len, bool is_group,
                               const char *key, size_t key_len, int64_t now, char *effect_out,
                               size_t cap) {
    if (!valid || !forms_live || !d || !d->effect[0] || !key || key_len == 0 ||
        key_len >= sizeof(s_budget[0].key) || !effect_out || strlen(d->effect) >= cap)
        return false;
    pthread_mutex_lock(&s_budget_mu);
    int64_t since;
    size_t slot;
    budget_since('e', key, key_len, now, &since, &slot);
    bool ok = hu_expressive_effect_allowed(d->effect, hu_expressive_somber(inbound, inbound_len),
                                           is_group, since);
    if (ok) {
        budget_mark('e', key, key_len, now, slot);
        memcpy(effect_out, d->effect, strlen(d->effect) + 1);
    }
    pthread_mutex_unlock(&s_budget_mu);
    return ok;
}

bool hu_selftest_from_owner(const struct hu_persona *p, const char *key, size_t key_len,
                            const char *text, size_t len) {
    hu_selftest_t t;
    return p && hu_share_is_owner(p, key, key_len) && hu_selftest_parse(text, len, &t);
}

/* ── Unknown-event guard ─────────────────────────────────────────────── */

#define UE_MAX_WORDS 48
#define UE_WORD      24

static bool ue_in(const char *w, const char *const *set) {
    for (size_t i = 0; set[i]; i++)
        if (strcmp(w, set[i]) == 0)
            return true;
    return false;
}

/* Lowercased words of `s`; a sentence break (?.!,;) is the word "|". Curly
 * apostrophes become '; other non-ASCII bytes separate words. */
static size_t ue_words(const char *s, size_t len, char out[][UE_WORD], size_t cap) {
    size_t n = 0, wl = 0;
    for (size_t i = 0; i <= len && n < cap; i++) {
        unsigned char c = i < len ? (unsigned char)s[i] : ' ';
        if (c == 0xE2 && i + 2 < len && (unsigned char)s[i + 1] == 0x80 &&
            ((unsigned char)s[i + 2] == 0x98 || (unsigned char)s[i + 2] == 0x99)) {
            c = '\'';
            i += 2;
        }
        bool part = isalnum(c) || c == '\'';
        if (part && wl + 1 < UE_WORD) {
            out[n][wl++] = (char)tolower(c);
            continue;
        }
        if (part)
            continue; /* over-long word: truncate */
        if (wl > 0) {
            out[n][wl] = '\0';
            n++;
            wl = 0;
        }
        if ((c == '?' || c == '.' || c == '!' || c == ',' || c == ';') && n < cap &&
            (n == 0 || strcmp(out[n - 1], "|") != 0)) {
            memcpy(out[n], "|", 2);
            n++;
        }
    }
    return n;
}

/* Does any history entry contain `w` at the start of a word? */
static bool ue_mentioned(const char *w, const hu_channel_history_entry_t *h, size_t n) {
    size_t wl = strlen(w);
    for (size_t e = 0; e < n; e++) {
        const char *t = h[e].text;
        for (size_t i = 0; t[i]; i++) {
            if (i > 0 && isalnum((unsigned char)t[i - 1]))
                continue;
            if (strncasecmp(t + i, w, wl) == 0)
                return true;
        }
    }
    return false;
}

bool hu_expressive_unknown_event(const char *msg, size_t msg_len,
                                 const hu_channel_history_entry_t *history, size_t history_count,
                                 char *topic, size_t topic_cap) {
    static const char *const k_det[] = {"the", "that", "this", "your", "ur",    "those", "these",
                                        "a",   "an",   "his",  "her",  "their", NULL};
    static const char *const k_stop[] = {
        "|",       "go",      "going", "goes", "turn", "turned", "went",      "doing", "settling",
        "holding", "getting", "into",  "in",   "with", "last",   "yesterday", "today", "tonight",
        "earlier", "so",      "at",    "and",  "then", "for",    NULL};
    static const char *const k_vague[] = {
        "it",      "that",    "this",     "things",  "everything", "stuff", "them",
        "he",      "she",     "they",     "you",     "day",        "days",  "week",
        "weekend", "morning", "night",    "evening", "afternoon",  "life",  "work",
        "sleep",   "weather", "everyone", "all",     NULL};
    if (topic && topic_cap)
        topic[0] = '\0';
    if (!msg || msg_len == 0)
        return false;
    char w[UE_MAX_WORDS][UE_WORD];
    size_t n = ue_words(msg, msg_len, w, UE_MAX_WORDS);

    for (size_t i = 0; i < n; i++) {
        size_t p = 0;           /* first word of the event phrase */
        bool need_verb = false; /* "how's X doing": the event ends at a state verb */
        if (strcmp(w[i], "how'd") == 0)
            p = i + 1;
        else if (strcmp(w[i], "how") == 0 && i + 1 < n &&
                 (strcmp(w[i + 1], "did") == 0 || strcmp(w[i + 1], "was") == 0))
            p = i + 2;
        else if (strcmp(w[i], "how's") == 0 ||
                 (strcmp(w[i], "how") == 0 && i + 1 < n && strcmp(w[i + 1], "is") == 0)) {
            p = strcmp(w[i], "how's") == 0 ? i + 1 : i + 2;
            need_verb = true;
        } else if (strcmp(w[i], "did") == 0 && i + 1 < n && strcmp(w[i + 1], "you") == 0) {
            size_t j = i + 2;
            while (j < n && (strcmp(w[j], "ever") == 0 || strcmp(w[j], "end") == 0 ||
                             strcmp(w[j], "up") == 0 || strcmp(w[j], "actually") == 0 ||
                             strcmp(w[j], "even") == 0))
                j++;
            if (j < n && (strcmp(w[j], "go") == 0 || strcmp(w[j], "going") == 0 ||
                          strcmp(w[j], "make") == 0)) {
                j++;
                if (j < n && strcmp(w[j], "it") == 0)
                    j++;
                if (j < n && strcmp(w[j], "to") == 0)
                    p = j + 1;
            }
        }
        if (p == 0 || p >= n)
            continue;
        while (p < n && ue_in(w[p], k_det))
            p++;
        size_t e = p;
        while (e < n && e - p < 4 && !ue_in(w[e], k_stop))
            e++;
        if (e == p)
            continue;
        if (need_verb &&
            (e >= n || !(strcmp(w[e], "doing") == 0 || strcmp(w[e], "settling") == 0 ||
                         strcmp(w[e], "holding") == 0 || strcmp(w[e], "getting") == 0)))
            continue;
        /* An event needs a real word; pronouns and time periods point
         * elsewhere ("how'd it go", "how was your day"). */
        bool eventful = false;
        for (size_t k = p; k < e; k++)
            if (strlen(w[k]) >= 3 && !ue_in(w[k], k_vague))
                eventful = true;
        if (!eventful)
            continue;
        /* Known when the thread already mentions any content word of it. */
        bool known = false;
        for (size_t k = p; k < e && !known; k++)
            if (strlen(w[k]) >= 4 && !ue_in(w[k], k_vague))
                known = ue_mentioned(w[k], history, history_count);
        if (known)
            return false;
        if (topic && topic_cap) {
            size_t pos = 0;
            for (size_t k = p; k < e; k++) {
                int wr = snprintf(topic + pos, topic_cap - pos, "%s%s", k > p ? " " : "", w[k]);
                if (wr < 0 || (size_t)wr >= topic_cap - pos)
                    break;
                pos += (size_t)wr;
            }
        }
        return true;
    }
    return false;
}

void hu_expressive_unknown_event_direction(const char *topic, char *out, size_t cap) {
    if (!out || cap == 0)
        return;
    snprintf(out, cap,
             "don't say how the %s went or turned out, you don't know; ask which one they mean "
             "or say you haven't heard yet, one line",
             topic && topic[0] ? topic : "thing");
}

void hu_expressive_unknown_event_guard(hu_director_result_t *d, const char *msg, size_t msg_len,
                                       const hu_channel_history_entry_t *history,
                                       size_t history_count) {
    hu_gate_mode_t g = hu_gate_mode_from_env("HU_UNKNOWN_EVENT_GUARD", HU_GATE_OFF);
    char topic[64];
    if (!d || g == HU_GATE_OFF ||
        !hu_expressive_unknown_event(msg, msg_len, history, history_count, topic, sizeof(topic)))
        return;
    hu_log_info("director", NULL, "unknown-event %s: topic=\"%s\" direction was \"%.80s\"",
                g == HU_GATE_LIVE ? "live" : "shadow", topic, d->direction);
    if (g == HU_GATE_LIVE)
        hu_expressive_unknown_event_direction(topic, d->direction, sizeof(d->direction));
}
