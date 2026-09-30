#include "human/agent.h"
#include "human/channel.h"
#include "human/cognition/emotional.h"
#include "human/context/conversation.h"
#include "human/core/error.h"
#include "human/core/gate_mode.h"
#include "human/core/log.h"
#include "human/core/string.h"
#include "human/daemon/common.h"
#include "human/daemon/director.h"
#include "human/daemon_comfort_summary.h"
#include "human/memory.h"
#include "human/memory/deep_extract.h"
#include "human/provider.h"

/* Private agent header: the G6 wiring below sets agent-internal director
 * state (borrowed scene pointer + history ring). Same cross-module
 * private include the gateway uses (src/gateway/openai_compat.c:2). */
#include "../agent/agent_internal.h"

#ifdef HU_ENABLE_SQLITE
#include "human/memory/superhuman.h"
#endif

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Lightweight classification provider (e.g. Gemini Flash Lite) for hybrid routing.
 * When llm_decides is active, the primary agent turn uses the local model while
 * classification/scoring calls use this fast cloud provider.
 * Shared with daemon.c for initialization and hybrid routing decisions. */
hu_provider_t g_classify_provider;
bool g_classify_provider_ok = false;
const char *g_classify_model = "gemini-3.1-flash-lite";
size_t g_classify_model_len = 21;

/* W9: real-time emotion detection stays here (per-message, from live
 * history) while the world model caches a snapshot. The two compose:
 * this function feeds live state, the world model feeds trend. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((unused))
#endif
hu_emotional_state_t hu_daemon_detect_emotion(hu_allocator_t *alloc, hu_agent_t *agent,
                                              const hu_channel_history_entry_t *entries,
                                              size_t count) {
#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)alloc;
    (void)agent;
    return hu_conversation_detect_emotion(entries, count);
#else
    /* Hybrid routing: prefer fast cloud classify provider when available */
    if (g_classify_provider_ok && g_classify_provider.vtable &&
        g_classify_provider.vtable->chat_with_system)
        return hu_conversation_detect_emotion_llm(alloc, &g_classify_provider, g_classify_model,
                                                  g_classify_model_len, entries, count);
    if (agent && agent->provider.vtable && agent->provider.vtable->chat_with_system)
        return hu_conversation_detect_emotion_llm(alloc, &agent->provider, agent->model_name,
                                                  agent->model_name_len, entries, count);
    return hu_conversation_detect_emotion(entries, count);
#endif
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((unused))
#endif
const char *hu_director_form_name(hu_director_form_t form) {
    static const char *const names[] = {"text", "voice", "tapback", "gif", "silence", "share"};
    return (unsigned)form < sizeof(names) / sizeof(names[0]) ? names[form] : "text";
}

/* The new fields are read only from the head of the line: `direction:` is
 * free text and always last, so a tone cue mentioning "effect:" is not one. */
static const char *head_find(const char *raw, size_t head_len, const char *key) {
    const char *p = strstr(raw, key);
    return (p && (size_t)(p - raw) < head_len) ? p : NULL;
}

/* Copy a field value up to '|' or end of head into out. */
static void head_value(const char *v, const char *head_end, char *out, size_t cap) {
    size_t n = 0;
    while (v + n < head_end && v[n] != '|' && v[n] != '\n' && n + 1 < cap)
        n++;
    while (n > 0 && (v[n - 1] == ' ' || v[n - 1] == '\r'))
        n--;
    memcpy(out, v, n);
    out[n] = '\0';
}

