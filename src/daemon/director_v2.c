/* Director v2: direct intent, tone and what to engage with, never length.
 * Gate, evidence and promotion measurement: include/human/daemon/director_v2.h
 * and docs/guides/director-v2.md. */
#include "human/daemon/director_v2.h"
#include "human/agent.h"
#include "human/config.h"
#include "human/core/gate_mode.h"
#include "human/core/log.h"
#include "human/daemon/common.h"
#include "human/daemon/director_tapback.h"
#include "human/persona.h"
#include "human/providers/compatible.h"

#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── directive flags (the brevity measurement) ─────────────────────── */

static bool alnum_at(const char *s, size_t n, size_t i) {
    return i < n && isalnum((unsigned char)s[i]);
}

/* Case-insensitive `pat` at s[i]; a word must start there. */
static bool starts_word(const char *s, size_t n, size_t i, const char *pat) {
    size_t pl = strlen(pat);
    if (i + pl > n || (i > 0 && alnum_at(s, n, i - 1)))
        return false;
    for (size_t k = 0; k < pl; k++)
        if (tolower((unsigned char)s[i + k]) != pat[k])
            return false;
    return true;
}

static const char *const k_length_cues[] = {
    "one line",    "one-line",    "oneline",   "one word", "one-word",
    "a few words", "word or two", "two words", "brief",
};
static const char *const k_deflect_cues[] = {
    "keep it light",  "non-committal", "noncommittal", "non committal",
    "over-explain",   "overexplain",   "over explain", "don't elaborate",
    "dont elaborate", "laugh it off",  "deflect",
};

/* "5 words", "10 word reply". */
static bool digits_then_word(const char *s, size_t n, size_t i) {
    if (!isdigit((unsigned char)s[i]) || (i > 0 && alnum_at(s, n, i - 1)))
        return false;
    size_t j = i;
    while (j < n && isdigit((unsigned char)s[j]))
        j++;
    return j < n && s[j] == ' ' && starts_word(s, n, j + 1, "word");
}

unsigned hu_director_directive_flags(const char *dir, size_t len) {
    unsigned f = 0;
    if (!dir)
        return 0;
    for (size_t i = 0; i < len && f != (HU_DIRECTIVE_LENGTH | HU_DIRECTIVE_DEFLECT); i++) {
        for (size_t c = 0; c < sizeof(k_length_cues) / sizeof(k_length_cues[0]); c++)
            if (starts_word(dir, len, i, k_length_cues[c]))
                f |= HU_DIRECTIVE_LENGTH;
        /* "short" as a whole word: not "shortly", and not the share kind in
         * a format spec ("share:<song|video|short|saved>"). */
        if (starts_word(dir, len, i, "short") && !alnum_at(dir, len, i + 5) &&
            !(i > 0 && dir[i - 1] && strchr("|:<", dir[i - 1])))
            f |= HU_DIRECTIVE_LENGTH;
        if (digits_then_word(dir, len, i))
            f |= HU_DIRECTIVE_LENGTH;
        for (size_t c = 0; c < sizeof(k_deflect_cues) / sizeof(k_deflect_cues[0]); c++)
            if (starts_word(dir, len, i, k_deflect_cues[c]))
                f |= HU_DIRECTIVE_DEFLECT;
    }
    return f;
}

bool hu_director_brevity_directive(const char *dir, size_t len) {
    return hu_director_directive_flags(dir, len) != 0;
}

/* ── prompts ───────────────────────────────────────────────────────── */

/* A schema and principles, no examples and no fixed rules: delay, silence
 * and tapbacks are judged from the thread and from Seth's measured behaviour
 * (the "How Seth replies" line), never from ranges or word lists. */
