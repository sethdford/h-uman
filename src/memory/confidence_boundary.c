/* confidence_boundary.c — the pure half of the confidence boundary: the gate,
 * the write-time share-level rules, the exclusion predicate, the backstop
 * ledger and sentence matcher. Contract: include/human/memory/confidence_boundary.h.
 * The per-path filters are in confidence_filters.c; the SQLite columns in
 * repos/confidence_repo_sqlite.c. */
#include "human/memory/confidence_boundary.h"

#include "human/core/log.h"
#include "human/core/string.h"
#include "human/memory/fact_extract.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static int s_mode_override = -1;

hu_gate_mode_t hu_confidence_mode(void) {
    if (s_mode_override >= 0)
        return (hu_gate_mode_t)s_mode_override;
    return hu_gate_mode_from_env("HU_CONFIDENCE_BOUNDARY", HU_GATE_OFF);
}

void hu_confidence_set_mode_for_test(int mode) {
    s_mode_override = mode;
}

const char *hu_cb_path_name(hu_cb_path_t path) {
    static const char *const names[HU_CB_PATH_COUNT] = {"semantic", "episodic", "pm_facts",
                                                        "commitments", "outbound"};
    return (unsigned)path < HU_CB_PATH_COUNT ? names[path] : "unknown";
}

/* ── Write-time derivation ─────────────────────────────────────────────── */

static void copy_contact(char *out, size_t cap, const char *src, size_t len) {
    if (!out || cap == 0)
        return;
    if (!src)
        len = 0;
    if (len >= cap)
        len = cap - 1;
    /* src re-tested here: after inlining a NULL-src call GCC's fortified
     * memcpy can't see the len=0 above and fails -Werror=nonnull. */
    if (src && len)
        memcpy(out, src, len);
    out[len] = '\0';
}

static bool has_prefix(const char *s, size_t len, const char *prefix) {
    size_t n = strlen(prefix);
    return s && len >= n && memcmp(s, prefix, n) == 0;
}

/* "<prefix><contact>:" or, with `to_end`, "<prefix><contact>" -> the contact. */
static bool key_contact(const char *key, size_t key_len, const char *prefix, bool to_end,
                        const char **c, size_t *c_len) {
    if (!has_prefix(key, key_len, prefix))
        return false;
    size_t start = strlen(prefix);
    size_t end = start;
    while (end < key_len && key[end] != ':')
        end++;
    if (end == start || (!to_end && end == key_len))
        return false;
    *c = key + start;
    *c_len = end - start;
    return true;
}

bool hu_confidence_is_owner_channel(const char *channel, size_t channel_len) {
    static const char *const owner[] = {"cli",        "stdin",      "human",
                                        "tool:human", "self_email", "calendar_self"};
    if (!channel || channel_len == 0)
        return false;
    for (size_t i = 0; i < sizeof(owner) / sizeof(owner[0]); i++) {
        if (strlen(owner[i]) == channel_len && memcmp(owner[i], channel, channel_len) == 0)
            return true;
    }
    return false;
}

hu_share_level_t hu_confidence_derive_row(const char *key, size_t key_len, const char *session,
                                          size_t session_len, const char *source, size_t source_len,
                                          const char *write_contact, size_t write_contact_len,
                                          char *out_contact, size_t out_cap) {
    copy_contact(out_contact, out_cap, NULL, 0);
    if (has_prefix(key, key_len, "_pref:"))
        return HU_SHARE_OWNER_SELF;
    if (session && session_len > 0) {
        copy_contact(out_contact, out_cap, session, session_len);
        return HU_SHARE_PRIVATE_TO_SOURCE;
    }
    const char *c = NULL;
    size_t c_len = 0;
    if (key_contact(key, key_len, "agent-promise:", false, &c, &c_len) ||
        key_contact(key, key_len, "contact:", false, &c, &c_len) ||
        (key_contact(key, key_len, "_ep:", true, &c, &c_len) &&
         !(c_len == 6 && memcmp(c, "global", 6) == 0))) {
        copy_contact(out_contact, out_cap, c, c_len);
        return HU_SHARE_PRIVATE_TO_SOURCE;
    }
    if (write_contact && write_contact_len > 0) {
        copy_contact(out_contact, out_cap, write_contact, write_contact_len);
        return HU_SHARE_PRIVATE_TO_SOURCE;
    }
    if (hu_confidence_is_owner_channel(source, source_len))
        return HU_SHARE_OWNER_SELF;
    return HU_SHARE_PRIVATE_TO_SOURCE; /* unknown source: conservative */
}

