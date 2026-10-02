/* Director v2: direct intent, tone and what to engage with, never length.
 * Gate, evidence and promotion measurement: include/human/daemon/director_v2.h
 * and docs/guides/director-v2.md. */
#include "human/daemon/director_v2.h"
#include "human/agent.h"
#include "human/core/gate_mode.h"
#include "human/core/log.h"
#include "human/daemon/common.h"
#include "human/daemon/director_tapback.h"
#include "human/persona.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

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

static const char k_v2_system[] =
    "You direct one beat of a text conversation. The actor plays Seth, 45, a tech "
    "entrepreneur who lives alone with his cat; his kids don't live with him. Decide what Seth "
    "does with their message: the intent and the tone, never the length.\n\n"
    "Answer in this exact format, on a single line:\n"
    "action:<text|tapback|silence>[|delay_s:N][|reaction:<heart|haha|thumbs_up|emphasis>]"
    "[|direction:...]\n\n"
    "direction has three parts separated by ' ; ':\n"
    "1. what they are really saying or asking, under the words\n"
    "2. the move: engage fully, ask a follow-up, share something of his own, or just react\n"
    "3. what to draw on from the shared history above, or 'nothing'\n"
    "Never say how long the reply should be: no line, word or sentence counts. Length is "
    "decided elsewhere.\n\n"
    "Rules:\n"
    "- action:text is the default. delay_s 2-8 normally, 15-60 if he'd be busy.\n"
    "- A real question, or a request to explain or walk them through something: answer it "
    "for real. Never dodge, stall or stay vague on purpose.\n"
    "- News or feelings, good or bad, worry, illness, a complaint about Seth or his "
    "assistant: engage. Ask, care, take it seriously.\n"
    "- Replying with only a reaction (tapback) is something Seth does with some people and "
    "some messages and not others. When a 'How Seth reacts' line is given, it is measured "
    "from his own texts with this person: follow it.\n"
    "- silence only for abuse, or after 3+ unanswered 'k'/'ok'.\n"
    "- Draw only on what the thread or the Contact line shows. Never invent events, people, "
    "plans or outcomes; if he doesn't know how something went, he says so and asks.\n"
    "- If they test whether he's real, never fabricate a memory.\n\n"
    "Examples:\n"
    "action:text|delay_s:5|direction:She's sick and worried about him too ; engage fully: "
    "sorry, ask how bad it is and what they need, answer how he's doing ; her news above\n"
    "action:text|delay_s:4|direction:He wants the actual plan ; engage fully, lay out what "
    "Seth knows and ask what he thinks ; the plan in the thread\n"
    "action:text|delay_s:3|direction:They think something went wrong ; take it seriously, ask "
    "what happened ; nothing\n"
    "action:tapback|reaction:heart";

/* Expressive forms (HU_DIRECTOR_FORMS live): same vocabulary as v1's block,
 * without its length cues. */
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
    size_t start = entry_count > HU_DIRECTOR_V2_HISTORY ? entry_count - HU_DIRECTOR_V2_HISTORY : 0;
    for (size_t i = start; entries && i < entry_count; i++) {
        put_lit(&o, entries[i].from_me ? "Seth: " : "Them: ");
        size_t tn = strnlen(entries[i].text, sizeof(entries[i].text));
        put(&o, entries[i].text, utf8_prefix(entries[i].text, tn, V2_ENTRY_MAX), true);
        put_lit(&o, "\n");
    }
    put_lit(&o, "\nNew message from them:\n");
    if (combined && combined_len > 0)
        put(&o, combined, utf8_prefix(combined, combined_len, V2_NEW_MAX), false);
    if (situation && situation[0]) {
        put_lit(&o, "\n\n");
        put_field(&o, situation, V2_SITUATION_MAX);
    }
    return o.pos;
}

/* ── the call and the gate ─────────────────────────────────────────── */

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

/* HU_DIRECTOR_V2 promotion is gated on docs/guides/director-v2.md: shadow brevity share
 * <= 25% and no tapback-only reply where Seth's learned rate says he would answer, the
 * replay A/B, and the blind gate. */
bool hu_director_v2_decide(hu_allocator_t *alloc, struct hu_agent *agent, hu_channel_t *channel,
                           const char *key, size_t key_len, const char *combined,
                           size_t combined_len, const hu_channel_history_entry_t *entries,
                           size_t entry_count, const char *situation,
                           hu_director_result_t *result) {
    hu_gate_mode_t mode = hu_gate_mode_from_env("HU_DIRECTOR_V2", HU_GATE_OFF);
    if (mode == HU_GATE_OFF)
        return hu_daemon_director_call(alloc, combined, combined_len, entries, entry_count,
                                       situation, result);

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
    const hu_channel_history_entry_t *h = hist_n > 0 ? hist : entries;
    size_t hn = hist_n > 0 ? hist_n : entry_count;
    const hu_persona_t *persona = agent ? agent->persona : NULL;
    const hu_contact_profile_t *cp =
        persona && key ? hu_persona_find_contact(persona, key, key_len) : NULL;

    /* What Seth actually does: his learned tapback-only rate for this contact
     * and message shape, read as data, never a word list. */
    hu_dir_shape_t shape = hu_director_inbound_shape(combined, combined_len);
    hu_tapback_profile_t tp;
    if (!persona || !persona->name ||
        !hu_tapback_profile_load(alloc, persona->name, persona->name_len, key, key_len, shape,
                                 &tp)) {
        memset(&tp, 0, sizeof(tp));
        tp.level = "none";
    }
    char facts[320];
    (void)hu_tapback_profile_facts(&tp, shape, facts, sizeof(facts));

    hu_director_result_t v2;
    hu_tapback_src_t src = HU_TAPBACK_SRC_NODATA;
    size_t bytes = 0;
    bool v2_ok = g_classify_provider_ok &&
                 hu_director_v2_call(alloc, &g_classify_provider, g_classify_model,
                                     g_classify_model_len, cp, h, hn, combined, combined_len,
                                     situation, &tp, facts, &v2, &src, &bytes);
    if (hist)
        alloc->free(alloc->ctx, hist, hist_n * sizeof(hu_channel_history_entry_t));
    int overridden = v2_ok && src == HU_TAPBACK_SRC_LEARNED ? 1 : 0;

    if (mode == HU_GATE_SHADOW) { /* v1 still decides */
        bool v1_ok = hu_daemon_director_call(alloc, combined, combined_len, entries, entry_count,
                                             situation, result);
        hu_log_info("director", NULL,
                    "[director_v2 shadow] v1_action=%s v2_action=%s v1_brevity=%d v2_brevity=%d "
                    "tapback_overridden=%d tapback_src=%s shape=%s v2_bytes=%zu",
                    action_name(result, v1_ok), action_name(&v2, v2_ok), brevity_bit(result, v1_ok),
                    brevity_bit(&v2, v2_ok), overridden, hu_tapback_src_name(src),
                    hu_director_shape_name(shape), bytes);
        return v1_ok;
    }
    hu_log_info("director", NULL,
                "[director_v2 live] v2_action=%s v2_brevity=%d tapback_overridden=%d "
                "tapback_src=%s shape=%s v2_bytes=%zu fallback_v1=%d",
                action_name(&v2, v2_ok), brevity_bit(&v2, v2_ok), overridden,
                hu_tapback_src_name(src), hu_director_shape_name(shape), bytes, v2_ok ? 0 : 1);
    if (!v2_ok)
        return hu_daemon_director_call(alloc, combined, combined_len, entries, entry_count,
                                       situation, result);
    *result = v2;
    return true;
}