static const char k_v2_system[] =
    "You direct one beat of a text conversation. The actor plays Seth, 45, a tech "
    "entrepreneur who lives alone with his cat; his kids don't live with him. Decide what Seth "
    "does with their message: its intent and its tone. How long the reply is gets decided "
    "elsewhere, so never mention length: no line, word or sentence counts.\n\n"
    "Answer on a single line, fields separated by '|', direction last:\n"
    "action:<text|tapback|silence> - write back, react to their message, or not reply\n"
    "delay_s:<seconds> - how long he waits before answering\n"
    "reaction:<heart|haha|thumbs_up|emphasis> - with tapback\n"
    "direction:<what they really mean or ask> ; <the move: engage fully, ask a follow-up, "
    "share something of his own, or just react> ; <what from the shared history to draw on, "
    "or nothing>\n"
    "Always write the direction, for a tapback or silence too.\n\n"
    "Judge from the thread. When a 'How Seth replies' line is given, it is measured from his "
    "own texts with this person (how soon he answers, how often he answers with only a "
    "reaction): follow it.\n\n"
    "Principles:\n"
    "- When they ask a real question or ask to be walked through something, answer it. Never "
    "dodge, stall or stay vague on purpose.\n"
    "- Draw only on what the thread or the Contact line shows. Never invent events, people, "
    "plans or outcomes; if he doesn't know how something went, he says so.\n"
    "- If they test whether he's real, never fabricate a memory.";

/* Expressive forms (HU_DIRECTOR_FORMS live): v1's vocabulary without its
 * length cues. */
static const char k_v2_forms[] =
    "\n\nOther forms, rare, and only what the 'This turn:' line allows: action:voice (a voice "
    "memo: they sent one, or a heartfelt moment); action:gif|gif:<search words> (playful, close "
    "friends or siblings only); effect:<impact|loud|gentle|invisibleink|confetti|lasers> on a "
    "text (a genuine big moment, never sad news); reply_to:true (answering an older message); "
    "action:share|share:<song|video|short|saved>|q:<words> (at most once a day, never on sad "
    "news).";

size_t hu_director_v2_system_prompt(char *buf, size_t cap) {
    if (!buf || cap == 0)
        return 0;
    size_t base = sizeof(k_v2_system) - 1;
    bool forms = hu_gate_mode_from_env("HU_DIRECTOR_FORMS", HU_GATE_OFF) == HU_GATE_LIVE;
    size_t extra = forms ? sizeof(k_v2_forms) - 1 : 0;
    if (base + extra + 1 > cap) {
        buf[0] = '\0';
        return 0;
    }
    memcpy(buf, k_v2_system, base);
    if (extra)
        memcpy(buf + base, k_v2_forms, extra);
    buf[base + extra] = '\0';
    return base + extra;
}

#define V2_ENTRY_MAX         130u
#define V2_FACTS_MAX         300u
#define V2_NEW_MAX           360u
#define V2_SITUATION_MAX     160u
#define V2_CONTACT_FIELD_MAX 48u

/* Longest prefix of s[0..n) no longer than max that does not split a UTF-8 sequence. */
static size_t utf8_prefix(const char *s, size_t n, size_t max) {
    if (n <= max)
        return n;
    size_t k = max;
    while (k > 0 && ((unsigned char)s[k] & 0xC0) == 0x80)
        k--;
    return k;
}

typedef struct {
    char *buf;
    size_t pos, lim;
    bool full;
} v2_out_t;

/* Appends s (newlines flattened, so a message cannot forge a "Seth:" line). */
static void put(v2_out_t *o, const char *s, size_t n, bool flatten) {
    if (o->full || o->pos + n + 1 > o->lim) {
        o->full = true;
        return;
    }
    for (size_t i = 0; i < n; i++)
        o->buf[o->pos++] = (flatten && (s[i] == '\n' || s[i] == '\r')) ? ' ' : s[i];
    o->buf[o->pos] = '\0';
}

static void put_lit(v2_out_t *o, const char *lit) {
    put(o, lit, strlen(lit), false);
}

static void put_field(v2_out_t *o, const char *s, size_t max) {
    size_t n = strlen(s);
    put(o, s, utf8_prefix(s, n, max), true);
}

static const char k_new_hdr[] = "\nNew message from them:\n";