static void parse_expressive_fields(const char *raw, size_t len, hu_director_result_t *out) {
    const char *dir = strstr(raw, "direction:");
    size_t head_len = dir ? (size_t)(dir - raw) : len;
    const char *head_end = raw + head_len;
    static const char *const effects[] = {"impact",       "loud",     "gentle",
                                          "invisibleink", "confetti", "lasers"};
    const char *ep = head_find(raw, head_len, "effect:");
    if (ep) {
        char e[16];
        head_value(ep + 7, head_end, e, sizeof(e));
        for (size_t i = 0; i < sizeof(effects) / sizeof(effects[0]); i++)
            if (strcmp(e, effects[i]) == 0)
                memcpy(out->effect, e, strlen(e) + 1);
    }
    out->reply_to = head_find(raw, head_len, "reply_to:true") != NULL;
    const char *gp = head_find(raw, head_len, "gif:");
    if (gp)
        head_value(gp + 4, head_end, out->gif_query, sizeof(out->gif_query));
    const char *sp = head_find(raw, head_len, "share:");
    if (sp) {
        static const char *const kinds[] = {"", "song", "video", "short", "saved"};
        char k[8];
        head_value(sp + 6, head_end, k, sizeof(k));
        for (size_t i = 1; i < sizeof(kinds) / sizeof(kinds[0]); i++)
            if (strcmp(k, kinds[i]) == 0)
                out->share = (hu_share_kind_t)i;
    }
    const char *qp = head_find(raw, head_len, "q:");
    if (qp)
        head_value(qp + 2, head_end, out->share_query, sizeof(out->share_query));
}

static const char k_director_system[] =
    "You are a dialogue director for a texting scene. The actor plays Seth, a 45yo "
    "tech entrepreneur. Lives alone with his cat. Kids don't live with him. "
    "Decide his BEHAVIOR — not just words.\n\n"
    "Reply in this exact format (one line, pipe-separated):\n"
    "action:<text|tapback|silence>[|delay_s:N][|reaction:<heart|haha|thumbs_up|emphasis>]"
    "[|burst:true][|direction:...]\n\n"
    "Rules:\n"
    "- DEFAULT is action:text. When in doubt, respond.\n"
    "- action:text — Seth sends a text. Include delay_s (seconds to wait before replying, "
    "2-8 for normal, 15-60 for busy/low-priority) and direction for tone/length.\n"
    "- BREVITY IS THE DEFAULT. Most texts are a few words — a reaction, one fact, an "
    "acknowledgment. Your `direction` should almost always carry a tight length cue: "
    "'one line', 'a few words', 'just a word or two'. Reserve longer only for genuinely "
    "weighty moments.\n"
    "- Seth does NOT interrogate. He often just acknowledges, states one thing, or reacts "
    "and stops — he does NOT tack on a follow-up question to keep the conversation going. "
    "Direct a question only when he'd genuinely want the answer; a reply with no question "
    "is normal and usually right.\n"
    "- THE UNSAID is a real, human move. People don't answer every question, don't fill "
    "every gap, and often just react. NOT addressing the literal question, a one-word "
    "reply, or a tapback is frequently the MOST human choice — direct it when it fits, "
    "especially on low-stakes or winding-down threads. Withholding has weight; restraint "
    "reads as real.\n"
    "- action:tapback — Seth REACTS instead of texting. This is what real humans do "
    "constantly. Use tapback for:\n"
    "  * Standalone photos/images — ALWAYS heart or haha, never text a photo description\n"
    "  * Pure reactions: 'haha nice', 'lol', 'omg', 'damn', 'yooo'\n"
    "  * Simple acknowledgments: 'ok', 'sounds good', 'got it', 'bet'\n"
    "  * Conversation closers: last message in a natural exchange ending\n"
    "  * Memes, GIFs, links to funny content\n"
    "  * When they share good news and a heart says it all\n"
    "  Reaction types: heart (love/appreciation/photos), haha (funny), "
    "thumbs_up (agreement/acknowledgment), emphasis (surprise/excitement)\n"
    "- action:silence — RARE. Only use when they're being toxic/abusive, or after 3+ "
    "unanswered low-effort messages in a row ('k', 'ok', '...'). NEVER use silence for "
    "greetings, questions, or any message that expects a reply.\n"
    "- burst:true — Seth sends 3-4 rapid messages. Only for urgent/exciting news.\n"
    "- CRITICAL: If they're testing whether you're real/AI, NEVER fabricate specific "
    "memories. Deflect: 'dude idk, that was forever ago'.\n"
    "- The `direction:` field is an OBJECTIVE + tone/pacing — what Seth is trying to DO in "
    "this beat, not just a mood. Direct a playable action: 'just acknowledge and let it "
    "go', 'make him laugh', 'close this out warm', 'get the one detail he needs', 'tease "
    "him a little', 'reassure and drop it'. An objective produces a specific, alive reply; "
    "a bare mood produces generic mush. Pair it with a length cue.\n"
    "- CRITICAL: the objective is an ACTION, never an invented FACT. NEVER put specific "
    "reasons, activities, people, places, or topics into `direction` that aren't visible "
    "in the Recent thread — the downstream actor pastes them in verbatim and it becomes "
    "cross-contact bleed (US-16). Forbidden: 'because he's getting back to the drink', "
    "'mention the cat', 'reference yesterday's meeting'. Allowed: 'short empathetic "
    "reaction, 5 words', 'busy, one-word reply', 'end it with a joke'. Recalling a REAL "
    "shared memory is the ACTOR's job from what it actually knows — never yours to "
    "invent.\n"
    "- When they ask how something in Seth's life went, whether he did something, or how "
    "someone is doing (a meeting, a concert, a trip, a person's news) and the Recent thread "
    "does not establish it, Seth has no answer on record. Never direct an outcome. Direct: "
    "'don't say how it went, ask which one or say not sure yet, one line'.\n\n";