hu_share_level_t hu_confidence_derive_fact(const hu_heuristic_fact_t *f, char *out_contact,
                                           size_t out_cap) {
    copy_contact(out_contact, out_cap, NULL, 0);
    if (!f)
        return HU_SHARE_PRIVATE_TO_SOURCE;
    const char *h =
        f->provenance.contact_handle[0] ? f->provenance.contact_handle : f->contact_handle;
    size_t h_len = strnlen(h, HU_PROV_HANDLE_MAX);
    if (h_len > 0) {
        copy_contact(out_contact, out_cap, h, h_len);
        return HU_SHARE_PRIVATE_TO_SOURCE;
    }
    const char *ch = f->provenance.channel;
    if (hu_confidence_is_owner_channel(ch, strnlen(ch, HU_PROV_CHANNEL_MAX)))
        return HU_SHARE_OWNER_SELF;
    return HU_SHARE_PRIVATE_TO_SOURCE;
}

bool hu_confidence_excludes(hu_share_level_t level, const char *source, size_t source_len,
                            const char *current, size_t current_len) {
    if (!current || current_len == 0)
        return false;
    if (level == HU_SHARE_SHAREABLE || level == HU_SHARE_OWNER_SELF)
        return false;
    if (!source || source_len == 0)
        return true;
    return !(source_len == current_len && memcmp(source, current, current_len) == 0);
}

static hu_confidence_owner_fn s_owner_fn;
static const void *s_owner_ctx;

void hu_confidence_set_owner_resolver(hu_confidence_owner_fn fn, const void *ctx) {
    s_owner_fn = fn;
    s_owner_ctx = fn ? ctx : NULL;
}

bool hu_confidence_is_owner_contact(const char *contact, size_t contact_len) {
    return s_owner_fn && contact && contact_len > 0 &&
           s_owner_fn(s_owner_ctx, contact, contact_len);
}

/* ── Backstop ledger ───────────────────────────────────────────────────────
 * The daemon runs one reply turn at a time, and the filters and the backstop
 * of a turn run on that thread, so a process-wide ledger keyed by contact is
 * enough. Noting for another contact starts a fresh ledger. */

#define CB_LEDGER_MAX 32
#define CB_ITEM_MAX   512
#define CB_TOKEN_MAX  48
#define CB_TOKENS_MAX 24

static struct {
    char contact[HU_CB_CONTACT_MAX];
    char items[CB_LEDGER_MAX][CB_ITEM_MAX];
    size_t count;
    size_t next; /* ring slot once full */
} s_ledger;

void hu_confidence_ledger_clear(void) {
    s_ledger.contact[0] = '\0';
    s_ledger.count = 0;
    s_ledger.next = 0;
}

size_t hu_confidence_ledger_count(void) {
    return s_ledger.count;
}

void hu_confidence_ledger_note(const char *contact, size_t contact_len, const char *text,
                               size_t text_len) {
    if (!contact || contact_len == 0 || contact_len >= HU_CB_CONTACT_MAX || !text || !text_len)
        return;
    if (strlen(s_ledger.contact) != contact_len ||
        memcmp(s_ledger.contact, contact, contact_len) != 0) {
        hu_confidence_ledger_clear();
        memcpy(s_ledger.contact, contact, contact_len);
        s_ledger.contact[contact_len] = '\0';
    }
    size_t slot;
    if (s_ledger.count < CB_LEDGER_MAX) {
        slot = s_ledger.count++;
    } else { /* full: overwrite the oldest */
        slot = s_ledger.next;
        s_ledger.next = (s_ledger.next + 1) % CB_LEDGER_MAX;
    }
    copy_contact(s_ledger.items[slot], CB_ITEM_MAX, text, text_len);
}