size_t hu_director_v2_user_prompt(char *buf, size_t cap, const struct hu_contact_profile *cp,
                                  const hu_channel_history_entry_t *entries, size_t entry_count,
                                  const char *combined, size_t combined_len, const char *situation,
                                  const char *facts) {
    if (!buf || cap == 0)
        return 0;
    v2_out_t o = {buf, 0, cap < HU_DIRECTOR_V2_USER_CAP ? cap : HU_DIRECTOR_V2_USER_CAP, false};
    buf[0] = '\0';
    bool rel = cp && cp->relationship && cp->relationship[0];
    bool dun = cp && cp->dunbar_layer && cp->dunbar_layer[0];
    if (rel || dun) { /* relationship and layer only: never a name */
        put_lit(&o, "Contact: ");
        if (rel)
            put_field(&o, cp->relationship, V2_CONTACT_FIELD_MAX);
        if (rel && dun)
            put_lit(&o, "; ");
        if (dun) {
            put_lit(&o, "dunbar layer: ");
            put_field(&o, cp->dunbar_layer, V2_CONTACT_FIELD_MAX);
        }
        put_lit(&o, "\n");
    }
    if (facts && facts[0]) {
        put_field(&o, facts, V2_FACTS_MAX);
        put_lit(&o, "\n");
    }
    put_lit(&o, "Thread, oldest first:\n");

    /* The new message and the situation line always fit: the history gives
     * way, oldest message first. */
    size_t new_n = combined ? utf8_prefix(combined, combined_len, V2_NEW_MAX) : 0;
    size_t sit_n =
        situation && situation[0] ? utf8_prefix(situation, strlen(situation), V2_SITUATION_MAX) : 0;
    size_t tail = sizeof(k_new_hdr) - 1 + new_n + (sit_n ? 2 + sit_n : 0) + 1;
    size_t room = o.lim > o.pos + tail ? o.lim - o.pos - tail : 0;
    size_t last = entries ? entry_count : 0;
    size_t start = last > HU_DIRECTOR_V2_HISTORY ? last - HU_DIRECTOR_V2_HISTORY : 0;
    size_t first = last, used = 0;
    while (first > start) {
        const char *t = entries[first - 1].text;
        size_t need = 6 + utf8_prefix(t, strnlen(t, sizeof(entries[0].text)), V2_ENTRY_MAX) + 1;
        if (used + need > room)
            break;
        used += need;
        first--;
    }
    for (size_t i = first; i < last; i++) {
        put_lit(&o, entries[i].from_me ? "Seth: " : "Them: ");
        size_t tn = strnlen(entries[i].text, sizeof(entries[i].text));
        put(&o, entries[i].text, utf8_prefix(entries[i].text, tn, V2_ENTRY_MAX), true);
        put_lit(&o, "\n");
    }
    put_lit(&o, k_new_hdr);
    if (new_n)
        put(&o, combined, new_n, false);
    if (sit_n) {
        put_lit(&o, "\n\n");
        put(&o, situation, sit_n, true);
    }
    return o.pos;
}

/* ── the call ──────────────────────────────────────────────────────── */

bool hu_director_v2_call(hu_allocator_t *alloc, hu_provider_t *provider, const char *model,
                         size_t model_len, const struct hu_contact_profile *cp,
                         const hu_channel_history_entry_t *entries, size_t entry_count,
                         const char *combined, size_t combined_len, const char *situation,
                         const hu_tapback_profile_t *tp, const char *facts,
                         hu_director_result_t *result, hu_tapback_src_t *src,
                         size_t *prompt_bytes) {
    if (src)
        *src = HU_TAPBACK_SRC_NODATA;
    if (prompt_bytes)
        *prompt_bytes = 0;
    if (!alloc || !result)
        return false;
    memset(result, 0, sizeof(*result));
    if (!provider || !provider->vtable || !provider->vtable->chat_with_system)
        return false;

    /* Heap, per call: the director can run for several contacts at once. */
    size_t cap = HU_DIRECTOR_V2_SYSTEM_CAP + 1 + HU_DIRECTOR_V2_USER_CAP;
    char *sys = (char *)alloc->alloc(alloc->ctx, cap);
    if (!sys)
        return false;
    char *user = sys + HU_DIRECTOR_V2_SYSTEM_CAP + 1;
    size_t sys_len = hu_director_v2_system_prompt(sys, HU_DIRECTOR_V2_SYSTEM_CAP + 1);
    size_t user_len =
        hu_director_v2_user_prompt(user, HU_DIRECTOR_V2_USER_CAP, cp, entries, entry_count,
                                   combined, combined_len, situation, facts);
    char *raw = NULL;
    size_t raw_len = 0;
    hu_error_t err =
        sys_len == 0
            ? HU_ERR_INTERNAL
            : provider->vtable->chat_with_system(provider->ctx, alloc, sys, sys_len, user, user_len,
                                                 model, model_len, 0.4, &raw, &raw_len);
    alloc->free(alloc->ctx, sys, cap);
    if (err != HU_OK || !raw || raw_len == 0 || raw_len > 600) {
        if (raw)
            alloc->free(alloc->ctx, raw, raw_len + 1);
        return false;
    }
    hu_daemon_parse_director_result(raw, raw_len, result);
    alloc->free(alloc->ctx, raw, raw_len + 1);
    hu_tapback_src_t ts = hu_director_v2_tapback_check(result, tp);
    if (src)
        *src = ts;
    if (prompt_bytes)
        *prompt_bytes = sys_len + user_len;
    return true;
}