/* Split from k_director_system: one literal would pass the 4095-byte ISO C
 * limit (-Woverlength-strings). hu_daemon_director_system_prompt joins them. */
static const char k_director_examples[] =
    "Examples:\n"
    "action:text|delay_s:3|direction:Just acknowledge the hard news, 5 words, don't fix it\n"
    "action:text|delay_s:2|direction:Greet back, match his energy, one line\n"
    "action:text|delay_s:4|direction:don't say how it went, ask which meeting, one line "
    "(they asked how a meeting went; nothing in the thread about it)\n"
    "action:tapback|reaction:heart (they sent a photo)\n"
    "action:tapback|reaction:haha (they said something funny)\n"
    "action:tapback|reaction:thumbs_up (simple acknowledgment)\n"
    "action:text|delay_s:2|burst:true|direction:Match urgency, 3 rapid messages\n"
    "action:text|delay_s:45|direction:He's busy, one-word reply when he gets back";

/* Spec 2026-09-28-expressive-imessage, Phase 2: the rest of how Seth responds.
 * Appended only when HU_DIRECTOR_FORMS is shadow or live; off, the prompt is
 * exactly today's. */
static const char k_director_forms[] =
    "\n\nMORE WAYS SETH RESPONDS (rare; most replies are still plain texts). Only use what the "
    "'This turn:' line says is available.\n"
    "- action:voice — a voice memo instead of a text: when they sent a voice memo, a heartfelt "
    "moment, or a real question worth talking through. The direction then describes a spoken "
    "memo (a few connected thoughts), not a text length.\n"
    "- action:gif|gif:<2-3 search words> — a GIF instead of a text, only in playful "
    "back-and-forth with a close friend or sibling. Never with parents or business contacts, "
    "never about anything serious.\n"
    "- effect:<impact|loud|gentle|invisibleink|confetti|lasers> — add to a text only for a "
    "genuine big moment: confetti for a birthday or big news, invisibleink for a surprise or "
    "spoiler, impact for a mic-drop line. Almost never. Never on sad news.\n"
    "- reply_to:true — thread your text onto their message when you're answering something "
    "older and newer messages came in between, or when it's a group chat.\n"
    "- action:share|share:<song|video|short|saved>|q:<search words> — send something alongside "
    "your text, the way people do: a song when they mention a band or need a lift, a video or a "
    "short when something reminds you of one, or 'saved' when the 'This turn:' line says Seth "
    "saved something for them. At most once a day per person; never on sad news.\n"
    "Examples:\n"
    "action:text|delay_s:4|effect:confetti|direction:Congratulate her big, one line\n"
    "action:voice|delay_s:40|direction:She sent a voice memo; talk back warmly, a few thoughts\n"
    "action:gif|gif:slow clap|direction:He nailed the joke, answer with a GIF\n"
    "action:text|delay_s:6|reply_to:true|direction:Answer his earlier question, a few words\n"
    "action:share|share:song|q:beach house space song|direction:She loves them, share it\n"
    "action:share|share:short|q:cat knocks glass off table|direction:Make him laugh";