/* Capitalized words that are not names: sentence openers, pronouns, labels
 * the stores write ("Task:", "Outcome:"), days and months. */
static bool is_name_stopword(const char *w) {
    static const char *const stop[] = {
        "The",     "This",     "That",   "These", "Those",    "There",   "Then",      "They",
        "Them",    "Their",    "She",    "Her",   "His",      "Him",     "And",       "But",
        "For",     "Not",      "Just",   "What",  "When",     "Where",   "Why",       "How",
        "Who",     "Yes",      "Yeah",   "Okay",  "Also",     "Can",     "Could",     "Would",
        "Should",  "Will",     "Did",    "Does",  "Have",     "Has",     "Had",       "Was",
        "Were",    "Are",      "You",    "Your",  "Our",      "Its",     "Task",      "Actions",
        "Outcome", "Score",    "Memory", "Seth",  "Monday",   "Tuesday", "Wednesday", "Thursday",
        "Friday",  "Saturday", "Sunday", "Today", "Tomorrow", "Tonight", "Lol",       "Haha",
        "Omg",     "Hey",      "Thanks", "Sorry", "Please",   "Maybe",   "Well",      "Still",
        "Some",    "Any",      "All",    "With",  "From",     "About",   "After",
    };
    for (size_t i = 0; i < sizeof(stop) / sizeof(stop[0]); i++)
        if (strcmp(w, stop[i]) == 0)
            return true;
    return false;
}

static bool is_content_stopword(const char *w) {
    static const char *const stop[] = {
        "that",  "this",  "with",   "have",    "just",    "about",   "what",       "when",
        "they",  "them",  "been",   "from",    "your",    "will",    "would",      "could",
        "there", "their", "really", "like",    "know",    "yeah",    "haha",       "okay",
        "also",  "some",  "much",   "very",    "well",    "good",    "think",      "want",
        "going", "gonna", "task",   "actions", "outcome", "score",   "agent_turn", "silence_intuit",
        "dont",  "didnt", "cant",   "said",    "told",    "tell",    "says",       "still",
        "were",  "here",  "then",   "than",    "into",    "over",    "back",       "only",
        "even",  "make",  "made",   "time",    "today",   "tonight",
    };
    for (size_t i = 0; i < sizeof(stop) / sizeof(stop[0]); i++)
        if (strcmp(w, stop[i]) == 0)
            return true;
    return false;
}

typedef struct {
    char names[CB_TOKENS_MAX][CB_TOKEN_MAX];
    size_t name_count;
    char words[CB_TOKENS_MAX][CB_TOKEN_MAX];
    size_t word_count;
} cb_item_tokens_t;

static bool token_seen(char list[][CB_TOKEN_MAX], size_t n, const char *w) {
    for (size_t i = 0; i < n; i++)
        if (strcasecmp(list[i], w) == 0)
            return true;
    return false;
}

static void tokenize_item(const char *item, cb_item_tokens_t *t) {
    memset(t, 0, sizeof(*t));
    size_t i = 0, len = strlen(item);
    while (i < len) {
        while (i < len && !isalpha((unsigned char)item[i]))
            i++;
        size_t s = i;
        while (i < len && (isalpha((unsigned char)item[i]) || item[i] == '_'))
            i++;
        size_t wl = i - s;
        if (wl < 3 || wl >= CB_TOKEN_MAX)
            continue;
        char w[CB_TOKEN_MAX];
        memcpy(w, item + s, wl);
        w[wl] = '\0';
        bool cap = isupper((unsigned char)w[0]) && islower((unsigned char)w[1]);
        if (cap && !is_name_stopword(w) && t->name_count < CB_TOKENS_MAX &&
            !token_seen(t->names, t->name_count, w))
            memcpy(t->names[t->name_count++], w, wl + 1);
        if (wl >= 4) {
            for (size_t k = 0; k < wl; k++)
                w[k] = (char)tolower((unsigned char)w[k]);
            if (!is_content_stopword(w) && t->word_count < CB_TOKENS_MAX &&
                !token_seen(t->words, t->word_count, w))
                memcpy(t->words[t->word_count++], w, wl + 1);
        }
    }
}