static const char *action_name(const hu_director_result_t *r, bool ok) {
    if (!ok)
        return "none";
    return r->action == DIR_TAPBACK ? "tapback" : r->action == DIR_SILENCE ? "silence" : "text";
}

static int brevity_bit(const hu_director_result_t *r, bool ok) {
    return ok && hu_director_brevity_directive(r->direction, strlen(r->direction)) ? 1 : 0;
}

/* ── where the director's provider sends the thread ────────────────── */

const char *hu_director_v2_endpoint(const struct hu_agent *agent, const hu_provider_t *provider) {
    if (!agent || !agent->config || !provider || !provider->vtable)
        return NULL;
    /* Under #587 local_only the director borrows the agent's own provider:
     * its endpoint is the default provider's. Otherwise it is the provider
     * of its own name (today "gemini"; no base_url means Vertex). */
    const char *name = provider->ctx == agent->provider.ctx ? agent->config->default_provider
                       : provider->vtable->get_name ? provider->vtable->get_name(provider->ctx)
                                                    : NULL;
    return hu_config_get_provider_base_url(agent->config, name);
}

bool hu_director_v2_endpoint_is_local(const struct hu_agent *agent, const hu_provider_t *provider) {
    const char *url = hu_director_v2_endpoint(agent, provider);
    return url && hu_compatible_url_is_loopback(url, strlen(url)); /* unknown: not local */
}

/* ── one v2 decision, shared by LIVE (inline) and SHADOW (worker) ──── */

typedef struct {
    hu_director_result_t result;
    bool ok;
    hu_tapback_src_t src;
    hu_dir_shape_t shape;
    size_t bytes;
} v2_outcome_t;

static void run_v2(hu_allocator_t *alloc, hu_channel_t *channel, const char *key, size_t key_len,
                   const char *persona, size_t persona_len, const hu_contact_profile_t *cp,
                   const char *combined, size_t combined_len,
                   const hu_channel_history_entry_t *entries, size_t entry_count,
                   const char *situation, v2_outcome_t *out) {
    memset(out, 0, sizeof(*out));
    /* 12 messages: a separate read, so nothing else on the turn sees a longer history. */
    hu_channel_history_entry_t *hist = NULL;
    size_t hist_n = 0;
    if (channel && channel->vtable && channel->vtable->load_conversation_history && key &&
        key_len > 0 &&
        channel->vtable->load_conversation_history(
            channel->ctx, alloc, key, key_len, HU_DIRECTOR_V2_HISTORY, &hist, &hist_n) != HU_OK) {
        hist = NULL;
        hist_n = 0;
    }
    /* What Seth actually does with this contact and this shape of message. */
    out->shape = hu_director_inbound_shape(combined, combined_len);
    hu_tapback_profile_t tp;
    (void)hu_tapback_profile_load(persona, persona_len, key, key_len, out->shape, &tp);
    char facts[V2_FACTS_MAX + 20];
    (void)hu_tapback_profile_facts(&tp, out->shape, facts, sizeof(facts));
    out->ok = g_classify_provider_ok &&
              hu_director_v2_call(alloc, &g_classify_provider, g_classify_model,
                                  g_classify_model_len, cp, hist_n > 0 ? hist : entries,
                                  hist_n > 0 ? hist_n : entry_count, combined, combined_len,
                                  situation, &tp, facts, &out->result, &out->src, &out->bytes);
    if (hist)
        alloc->free(alloc->ctx, hist, hist_n * sizeof(hu_channel_history_entry_t));
}