size_t hu_daemon_director_system_prompt(char *buf, size_t cap) {
    if (!buf || cap == 0)
        return 0;
    size_t rules = sizeof(k_director_system) - 1;
    size_t base = rules + sizeof(k_director_examples) - 1;
    bool forms = hu_gate_mode_from_env("HU_DIRECTOR_FORMS", HU_GATE_OFF) != HU_GATE_OFF;
    size_t extra = forms ? sizeof(k_director_forms) - 1 : 0;
    if (base + extra + 1 > cap) {
        buf[0] = '\0';
        return 0;
    }
    memcpy(buf, k_director_system, rules);
    memcpy(buf + rules, k_director_examples, sizeof(k_director_examples) - 1);
    if (extra)
        memcpy(buf + base, k_director_forms, extra);
    buf[base + extra] = '\0';
    return base + extra;
}

void hu_daemon_parse_director_result(const char *raw, size_t len, hu_director_result_t *out) {
    memset(out, 0, sizeof(*out));
    out->action = DIR_TEXT;

    if (!raw || len == 0)
        return;

    /* Look for "action:" prefix — if absent, treat whole string as direction */
    const char *ap = strstr(raw, "action:");
    if (!ap) {
        size_t cp = len < sizeof(out->direction) - 1 ? len : sizeof(out->direction) - 1;
        memcpy(out->direction, raw, cp);
        out->direction[cp] = '\0';
        return;
    }

    const char *val = ap + 7; /* skip "action:" */
    if (strncmp(val, "tapback", 7) == 0) {
        out->action = DIR_TAPBACK;
        out->form = HU_DIR_FORM_TAPBACK;
    } else if (strncmp(val, "silence", 7) == 0) {
        out->action = DIR_SILENCE;
        out->form = HU_DIR_FORM_SILENCE;
    } else if (strncmp(val, "voice", 5) == 0) {
        out->form = HU_DIR_FORM_VOICE; /* runs as text until voice-first owns it */
    } else if (strncmp(val, "share", 5) == 0) {
        out->form = HU_DIR_FORM_SHARE; /* the reply is still a text; the share rides along */
    } else if (strncmp(val, "gif", 3) == 0) {
        out->form = HU_DIR_FORM_GIF; /* runs as text until the GIF executor is LIVE */
    }
    parse_expressive_fields(raw, len, out);

    /* Parse "|delay_s:N" */
    const char *dp = strstr(raw, "delay_s:");
    if (dp)
        out->delay_s = (uint32_t)strtoul(dp + 8, NULL, 10);

    /* Parse "|burst:true" */
    out->burst = (strstr(raw, "burst:true") != NULL);

    /* Parse "|reaction:<type>" */
    const char *rp = strstr(raw, "reaction:");
    if (rp) {
        const char *rv = rp + 9;
        if (strncmp(rv, "heart", 5) == 0)
            out->reaction = HU_REACTION_HEART;
        else if (strncmp(rv, "haha", 4) == 0)
            out->reaction = HU_REACTION_HAHA;
        else if (strncmp(rv, "thumbs_up", 9) == 0)
            out->reaction = HU_REACTION_THUMBS_UP;
        else if (strncmp(rv, "emphasis", 8) == 0)
            out->reaction = HU_REACTION_EMPHASIS;
        else if (strncmp(rv, "thumbs_down", 11) == 0)
            out->reaction = HU_REACTION_THUMBS_DOWN;
        else if (strncmp(rv, "question", 8) == 0)
            out->reaction = HU_REACTION_QUESTION;
    }

    /* Parse "|direction:..." (everything after "direction:") */
    const char *drp = strstr(raw, "direction:");
    if (drp) {
        const char *dv = drp + 10;
        size_t offset = (size_t)(dv - raw);
        if (offset > len)
            return;
        size_t rem = len - offset;
        size_t cp = rem < sizeof(out->direction) - 1 ? rem : sizeof(out->direction) - 1;
        memcpy(out->direction, dv, cp);
        out->direction[cp] = '\0';
        /* Trim trailing whitespace/pipe from direction */
        while (cp > 0 && (out->direction[cp - 1] == '|' || out->direction[cp - 1] == '\n' ||
                          out->direction[cp - 1] == '\r' || out->direction[cp - 1] == ' '))
            out->direction[--cp] = '\0';
    }
}