static bool sentence_matches(const char *s, size_t len, const cb_item_tokens_t *t) {
    for (size_t n = 0; n < t->name_count; n++) {
        if (!hu_str_contains_word_ci_n(s, len, t->names[n]))
            continue;
        for (size_t w = 0; w < t->word_count; w++) {
            if (strcasecmp(t->words[w], t->names[n]) == 0)
                continue;
            if (hu_str_contains_word_ci_n(s, len, t->words[w]))
                return true;
        }
    }
    return false;
}

size_t hu_confidence_backstop_scan(const char *draft, size_t draft_len, char *out,
                                   size_t *out_len) {
    size_t w = 0, matched = 0;
    if (out_len)
        *out_len = 0;
    if (!draft || draft_len == 0) {
        if (out)
            out[0] = '\0';
        return 0;
    }
    static cb_item_tokens_t toks[CB_LEDGER_MAX];
    size_t n_items = s_ledger.count;
    for (size_t i = 0; i < n_items; i++)
        tokenize_item(s_ledger.items[i], &toks[i]);
    size_t i = 0;
    while (i < draft_len) {
        size_t s = i;
        while (i < draft_len && draft[i] != '.' && draft[i] != '!' && draft[i] != '?' &&
               draft[i] != '\n')
            i++;
        while (i < draft_len && (draft[i] == '.' || draft[i] == '!' || draft[i] == '?' ||
                                 draft[i] == '\n' || draft[i] == ' '))
            i++;
        bool hit = false;
        for (size_t k = 0; k < n_items && !hit; k++)
            hit = sentence_matches(draft + s, i - s, &toks[k]);
        if (hit) {
            matched++;
            continue;
        }
        if (out) {
            size_t from = s;
            if (w == 0)
                while (from < i && draft[from] == ' ')
                    from++;
            memcpy(out + w, draft + from, i - from);
            w += i - from;
        }
    }
    if (out) {
        while (w > 0 && (out[w - 1] == ' ' || out[w - 1] == '\n'))
            w--;
        out[w] = '\0';
    }
    if (out_len)
        *out_len = w;
    return matched;
}

size_t hu_confidence_backstop_apply(hu_allocator_t *alloc, const char *contact, size_t contact_len,
                                    char **response, size_t *response_len) {
    hu_gate_mode_t mode = hu_confidence_mode();
    if (mode == HU_GATE_OFF)
        return 0;
    size_t matched = 0;
    bool ours = contact && contact_len > 0 && strlen(s_ledger.contact) == contact_len &&
                memcmp(s_ledger.contact, contact, contact_len) == 0;
    if (ours && s_ledger.count > 0 && alloc && response && *response && response_len &&
        *response_len > 0) {
        size_t len = *response_len;
        char *scratch = (char *)alloc->alloc(alloc->ctx, len + 1);
        if (scratch) {
            size_t out_len = 0;
            matched = hu_confidence_backstop_scan(*response, len, scratch, &out_len);
            if (mode == HU_GATE_LIVE && matched > 0) {
                /* A fresh buffer of exactly out_len + 1: callers free len + 1. */
                char *fresh = (char *)alloc->alloc(alloc->ctx, out_len + 1);
                if (fresh) {
                    memcpy(fresh, scratch, out_len + 1);
                    alloc->free(alloc->ctx, *response, len + 1);
                    *response = fresh;
                    *response_len = out_len;
                }
            }
            alloc->free(alloc->ctx, scratch, len + 1);
        }
        hu_log_info("confidence-boundary", NULL,
                    "[confidence-boundary %s] path=outbound ledger=%zu sentences_%s=%zu",
                    mode == HU_GATE_LIVE ? "live" : "shadow", s_ledger.count,
                    mode == HU_GATE_LIVE ? "dropped" : "would_drop", matched);
    }
    hu_confidence_ledger_clear();
    return matched;
}