/* ── SHADOW: off the reply path ────────────────────────────────────── */

/* One shadow job in flight at a time: shadow is a sample, and a second
 * director round trip must never queue behind replies. */
static atomic_int s_shadow_inflight;

typedef struct {
    hu_channel_t *channel; /* channels live for the process */
    char *key, *combined, *situation, *persona, *relationship, *dunbar;
    size_t key_len, combined_len, persona_len;
    hu_channel_history_entry_t *entries;
    size_t entry_count;
    const char *v1_action; /* static string */
    int v1_brevity;
} v2_shadow_job_t;

static char *dup_n(const char *s, size_t n) {
    if (!s)
        return NULL;
    char *d = (char *)malloc(n + 1);
    if (d) {
        memcpy(d, s, n);
        d[n] = '\0';
    }
    return d;
}

static char *dup_z(const char *s) {
    return s ? dup_n(s, strlen(s)) : NULL;
}

static void job_free(v2_shadow_job_t *j) {
    free(j->key);
    free(j->combined);
    free(j->situation);
    free(j->persona);
    free(j->relationship);
    free(j->dunbar);
    free(j->entries);
    free(j);
}

static void *shadow_worker(void *arg) {
    v2_shadow_job_t *j = (v2_shadow_job_t *)arg;
    hu_allocator_t alloc = hu_system_allocator();
    hu_contact_profile_t cp;
    memset(&cp, 0, sizeof(cp));
    cp.relationship = j->relationship;
    cp.dunbar_layer = j->dunbar;
    v2_outcome_t v2;
    run_v2(&alloc, j->channel, j->key, j->key_len, j->persona, j->persona_len, &cp, j->combined,
           j->combined_len, j->entries, j->entry_count, j->situation, &v2);
    hu_log_info("director", NULL,
                "[director_v2 shadow] v1_action=%s v2_action=%s v1_brevity=%d v2_brevity=%d "
                "tapback_overridden=%d tapback_src=%s shape=%s v2_bytes=%zu v2=ran",
                j->v1_action, action_name(&v2.result, v2.ok), j->v1_brevity,
                brevity_bit(&v2.result, v2.ok), v2.ok && v2.src == HU_TAPBACK_SRC_LEARNED,
                hu_tapback_src_name(v2.src), hu_director_shape_name(v2.shape), v2.bytes);
    job_free(j);
    atomic_fetch_sub(&s_shadow_inflight, 1);
    return NULL;
}

static bool shadow_enqueue(const hu_agent_t *agent, hu_channel_t *channel, const char *key,
                           size_t key_len, const char *combined, size_t combined_len,
                           const hu_channel_history_entry_t *entries, size_t entry_count,
                           const char *situation, const hu_director_result_t *v1, bool v1_ok) {
    int idle = 0;
    if (!atomic_compare_exchange_strong(&s_shadow_inflight, &idle, 1))
        return false;
    v2_shadow_job_t *j = (v2_shadow_job_t *)calloc(1, sizeof(*j));
    const hu_persona_t *persona = agent ? agent->persona : NULL;
    const hu_contact_profile_t *cp =
        persona && key ? hu_persona_find_contact(persona, key, key_len) : NULL;
    if (j) {
        j->channel = channel;
        j->key = dup_n(key, key ? key_len : 0);
        j->key_len = key ? key_len : 0;
        j->combined = dup_n(combined, combined ? combined_len : 0);
        j->combined_len = combined ? combined_len : 0;
        j->situation = dup_z(situation);
        if (persona && persona->name) {
            j->persona = dup_n(persona->name, persona->name_len);
            j->persona_len = persona->name_len;
        }
        j->relationship = cp ? dup_z(cp->relationship) : NULL;
        j->dunbar = cp ? dup_z(cp->dunbar_layer) : NULL;
        if (entries && entry_count > 0) {
            j->entries = (hu_channel_history_entry_t *)malloc(entry_count * sizeof(*entries));
            if (j->entries) {
                memcpy(j->entries, entries, entry_count * sizeof(*entries));
                j->entry_count = entry_count;
            }
        }
        j->v1_action = action_name(v1, v1_ok);
        j->v1_brevity = brevity_bit(v1, v1_ok);
    }
    pthread_attr_t attr;
    pthread_t tid;
    bool started = false;
    if (j && pthread_attr_init(&attr) == 0) {
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        started = pthread_create(&tid, &attr, shadow_worker, j) == 0;
        pthread_attr_destroy(&attr);
    }
    if (!started) {
        if (j)
            job_free(j);
        atomic_fetch_sub(&s_shadow_inflight, 1);
    }
    return started;
}