/* Real-time scene director: Flash Lite call that returns structured meta-behavior.
 * Decides action (text/tapback/silence), delay, reaction type, burst mode, and
 * performance direction. Only runs when llm_decides && g_classify_provider_ok.
 * Returns true if result is valid. Caller uses result to route behavior. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((unused))
#endif
bool hu_daemon_director_call(hu_allocator_t *alloc, const char *combined, size_t combined_len,
                             const hu_channel_history_entry_t *entries, size_t entry_count,
                             const char *situation, hu_director_result_t *result) {
#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)alloc;
    (void)entries;
    (void)entry_count;
    (void)situation;
    memset(result, 0, sizeof(*result));
    result->action = DIR_TEXT;
    result->delay_s = 3;
    (void)snprintf(result->direction, sizeof(result->direction), "test director: casual short");
    if (combined && combined_len > 0 &&
        hu_conversation_is_media_message(combined, combined_len, NULL, 0)) {
        result->action = DIR_TAPBACK;
        result->reaction = HU_REACTION_HEART;
        result->delay_s = 1;
        return true;
    }
    if (combined && combined_len <= 4) {
        /* Standalone "k"/"ok" style ack → tapback in production director rubric */
        bool short_ack = true;
        for (size_t i = 0; i < combined_len; i++) {
            unsigned char c = (unsigned char)combined[i];
            if (c != 'k' && c != 'K' && c != 'o' && c != 'O' && c != '\n' && c != '\r')
                short_ack = false;
        }
        if (short_ack && combined_len >= 1) {
            result->action = DIR_TAPBACK;
            result->reaction = HU_REACTION_THUMBS_UP;
            result->delay_s = 1;
        }
    }
    return true;
