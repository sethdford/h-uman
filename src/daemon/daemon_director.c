#include "human/agent.h"
#include "human/channel.h"
#include "human/cognition/emotional.h"
#include "human/context/conversation.h"
#include "human/core/error.h"
#include "human/core/log.h"
#include "human/core/string.h"
#include "human/daemon/common.h"
#include "human/daemon/director.h"
#include "human/daemon_comfort_summary.h"
#include "human/memory.h"
#include "human/memory/deep_extract.h"
#include "human/provider.h"

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
const char *g_classify_model = "gemini-3.1-flash-lite-preview";
size_t g_classify_model_len = 29;

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
    if (strncmp(val, "tapback", 7) == 0)
        out->action = DIR_TAPBACK;
    else if (strncmp(val, "silence", 7) == 0)
        out->action = DIR_SILENCE;

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
                             hu_director_result_t *result) {
#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)alloc;
    (void)entries;
    (void)entry_count;
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

    static const char director_system[] =
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
        "- CRITICAL: The `direction:` field describes TONE + PACING only. NEVER invent "
        "specific reasons, activities, people, places, or topics that aren't visible in the "
        "Recent thread. Forbidden: 'because he's getting back to the drink', 'mention the "
        "cat', 'reference yesterday's meeting'. Allowed: 'short empathetic reaction', "
        "'casual, match their energy', 'busy tone, one-word reply'. The downstream agent "
        "treats your `direction` text as ground truth and will paste invented specifics "
        "into the actual reply, which manifests as cross-contact bleed (US-16, Mindy "
        "diagnostic 2026-05-26).\n\n"
        "Examples:\n"
        "action:text|delay_s:3|direction:Short empathetic reaction, 5 words max\n"
        "action:text|delay_s:2|direction:Casual greeting back, match their energy\n"
        "action:tapback|reaction:heart (they sent a photo)\n"
        "action:tapback|reaction:haha (they said something funny)\n"
        "action:tapback|reaction:thumbs_up (simple acknowledgment)\n"
        "action:text|delay_s:2|burst:true|direction:Match urgency, 3 rapid messages\n"
        "action:text|delay_s:45|direction:He's busy, one-word reply when he gets back";

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

    char *raw = NULL;
    size_t raw_len = 0;
    hu_error_t err = g_classify_provider.vtable->chat_with_system(
        g_classify_provider.ctx, alloc, director_system, sizeof(director_system) - 1, user_buf, pos,
        g_classify_model, g_classify_model_len, 0.4, &raw, &raw_len);

    if (err != HU_OK || !raw || raw_len == 0 || raw_len > 500) {
        if (raw)
            alloc->free(alloc->ctx, raw, raw_len + 1);
        return false;
    }

    hu_daemon_parse_director_result(raw, raw_len, result);

    hu_log_info("director", NULL, "meta: action=%s delay=%us reaction=%d burst=%d dir=%s",
                result->action == DIR_TAPBACK   ? "tapback"
                : result->action == DIR_SILENCE ? "silence"
                                                : "text",
                result->delay_s, (int)result->reaction, result->burst,
                result->direction[0] ? result->direction : "(none)");

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