bool hu_director_v2_shadow_drain(unsigned timeout_ms) {
    for (unsigned waited = 0; atomic_load(&s_shadow_inflight) > 0; waited += 5) {
        if (waited >= timeout_ms)
            return false;
        struct timespec ts = {0, 5 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    return true;
}

/* ── the gate ──────────────────────────────────────────────────────── */

/* HU_DIRECTOR_V2 promotion is gated on docs/guides/director-v2.md: shadow brevity share
 * <= 25%, tapbacks that match Seth's learned behaviour, the replay A/B, and the blind gate. */
bool hu_director_v2_decide(hu_allocator_t *alloc, struct hu_agent *agent, hu_channel_t *channel,
                           const char *key, size_t key_len, const char *combined,
                           size_t combined_len, const hu_channel_history_entry_t *entries,
                           size_t entry_count, const char *situation,
                           hu_director_result_t *result) {
    hu_gate_mode_t mode = hu_gate_mode_from_env("HU_DIRECTOR_V2", HU_GATE_OFF);
    if (mode == HU_GATE_OFF)
        return hu_daemon_director_call(alloc, combined, combined_len, entries, entry_count,
                                       situation, result);

    if (mode == HU_GATE_SHADOW) {
        /* v1 decides and returns now; v2 never sits on the reply path. Until
         * #587 moves the director local, v2 runs only on a loopback provider:
         * a thrown-away result is no reason to send the thread to a cloud. */
        bool v1_ok = hu_daemon_director_call(alloc, combined, combined_len, entries, entry_count,
                                             situation, result);
        const char *skip = !g_classify_provider_ok ? "skipped_noprovider"
                           : !hu_director_v2_endpoint_is_local(agent, &g_classify_provider)
                               ? "skipped_nonlocal"
                           : !shadow_enqueue(agent, channel, key, key_len, combined, combined_len,
                                             entries, entry_count, situation, result, v1_ok)
                               ? "skipped_busy"
                               : NULL;
        if (skip)
            hu_log_info("director", NULL, "[director_v2 shadow] v1_action=%s v1_brevity=%d v2=%s",
                        action_name(result, v1_ok), brevity_bit(result, v1_ok), skip);
        return v1_ok;
    }

    const hu_persona_t *persona = agent ? agent->persona : NULL;
    const hu_contact_profile_t *cp =
        persona && key ? hu_persona_find_contact(persona, key, key_len) : NULL;
    v2_outcome_t v2;
    run_v2(alloc, channel, key, key_len, persona ? persona->name : NULL,
           persona ? persona->name_len : 0, cp, combined, combined_len, entries, entry_count,
           situation, &v2);
    hu_log_info("director", NULL,
                "[director_v2 live] v2_action=%s v2_brevity=%d tapback_overridden=%d "
                "tapback_src=%s shape=%s v2_bytes=%zu fallback_v1=%d",
                action_name(&v2.result, v2.ok), brevity_bit(&v2.result, v2.ok),
                v2.ok && v2.src == HU_TAPBACK_SRC_LEARNED, hu_tapback_src_name(v2.src),
                hu_director_shape_name(v2.shape), v2.bytes, v2.ok ? 0 : 1);
    if (!v2.ok)
        return hu_daemon_director_call(alloc, combined, combined_len, entries, entry_count,
                                       situation, result);
    *result = v2.result;
    return true;
}