#else
    memset(result, 0, sizeof(*result));
    if (!g_classify_provider_ok || !g_classify_provider.vtable ||
        !g_classify_provider.vtable->chat_with_system)
        return false;

    char user_buf[2048];
    size_t pos = 0;
    static const char hdr[] = "Recent thread:\n";
    memcpy(user_buf, hdr, sizeof(hdr) - 1);
    pos = sizeof(hdr) - 1;

    size_t start = entry_count > 5 ? entry_count - 5 : 0;
    for (size_t i = start; i < entry_count; i++) {
        const char *who = entries[i].from_me ? "Seth" : "Them";
        int w = snprintf(user_buf + pos, sizeof(user_buf) - pos, "%s: %s\n", who, entries[i].text);
        if (w > 0 && pos + (size_t)w < sizeof(user_buf))
            pos += (size_t)w;
    }
    {
        int w = snprintf(user_buf + pos, sizeof(user_buf) - pos, "\nNew message from them:\n%.*s",
                         (int)(combined_len > 500 ? 500 : combined_len), combined);
        if (w > 0 && pos + (size_t)w < sizeof(user_buf))
            pos += (size_t)w;
    }
    if (situation && situation[0]) { /* what is possible this turn (Phase 2) */
        int w = snprintf(user_buf + pos, sizeof(user_buf) - pos, "\n\n%s", situation);
        if (w > 0 && pos + (size_t)w < sizeof(user_buf))
            pos += (size_t)w;
    }

    /* Heap, per call: the director can run for several contacts at once. */
    size_t sys_cap =
        sizeof(k_director_system) + sizeof(k_director_examples) + sizeof(k_director_forms);
    char *sys_prompt = alloc->alloc(alloc->ctx, sys_cap);
    if (!sys_prompt)
        return false;
    size_t sys_len = hu_daemon_director_system_prompt(sys_prompt, sys_cap);
    char *raw = NULL;
    size_t raw_len = 0;
    hu_error_t err = sys_len == 0
                         ? HU_ERR_INTERNAL
                         : g_classify_provider.vtable->chat_with_system(
                               g_classify_provider.ctx, alloc, sys_prompt, sys_len, user_buf, pos,
                               g_classify_model, g_classify_model_len, 0.4, &raw, &raw_len);
    alloc->free(alloc->ctx, sys_prompt, sys_cap);

    if (err != HU_OK || !raw || raw_len == 0 || raw_len > 500) {
        if (raw)
            alloc->free(alloc->ctx, raw, raw_len + 1);
        return false;
    }

    hu_daemon_parse_director_result(raw, raw_len, result);

    hu_log_info("director", NULL, "meta: action=%s form=%s delay=%us reaction=%d burst=%d dir=%s",
                result->action == DIR_TAPBACK   ? "tapback"
                : result->action == DIR_SILENCE ? "silence"
                                                : "text",
                hu_director_form_name(result->form), result->delay_s, (int)result->reaction,
                result->burst, result->direction[0] ? result->direction : "(none)");

    alloc->free(alloc->ctx, raw, raw_len + 1);
    return true;
#endif
}

/* F27: Classify our response type for comfort pattern learning.
 * Heuristic: haha/lol/joke -> distraction; sorry/i understand/that sucks -> empathy;
 * very short (<20 chars) -> space; you should/try this/maybe -> advice; default empathy. */
void hu_daemon_classify_comfort_response_type(const char *response, size_t response_len,
                                              char *out_type, size_t out_cap) {
    if (!response || !out_type || out_cap < 8)
        return;
    out_type[0] = '\0';
    if (response_len < 20) {
        snprintf(out_type, out_cap, "space");
        return;
    }
    char lower[256];
    size_t copy = response_len < sizeof(lower) - 1 ? response_len : sizeof(lower) - 1;
    for (size_t i = 0; i < copy; i++) {
        char c = response[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    lower[copy] = '\0';
    if (strstr(lower, "haha") || strstr(lower, "lol") || strstr(lower, "hah ") ||
        strstr(lower, " joke") || strstr(lower, "funny")) {
        snprintf(out_type, out_cap, "distraction");
        return;
    }
    if (strstr(lower, "you should") || strstr(lower, "try this") || strstr(lower, "maybe ") ||
        strstr(lower, "have you tried") || strstr(lower, "i'd suggest")) {
        snprintf(out_type, out_cap, "advice");
        return;
    }
    if (strstr(lower, "sorry") || strstr(lower, "i understand") || strstr(lower, "that sucks") ||
        strstr(lower, "i hear you") || strstr(lower, "that must be")) {
        snprintf(out_type, out_cap, "empathy");
        return;
    }
    snprintf(out_type, out_cap, "empathy");
}

/* ── G6 director-echo guard wiring ─────────────────────────────────────
 * Contract, ownership rules and call order: include/human/daemon/director.h.
 * Extracted from daemon.c's batch loop 2026-09-21 so the prompt injection
 * and the guard arming that must accompany it live in one place. */

void hu_daemon_director_arm_guard(hu_allocator_t *alloc, hu_agent_t *agent,
                                  const hu_director_result_t *result, char **convo_ctx,
                                  size_t *convo_ctx_len) {
    if (!alloc || !agent || !result || !convo_ctx || !convo_ctx_len)
        return;
    if (result->direction[0] == '\0')
        return;

    size_t dn_len = strlen(result->direction);

    /* Idempotent per turn. daemon.c's batch loop is a `do { } while (1)`
     * with five paths that `continue` back to the top for another provider
     * call (local->cloud fallback, ai-tell, quality, turing and llm-judge
     * retries), and it builds `convo_ctx` ONCE above that loop. Arming per
     * iteration therefore appended to the already-appended buffer, so a
     * retried turn carried two "this message only" blocks and a second
     * retry three. G6 is already armed with this exact direction and the
     * text is already in the prompt, so there is nothing left to do;
     * hu_daemon_director_end_turn drops the arming, which is what lets the
     * NEXT turn inject again. */
    if (agent->scene_direction_text && agent->scene_direction_text_len == dn_len &&
        memcmp(agent->scene_direction_text, result->direction, dn_len) == 0)
        return;

    static const char dn_hdr[] = "\n--- Scene Direction (this message only) ---\n";
    static const char dn_tail[] = "\n";
    size_t old_len = *convo_ctx_len;
    size_t new_len = old_len + sizeof(dn_hdr) - 1 + dn_len + sizeof(dn_tail) - 1 + 1;
    char *new_convo = (char *)alloc->alloc(alloc->ctx, new_len);
    if (!new_convo)
        return; /* direction never reached the prompt — leave G6 disarmed */

    if (*convo_ctx && old_len > 0)
        memcpy(new_convo, *convo_ctx, old_len);
    memcpy(new_convo + old_len, dn_hdr, sizeof(dn_hdr) - 1);
    memcpy(new_convo + old_len + sizeof(dn_hdr) - 1, result->direction, dn_len);
    memcpy(new_convo + old_len + sizeof(dn_hdr) - 1 + dn_len, dn_tail, sizeof(dn_tail) - 1);
    new_convo[new_len - 1] = '\0';
    alloc->free(alloc->ctx, *convo_ctx, old_len + 1);
    *convo_ctx = new_convo;
    *convo_ctx_len = new_len - 1;
    agent->conversation_context = new_convo;
    agent->conversation_context_len = new_len - 1;

    /* Arm G6. The agent borrows a const pointer into the caller's
     * `result->direction`; hu_daemon_director_end_turn drops it. */
    hu_agent_internal_set_scene_direction(agent, result->direction, dn_len);
}

void hu_daemon_director_end_turn(hu_agent_t *agent) {
    if (!agent)
        return;
    /* Push BEFORE clearing — the push copies onto agent->alloc, so the
     * ring survives the daemon's stack buffer going out of scope and G6
     * can still catch a cross-turn echo on the next turn. */
    hu_agent_internal_push_director_history(agent, agent->scene_direction_text,
                                            agent->scene_direction_text_len);
    hu_agent_internal_clear_scene_direction(agent);
}

void hu_daemon_director_contact_boundary(hu_agent_t *agent, const char *key, size_t key_len) {
    /* The daemon drives one agent across every contact, so this is the
     * only place the cross-contact carry is broken. Static last-key state
     * is safe here for the same reason g_classify_provider above is: the
     * poll loop that calls this is single-threaded. */
    static char last_key[320];
    static size_t last_key_len = 0;

    if (!agent)
        return;
    if (key && key_len > 0 && key_len == last_key_len && memcmp(last_key, key, key_len) == 0)
        return; /* same contact — keep multi-turn director history */

    hu_agent_internal_reset_contact_boundary_state(agent);

    if (key && key_len > 0 && key_len < sizeof(last_key)) {
        memcpy(last_key, key, key_len);
        last_key_len = key_len;
    } else {
        /* Unknown or oversized key: record nothing, so the next batch
         * also resets. Conservative — never carries state it can't prove. */
        last_key_len = 0;
    }
}
