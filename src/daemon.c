#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
/* Core daemon headers */
#include "human/daemon.h"
#include "human/agent.h"
#include "human/channel.h"
#include "human/config.h"
#include "human/core/error.h"
#include "human/core/log.h"
#include "human/core/process_util.h"
#include "human/core/string.h"
#include "human/humanness.h"

/* Subsystem facades — each aggregates related implementation headers */
#include "human/agent/autodream.h"
#include "human/agent/kv_cache.h"
#include "human/agent/lora_runner.h"
#ifdef HU_ENABLE_RL_FULL
#include "human/eval/eval_gate.h"
#include "human/eval/leaderboard.h"
#endif
#include "human/agent/training_data_runner.h"
#include "human/agent/verifier_metrics.h"
#include "human/agent/world_model_bridge.h"
#include "human/ml/learner.h"
#include "human/ml/learner_bridge.h"
#include "human/ml/lora_retrain_runner.h"
#ifdef HU_ENABLE_MOLORA
#include "human/ml/molora.h"
#endif
#ifdef HU_ENABLE_ML
#include "human/ml/m3_frontier_adapter.h"
#endif
#include "human/agent/choreography.h"
#include "human/daemon/agent_facade.h"
#include "human/daemon/context_facade.h"
#include "human/daemon/feeds_facade.h"
#include "human/daemon/intelligence_facade.h"
#include "human/daemon/memory_facade.h"
#include "human/daemon/ml_facade.h"
#include "human/daemon/persona_facade.h"
#include "human/daemon/platform_facade.h"
#include "human/daemon/voice_facade.h"

/* Channel helpers */
#include "human/channels/channel_embed.h"
/* Provider factory for classify provider creation */
#include "human/providers/factory.h"
#ifdef HU_ENABLE_APPLE_INTELLIGENCE
#include "human/providers/apple.h"
#endif

/* Plan 2: Background observer registry */
#include "human/background_observer.h"

/* Security */
#include "human/security/adversarial.h"
#include "human/security/companion_safety.h"
#include "human/security/keystore.h"
#include "human/security/moderation.h"

/* Music preview integration */
#include "human/music.h"
#include "human/persona/persona_deltas.h"
/* Proactive image generation */
#include "human/tools/image_gen.h"
/* Daemon modules */
#include "human/daemon_cron.h"
#include "human/daemon_lifecycle.h"
#include "human/daemon_proactive.h"
#include "human/daemon_routing.h"
/* reaction_handler.h pulled out of the RL_FULL gate: the
 * personal-model sink (Phase 1c of docs/plans/2026-05-18-imessage-sota.md)
 * runs regardless of RL_FULL, and the header's declarations are gate-free.
 * The other three headers below remain gated because their call sites are
 * gated. */
#include "human/agent/reaction_handler.h"
#if defined(HU_ENABLE_RL_FULL)
#include "human/channels/imessage.h"
#include "human/channels/imessage_reactions.h"
#include "human/daemon_reaction_poll.h"
#endif

/* follow_up.h must be included unconditionally — the read-receipt watcher
 * scheduling block at L~1259 uses hu_followup_dedup_t / hu_followup_decide
 * regardless of HU_ENABLE_RL_FULL. Previously grouped inside the RL_FULL
 * block, which caused a build failure on the `human` target (human_core
 * doesn't get HU_ENABLE_RL_FULL); the test target masked it via different
 * defines. Pinned by the daemon rebuild path. */
#include "human/follow_up.h"

#include "human/agent/governor.h"
#include "human/agent/output_validator_chain.h"
#include "human/agent/proactive.h"
#include "human/agent/proactive_ext.h"
#include "human/agent/proactive_throttle.h"
#include "human/agent/validators/builtin.h"
#include "human/context/self_awareness.h"
#include "human/observability/validator_telemetry.h"
#ifdef HU_HAS_CRON
#include "human/cron.h"
#include "human/crontab.h"
#endif

/* Forward declaration — avoids type conflict with context/style_tracker.h */
hu_error_t hu_style_clone_from_history(hu_allocator_t *alloc, const char **own_messages,
                                       size_t own_msg_count, char **prompt_out, size_t *prompt_len);
#include <ctype.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* sqlite3.h no longer needed in daemon — moved to channel vtable implementations */

#if !defined(_WIN32) && !defined(__CYGWIN__)
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define HU_DAEMON_PID_DIR  ".human"
#define HU_DAEMON_PID_FILE "human.pid"
#define HU_MAX_PATH        1024

/* Lightweight classification provider (e.g. Gemini Flash Lite) for hybrid routing.
 * When llm_decides is active, the primary agent turn uses the local model while
 * classification/scoring calls use this fast cloud provider.
 * Only meaningful in production — test builds early-return before these are accessed. */
#if !(defined(HU_IS_TEST) && HU_IS_TEST)
static hu_provider_t g_classify_provider;
static bool g_classify_provider_ok = false;
static const char *g_classify_model = "gemini-3.1-flash-lite-preview";
static size_t g_classify_model_len = 29;
#endif

/* Emotion detection: test builds use heuristic-only (no LLM), production uses hybrid
 * routing via g_classify_provider when available. May appear unreferenced in test builds
 * because some call sites are behind #ifndef HU_IS_TEST. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((unused))
#endif
/* W9: real-time emotion detection stays here (per-message, from live
 * history) while the world model caches a snapshot. The two compose:
 * this function feeds live state, the world model feeds trend. */
static hu_emotional_state_t hu_daemon_detect_emotion(hu_allocator_t *alloc, hu_agent_t *agent,
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

/* ── Director meta-behavior result ─────────────────────────────────────── */
typedef enum { DIR_TEXT = 0, DIR_TAPBACK, DIR_SILENCE } hu_director_action_t;

typedef struct {
    hu_director_action_t action;
    uint32_t delay_s;
    hu_reaction_type_t reaction;
    bool burst;
    char direction[512];
} hu_director_result_t;

#if defined(__GNUC__) || defined(__clang__)
__attribute__((unused))
#endif
static void hu_daemon_parse_director_result(const char *raw, size_t len,
                                            hu_director_result_t *out) {
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
static bool hu_daemon_director_call(hu_allocator_t *alloc, const char *combined,
                                    size_t combined_len, const hu_channel_history_entry_t *entries,
                                    size_t entry_count, hu_director_result_t *result) {
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
        "memories. Deflect: 'dude idk, that was forever ago'.\n\n"
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

/* Per-channel daemon.* config for the active messaging channel.
 *
 * To add a channel: ensure hu_<channel>_channel_config_t in config.h has a `daemon` field,
 * then add one row to k_daemon_configs[] with the vtable `name()` string and the combined
 * offset (offsetof(hu_channels_config_t, <member>) + offsetof(hu_*_channel_config_t, daemon)).
 */
typedef struct {
    const char *name;
    size_t daemon_offset; /* bytes from &config->channels to that channel's daemon struct */
} hu_daemon_config_entry_t;

static const hu_daemon_config_entry_t k_daemon_configs[] = {
    {"discord",
     offsetof(hu_channels_config_t, discord) + offsetof(hu_discord_channel_config_t, daemon)},
    {"email", offsetof(hu_channels_config_t, email) + offsetof(hu_email_channel_config_t, daemon)},
    {"gmail", offsetof(hu_channels_config_t, gmail) + offsetof(hu_gmail_channel_config_t, daemon)},
    {"imessage",
     offsetof(hu_channels_config_t, imessage) + offsetof(hu_imessage_channel_config_t, daemon)},
    {"irc", offsetof(hu_channels_config_t, irc) + offsetof(hu_irc_channel_config_t, daemon)},
    {"matrix",
     offsetof(hu_channels_config_t, matrix) + offsetof(hu_matrix_channel_config_t, daemon)},
    {"nostr", offsetof(hu_channels_config_t, nostr) + offsetof(hu_nostr_channel_config_t, daemon)},
    {"signal",
     offsetof(hu_channels_config_t, signal) + offsetof(hu_signal_channel_config_t, daemon)},
    {"slack", offsetof(hu_channels_config_t, slack) + offsetof(hu_slack_channel_config_t, daemon)},
    {"telegram",
     offsetof(hu_channels_config_t, telegram) + offsetof(hu_telegram_channel_config_t, daemon)},
    {"whatsapp",
     offsetof(hu_channels_config_t, whatsapp) + offsetof(hu_whatsapp_channel_config_t, daemon)},
};

static const hu_channel_daemon_config_t *get_active_daemon_config(const hu_config_t *config,
                                                                  const char *ch_name) {
    if (!config)
        return NULL;
    if (!ch_name)
        return &config->channels.default_daemon;
    for (size_t i = 0; i < sizeof(k_daemon_configs) / sizeof(k_daemon_configs[0]); i++) {
        if (strcmp(ch_name, k_daemon_configs[i].name) == 0) {
            const char *base = (const char *)&config->channels;
            return (const hu_channel_daemon_config_t *)(void *)(base +
                                                                k_daemon_configs[i].daemon_offset);
        }
    }
    return &config->channels.default_daemon;
}

#ifdef HU_IS_TEST
const hu_channel_daemon_config_t *hu_daemon_test_get_active_daemon_config(const hu_config_t *config,
                                                                          const char *ch_name) {
    return get_active_daemon_config(config, ch_name);
}
#endif

#ifndef HU_IS_TEST
/* F119: Proactive volume governor — shared between check-in cycle and inbound handler */
static hu_proactive_budget_t gov_budget;
static bool gov_budget_inited;

/* Proactive-style budget for outbound visual attachments (separate from check-in governor). */
static hu_proactive_budget_t hu_daemon_visual_attach_gov;
static bool hu_daemon_visual_attach_gov_init;

static bool hu_daemon_visual_attach_gov_allow(uint64_t now_ms) {
    if (!hu_daemon_visual_attach_gov_init) {
        hu_proactive_budget_config_t cfg = {
            .daily_max = 4,
            .weekly_max = 14,
            .relationship_multiplier = 1.0,
            .cool_off_after_unanswered = 255,
            .cool_off_hours = 0,
        };
        hu_governor_init(&cfg, &hu_daemon_visual_attach_gov);
        hu_daemon_visual_attach_gov_init = true;
    }
    return hu_governor_has_budget(&hu_daemon_visual_attach_gov, now_ms);
}

static void hu_daemon_visual_attach_gov_after_send(uint64_t now_ms) {
    if (hu_daemon_visual_attach_gov_init)
        (void)hu_governor_record_sent(&hu_daemon_visual_attach_gov, now_ms);
}
#endif /* !HU_IS_TEST */

#if defined(HU_ENABLE_SQLITE) && !defined(HU_IS_TEST)
static void cross_channel_format_when(char *out, size_t out_sz, const char *ts) {
    if (!out || out_sz == 0)
        return;
    out[0] = '\0';
    if (!ts || !ts[0]) {
        if (out_sz >= 7)
            memcpy(out, "recent", 7);
        return;
    }
    char ts_work[48];
    size_t tl = strlen(ts);
    if (tl >= sizeof(ts_work))
        tl = sizeof(ts_work) - 1;
    memcpy(ts_work, ts, tl);
    ts_work[tl] = '\0';

    struct tm tm_buf;
    memset(&tm_buf, 0, sizeof(tm_buf));
    static const char *const fmts[] = {"%Y-%m-%dT%H:%M:%S", "%Y-%m-%d %H:%M", NULL};
    time_t msg_t = (time_t)-1;
    for (int fi = 0; fmts[fi]; fi++) {
        memset(&tm_buf, 0, sizeof(tm_buf));
        if (strptime(ts_work, fmts[fi], &tm_buf)) {
            msg_t = mktime(&tm_buf);
            if (msg_t != (time_t)-1)
                break;
        }
    }
    if (msg_t == (time_t)-1) {
        (void)snprintf(out, out_sz, "%s", ts_work);
        return;
    }
    time_t now = time(NULL);
    double diff = difftime(now, msg_t);
    if (diff < 60.0)
        (void)snprintf(out, out_sz, "just now");
    else if (diff < 3600.0)
        (void)snprintf(out, out_sz, "%dm ago", (int)(diff / 60.0));
    else if (diff < 86400.0)
        (void)snprintf(out, out_sz, "%dh ago", (int)(diff / 3600.0));
    else if (diff < 86400.0 * 7.0)
        (void)snprintf(out, out_sz, "%dd ago", (int)(diff / 86400.0));
    else
        (void)snprintf(out, out_sz, "%.10s", ts_work);
}

static void cross_channel_platform_label(const char *plat, char *out, size_t out_sz) {
    if (!plat || !out || out_sz < 2) {
        if (out && out_sz)
            out[0] = '\0';
        return;
    }
    size_t i = 0;
    for (; plat[i] && i + 1 < out_sz; i++) {
        if (i == 0 && plat[i] >= 'a' && plat[i] <= 'z')
            out[i] = (char)(plat[i] - 'a' + 'A');
        else
            out[i] = plat[i];
    }
    out[i] = '\0';
}

static bool daemon_cross_ctx_append_line(hu_allocator_t *alloc, char **buf, size_t *buf_len,
                                         const char *line, size_t line_len) {
    if (!alloc || !buf || !buf_len || !line || line_len == 0)
        return true;
    size_t new_len = *buf_len ? *buf_len + 1 + line_len : line_len;
    char *n = (char *)alloc->alloc(alloc->ctx, new_len + 1);
    if (!n)
        return false;
    if (*buf && *buf_len > 0) {
        memcpy(n, *buf, *buf_len);
        n[*buf_len] = '\n';
        memcpy(n + *buf_len + 1, line, line_len);
        n[new_len] = '\0';
        alloc->free(alloc->ctx, *buf, *buf_len + 1);
    } else {
        memcpy(n, line, line_len);
        n[line_len] = '\0';
        new_len = line_len;
    }
    *buf = n;
    *buf_len = new_len;
    return true;
}
#endif /* HU_ENABLE_SQLITE && !HU_IS_TEST */

/* GCC's warn_unused_result is not suppressed by (void) casts */
#define HU_IGNORE_RESULT(expr)   \
    do {                         \
        if ((expr)) { /* noop */ \
        }                        \
    } while (0)

/* build_callback_context — now in daemon_proactive.c as hu_daemon_build_callback_context */
#define build_callback_context hu_daemon_build_callback_context

#ifndef HU_IS_TEST
/* F27: Classify our response type for comfort pattern learning.
 * Heuristic: haha/lol/joke -> distraction; sorry/i understand/that sucks -> empathy;
 * very short (<20 chars) -> space; you should/try this/maybe -> advice; default empathy. */
static void classify_comfort_response_type(const char *response, size_t response_len,
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

#ifdef HU_ENABLE_SQLITE
/* F23: Extract significant topic keywords from user text and record baselines.
 * Skips stopwords, records each significant word (3–32 chars) via topic_baselines. */
#define HU_DAEMON_TOPIC_BASELINE_MAX 8
#define HU_DAEMON_TOPIC_BUF          32

static void record_topic_baselines_from_text(hu_memory_t *memory, const char *contact_id,
                                             size_t contact_id_len, const char *text,
                                             size_t text_len) {
    if (!memory || !contact_id || contact_id_len == 0 || !text || text_len == 0)
        return;
    static const char *const stop[] = {
        "i",     "the",   "a",      "is",    "was", "that", "this", "it",    "to",  "and",  "but",
        "so",    "just",  "really", "what",  "how", "why",  "when", "where", "who", "can",  "will",
        "would", "could", "should", "have",  "has", "had",  "do",   "does",  "did", "am",   "are",
        "were",  "be",    "been",   "being", "of",  "in",   "on",   "at",    "for", "with", "about",
        "from",  "as",    "or",     "if",    "not", "no",   "yes",  "oh",    "um",  "like", NULL,
    };
    char topics[HU_DAEMON_TOPIC_BASELINE_MAX][HU_DAEMON_TOPIC_BUF];
    size_t topic_count = 0;
    memset(topics, 0, sizeof(topics));

    const char *p = text;
    const char *end = text + text_len;
    while (p < end && topic_count < HU_DAEMON_TOPIC_BASELINE_MAX) {
        while (p < end && !isalnum((unsigned char)*p))
            p++;
        if (p >= end)
            break;
        const char *start = p;
        while (p < end && (isalnum((unsigned char)*p) || *p == '\'' || *p == '-'))
            p++;
        size_t wlen = (size_t)(p - start);
        if (wlen < 3 || wlen >= HU_DAEMON_TOPIC_BUF - 1)
            continue;
        bool is_stop = false;
        for (const char *const *sw = stop; *sw; sw++) {
            size_t swlen = strlen(*sw);
            if (wlen == swlen && strncasecmp(start, *sw, wlen) == 0) {
                is_stop = true;
                break;
            }
        }
        if (is_stop)
            continue;
        /* Dedupe */
        bool dup = false;
        for (size_t k = 0; k < topic_count; k++) {
            if (strncasecmp(topics[k], start, wlen) == 0 && topics[k][wlen] == '\0') {
                dup = true;
                break;
            }
        }
        if (dup)
            continue;
        memcpy(topics[topic_count], start, wlen);
        topics[topic_count][wlen] = '\0';
        for (size_t i = 0; i < wlen; i++)
            topics[topic_count][i] = (char)tolower((unsigned char)topics[topic_count][i]);
        (void)hu_superhuman_topic_baseline_record(memory, contact_id, contact_id_len,
                                                  topics[topic_count], wlen);
        topic_count++;
    }
}
#endif

/* F27: Score engagement from their reply. reply_len>20 + positive words -> 0.8;
 * brief thanks -> 0.4; very short -> 0.2. */
static float score_comfort_engagement(const char *reply, size_t reply_len) {
    if (!reply)
        return 0.2f;
    if (reply_len <= 5)
        return 0.2f;
    char lower[256];
    size_t copy = reply_len < sizeof(lower) - 1 ? reply_len : sizeof(lower) - 1;
    for (size_t i = 0; i < copy; i++) {
        char c = reply[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    lower[copy] = '\0';
    if (strstr(lower, "thanks") || strstr(lower, "thank you") || strstr(lower, "ty ") ||
        strstr(lower, "thx")) {
        if (reply_len > 20)
            return 0.8f;
        return 0.4f;
    }
    if (strstr(lower, "yes") || strstr(lower, "yeah") || strstr(lower, "that helped") ||
        strstr(lower, "good point") || strstr(lower, "makes sense")) {
        return 0.8f;
    }
    if (reply_len > 20)
        return 0.6f;
    return 0.3f;
}
#endif

#ifndef HU_IS_TEST
/* Store a conversation summary as long-term memory.
 * Concatenates the user message and agent response, runs deep-extract
 * on the full exchange, and stores extracted facts scoped to the contact.
 * When graph is non-NULL, also upserts facts and relations into the GraphRAG knowledge graph.
 * agent may be NULL; when non-NULL and bth_metrics set, increments facts_extracted. */
static void store_conversation_summary(hu_allocator_t *alloc, hu_memory_t *memory,
                                       hu_graph_t *graph, hu_agent_t *agent, const char *session_id,
                                       size_t session_id_len, const char *user_msg,
                                       size_t user_msg_len, const char *response,
                                       size_t response_len) {
    if (!alloc || !memory || !memory->vtable || !memory->vtable->store)
        return;
    if (!user_msg || user_msg_len == 0)
        return;
#ifndef HU_ENABLE_SQLITE
    (void)graph;
#endif

    /* Build "them: ... | me: ..." for richer extraction context */
    if (response_len > SIZE_MAX - user_msg_len)
        return;
    if (user_msg_len + response_len > SIZE_MAX - 17)
        return;
    size_t total = user_msg_len + response_len + 16;
    char *combined = (char *)alloc->alloc(alloc->ctx, total + 1);
    if (!combined)
        return;
    int n = snprintf(combined, total + 1, "them: %.*s | me: %.*s", (int)user_msg_len, user_msg,
                     (int)response_len, response);
    if (n <= 0) {
        alloc->free(alloc->ctx, combined, total + 1);
        return;
    }
    size_t combined_len = (size_t)n < total ? (size_t)n : total;

    hu_deep_extract_result_t de;
    memset(&de, 0, sizeof(de));
    hu_error_t err = hu_deep_extract_lightweight(alloc, combined, combined_len, &de);
    if (err == HU_OK && de.fact_count > 0) {
        if (agent && agent->bth_metrics)
            agent->bth_metrics->facts_extracted += de.fact_count;
        static const char cat_name[] = "conversation_summary";
        hu_memory_category_t cat = {
            .tag = HU_MEMORY_CATEGORY_CUSTOM,
            .data.custom = {.name = cat_name, .name_len = sizeof(cat_name) - 1},
        };
        for (size_t i = 0; i < de.fact_count; i++) {
            const hu_extracted_fact_t *f = &de.facts[i];
            if (!f->subject || !f->predicate || !f->object)
                continue;
            char key_buf[256];
            int kn =
                snprintf(key_buf, sizeof(key_buf), "%s:%s:%s", f->subject, f->predicate, f->object);
            if (kn > 0 && (size_t)kn < sizeof(key_buf)) {
                (void)memory->vtable->store(memory->ctx, key_buf, (size_t)kn, f->object,
                                            strlen(f->object), &cat, session_id ? session_id : "",
                                            session_id ? session_id_len : 0);
            }
#ifdef HU_ENABLE_SQLITE
            if (graph) {
                int64_t src_id = 0;
                int64_t tgt_id = 0;
                size_t subj_len = strlen(f->subject);
                size_t obj_len = strlen(f->object);
                hu_relation_type_t rel_type =
                    hu_relation_type_from_string(f->predicate, strlen(f->predicate));
                if (hu_graph_upsert_entity(graph, session_id, session_id_len, f->subject, subj_len,
                                           HU_ENTITY_UNKNOWN, NULL, &src_id) == HU_OK &&
                    hu_graph_upsert_entity(graph, session_id, session_id_len, f->object, obj_len,
                                           HU_ENTITY_UNKNOWN, NULL, &tgt_id) == HU_OK) {
                    hu_error_t rel_err =
                        hu_graph_upsert_relation(graph, session_id, session_id_len, src_id, tgt_id,
                                                 rel_type, 1.0f, f->object, obj_len);
                    if (rel_err != HU_OK)
                        hu_log_error("daemon", agent ? agent->observer : NULL,
                                     "graph: relation upsert failed: %s", hu_error_string(rel_err));
                }
            }
#endif
        }
    }
#ifdef HU_ENABLE_SQLITE
    if (graph && err == HU_OK && de.relation_count > 0) {
        for (size_t i = 0; i < de.relation_count; i++) {
            const hu_extracted_relation_t *r = &de.relations[i];
            if (!r->entity_a || !r->relation || !r->entity_b)
                continue;
            int64_t src_id = 0;
            int64_t tgt_id = 0;
            size_t a_len = strlen(r->entity_a);
            size_t b_len = strlen(r->entity_b);
            size_t rel_len = strlen(r->relation);
            hu_relation_type_t rel_type = hu_relation_type_from_string(r->relation, rel_len);
            if (hu_graph_upsert_entity(graph, session_id, session_id_len, r->entity_a, a_len,
                                       HU_ENTITY_UNKNOWN, NULL, &src_id) == HU_OK &&
                hu_graph_upsert_entity(graph, session_id, session_id_len, r->entity_b, b_len,
                                       HU_ENTITY_UNKNOWN, NULL, &tgt_id) == HU_OK) {
                hu_error_t rel_err =
                    hu_graph_upsert_relation(graph, session_id, session_id_len, src_id, tgt_id,
                                             rel_type, 1.0f, r->entity_b, b_len);
                if (rel_err != HU_OK)
                    hu_log_error("daemon", agent ? agent->observer : NULL,
                                 "graph: relation upsert failed: %s", hu_error_string(rel_err));
            }
        }
    }
#endif
    hu_deep_extract_result_deinit(&de, alloc);
    alloc->free(alloc->ctx, combined, total + 1);
}
#endif /* !HU_IS_TEST */

/* validate_home and get_pid_path moved to daemon_lifecycle.c */

/* Cron field parsing, schedule matching, and tick execution moved to daemon_cron.c.
 * Public API: hu_cron_schedule_matches (daemon.h), hu_service_run_agent_cron (daemon.h).
 * Internal: hu_daemon_cron_tick (daemon_cron.h). */

/* ── Per-contact trust state (thread-safe, LRU-evicted) ─────────────── */

/* TRUST-006: Per-contact trust state tracking
 * Expanded from 256 to 4096 with LRU eviction and mutex for thread safety. */
#define HU_DAEMON_TRUST_CAP 4096

static hu_daemon_contact_trust_t g_contact_trust[HU_DAEMON_TRUST_CAP];
static size_t g_contact_trust_count;

#if !defined(_WIN32) && !defined(__CYGWIN__)
static pthread_mutex_t g_trust_mutex = PTHREAD_MUTEX_INITIALIZER;
#define TRUST_LOCK()   pthread_mutex_lock(&g_trust_mutex)
#define TRUST_UNLOCK() pthread_mutex_unlock(&g_trust_mutex)
#else
#define TRUST_LOCK()   ((void)0)
#define TRUST_UNLOCK() ((void)0)
#endif

static hu_error_t trust_find_or_create_slot(const char *contact_id, size_t cid_len,
                                            size_t *slot_out) {
    if (!contact_id || cid_len == 0 || !slot_out)
        return HU_ERR_INVALID_ARGUMENT;

    for (size_t i = 0; i < g_contact_trust_count; i++) {
        if (strlen(g_contact_trust[i].contact_id) == cid_len &&
            memcmp(g_contact_trust[i].contact_id, contact_id, cid_len) == 0) {
            *slot_out = i;
            return HU_OK;
        }
    }

    size_t slot;
    if (g_contact_trust_count < HU_DAEMON_TRUST_CAP) {
        slot = g_contact_trust_count++;
    } else {
        slot = 0;
        int64_t oldest = g_contact_trust[0].state.last_updated_at;
        for (size_t i = 1; i < g_contact_trust_count; i++) {
            if (g_contact_trust[i].state.last_updated_at < oldest) {
                oldest = g_contact_trust[i].state.last_updated_at;
                slot = i;
            }
        }
    }

    size_t copy_len = cid_len;
    if (copy_len >= sizeof(g_contact_trust[slot].contact_id))
        copy_len = sizeof(g_contact_trust[slot].contact_id) - 1;
    memcpy(g_contact_trust[slot].contact_id, contact_id, copy_len);
    g_contact_trust[slot].contact_id[copy_len] = '\0';
    hu_trust_init(&g_contact_trust[slot].state);
    *slot_out = slot;
    return HU_OK;
}

hu_error_t hu_daemon_get_trust_state(const char *contact_id, size_t cid_len,
                                     hu_trust_state_t *out) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    TRUST_LOCK();
    size_t slot;
    hu_error_t err = trust_find_or_create_slot(contact_id, cid_len, &slot);
    if (err == HU_OK)
        *out = g_contact_trust[slot].state;
    TRUST_UNLOCK();
    return err;
}

hu_error_t hu_daemon_set_trust_state(const char *contact_id, size_t cid_len,
                                     const hu_trust_state_t *state) {
    if (!state)
        return HU_ERR_INVALID_ARGUMENT;
    TRUST_LOCK();
    size_t slot;
    hu_error_t err = trust_find_or_create_slot(contact_id, cid_len, &slot);
    if (err == HU_OK)
        g_contact_trust[slot].state = *state;
    TRUST_UNLOCK();
    return err;
}

#ifdef HU_IS_TEST
size_t hu_daemon_trust_count(void) {
    return g_contact_trust_count;
}
void hu_daemon_trust_reset(void) {
    TRUST_LOCK();
    g_contact_trust_count = 0;
    memset(g_contact_trust, 0, sizeof(g_contact_trust));
    TRUST_UNLOCK();
}
#endif

/* ── US-7.3 — Honesty gate: LoRA adapter ignored by cloud provider ───
 *
 * Per-process one-shot. Sprint-7 decision D4 binds:
 *   - fire at most once per daemon process lifetime
 *   - test-only reset shim, no production-side suppression key
 *
 * The literal substring "personalization adapter ignored" plus the
 * provider name is what AC-7.3.1 pins. Do not change either without
 * updating tests/test_provider_all.c. */
static int s_personalization_warn_emitted = 0;

void hu_daemon_personalization_warn_adapter_ignored(struct hu_observer *observer,
                                                    const char *provider_name,
                                                    const char *adapter_id) {
    if (s_personalization_warn_emitted)
        return;
    s_personalization_warn_emitted = 1;
    const char *pname = (provider_name && *provider_name) ? provider_name : "(unknown)";
    const char *aid = (adapter_id && *adapter_id) ? adapter_id : "(unknown)";
    hu_log_warn("human", observer,
                "personalization adapter ignored: provider '%s' does not support LoRA adapters "
                "(adapter '%s' will not be applied)",
                pname, aid);
}

#ifdef HU_IS_TEST
void hu_daemon_personalization_warn_reset_for_test(void) {
    s_personalization_warn_emitted = 0;
}
#endif

/* ── Proactive check-in (utilities now in daemon_proactive.c) ──────── */

#ifndef HU_IS_TEST

/* Proactive context — shared LRU cache for contact activity tracking. */
static hu_proactive_context_t g_proactive_ctx;

/* 2026-05-16 P1-6 / P1-7 / P4-6: centralized throttle for outbound proactive
 * sends. Lazy-initialized on first send so tests that exercise the daemon
 * skeleton don't allocate. */
static hu_proactive_throttle_t g_proactive_throttle;
static int g_proactive_throttle_initialized;

static hu_proactive_throttle_t *daemon_throttle(hu_allocator_t *alloc) {
    if (!g_proactive_throttle_initialized) {
        hu_proactive_throttle_init(&g_proactive_throttle, alloc);
        g_proactive_throttle_initialized = 1;
    }
    return &g_proactive_throttle;
}

/* Compatibility aliases — old static names → public API in daemon_proactive.c */
#define daemon_contact_activity_record(cid, ch, sk) \
    hu_daemon_contact_activity_record(&g_proactive_ctx, (cid), (ch), (sk))
#define daemon_proactive_parse_route hu_daemon_proactive_parse_route
#define daemon_contact_activity_apply_route(cid, now, chs, cnt, cb, tb, tl) \
    hu_daemon_proactive_apply_route(&g_proactive_ctx, (cid), (now), (chs), (cnt), (cb), (tb), (tl))

/* proactive_prompt_for_contact — now in daemon_proactive.c */
#define proactive_prompt_for_contact hu_daemon_proactive_prompt_for_contact

void hu_service_run_proactive_checkins(hu_allocator_t *alloc, hu_agent_t *agent,
                                       hu_service_channel_t *channels, size_t channel_count) {
    if (!alloc || !agent || !agent->persona)
        return;

    /* Global kill switch — see 2026-05-16 incident.  Defaults to OFF on a
     * freshly loaded persona; operator must opt in by setting
     * "proactive": { "master_enabled": true } in the persona JSON.  This is
     * the durable in-config gate; HU_PROACTIVE_ENABLED below is the legacy
     * env override that still works as a hard runtime kill. */
    if (!hu_persona_proactive_is_enabled(agent->persona))
        return;

    /* HU_PROACTIVE_ENABLED=0 disables proactive check-ins entirely.
     * Default: enabled (any value other than "0"). */
    const char *proactive_env = getenv("HU_PROACTIVE_ENABLED");
    if (proactive_env && proactive_env[0] == '0' && proactive_env[1] == '\0')
        return;

    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    int hour = tm_now.tm_hour;

    /* Only check in during social hours (9am-9pm) */
    if (hour < 9 || hour > 21)
        return;

    /* F119 (Pillar 19): Proactive volume governor — check budget before proceeding */
    if (!gov_budget_inited) {
        hu_proactive_budget_config_t gcfg = {.daily_max = 6,
                                             .weekly_max = 15,
                                             .relationship_multiplier = 1.0,
                                             .cool_off_after_unanswered = 2,
                                             .cool_off_hours = 72};
        hu_governor_init(&gcfg, &gov_budget);
        gov_budget_inited = true;
    }
    uint64_t gov_now_ms = (uint64_t)now * 1000ULL;
    /* Escalating backoff: 72h → 144h → 288h → never, based on consecutive unanswered */
    gov_budget.cool_off_hours = hu_proactive_backoff_hours(gov_budget.unanswered_count);
    /* Reciprocity + busyness modulation of relationship multiplier */
    {
        hu_reciprocity_state_t rs = {.initiation_ratio = 0.5,
                                     .unanswered_proactive = gov_budget.unanswered_count,
                                     .contact_just_reengaged = false};
        double recip_mult = hu_reciprocity_budget_multiplier(&rs);
        hu_busyness_state_t bs = {
            .calendar_busy = false, .life_sim_stressed = false, .seed = (uint32_t)now};
        double busy_mult = hu_busyness_budget_multiplier(&bs);
        gov_budget.relationship_multiplier = 1.0 * recip_mult * busy_mult;
    }
    if (!hu_governor_has_budget(&gov_budget, gov_now_ms))
        return;

    /* F53: Deduplication — don't send same important_date to same contact twice per day.
     *
     * 2026-05-16 P1-7: the static char[8][64] ring buffer below overflowed
     * silently with 9+ contacts. It is retained ONLY for backward compatibility
     * (downstream callers still touch g_sent_important_date_count for stats).
     * The authoritative check is now hu_proactive_throttle_dedup_already_today /
     * _first_today, which is heap-backed and scales to 256 contacts. */
    static char g_sent_important_date_contacts[8][64];
    static int g_sent_important_date_count = 0;
    static int g_sent_important_date_ymd = -1;
    int today_ymd = (tm_now.tm_year + 1900) * 10000 + (tm_now.tm_mon + 1) * 100 + tm_now.tm_mday;
    if (g_sent_important_date_ymd != today_ymd) {
        g_sent_important_date_ymd = today_ymd;
        g_sent_important_date_count = 0;
    }
    /* Heap-backed dedup (replaces the [8] ring). */
    (void)daemon_throttle(alloc);
    uint32_t throttle_ymd = (uint32_t)today_ymd;

#ifndef HU_IS_TEST
    /* F25: Emotional check-ins — due moments from 1–3 days ago */
    if (agent->memory) {
        hu_emotional_moment_t *due = NULL;
        size_t due_count = 0;
        if (hu_emotional_moment_get_due(alloc, agent->memory, (int64_t)now, &due, &due_count) ==
                HU_OK &&
            due && due_count > 0) {
            for (size_t d = 0; d < due_count; d++) {
                const hu_emotional_moment_t *m = &due[d];
                /* Find contact and channel for this contact_id */
                for (size_t i = 0; i < agent->persona->contacts_count; i++) {
                    const hu_contact_profile_t *cp = &agent->persona->contacts[i];
                    if (!cp->proactive_checkin || !cp->proactive_channel || !cp->contact_id)
                        continue;
                    /* Strict contact_id equality only.  See 2026-05-16 incident:
                     * the prior implementation also accepted the moment's
                     * contact_id matching cp->proactive_channel (whole or
                     * after-colon), which routed Mindy's emotional moment to
                     * three contacts whose proactive_channel handles
                     * collided.  Predicate pinned by tests/test_proactive.c. */
                    if (!hu_proactive_contact_matches_moment(cp->contact_id, m->contact_id))
                        continue;

                    char ch_buf[64] = {0};
                    char target_route_buf[128] = {0};
                    daemon_proactive_parse_route(cp, ch_buf, target_route_buf);
                    size_t target_len = strlen(target_route_buf);
                    daemon_contact_activity_apply_route(cp->contact_id, now, channels,
                                                        channel_count, ch_buf, target_route_buf,
                                                        &target_len);
                    const char *ch_part = ch_buf;
                    const char *target_part = target_route_buf;

                    for (size_t c = 0; c < channel_count; c++) {
                        if (!channels[c].channel || !channels[c].channel->vtable ||
                            !channels[c].channel->vtable->name)
                            continue;
                        const char *ch_name =
                            channels[c].channel->vtable->name(channels[c].channel->ctx);
                        if (!ch_name || strcmp(ch_name, ch_part) != 0)
                            continue;
                        if (!channels[c].channel->vtable->send)
                            break;

                        /* Outbound safety gate — see 2026-05-16 incident.
                         * m->topic can contain a raw window of the user's own
                         * emotional confession or the literal "(last: %lld)"
                         * recall-format string.  hu_proactive_topic_is_safe is
                         * the predicate that pins what we refuse to ship; if
                         * the topic fails, we drop the send and log enough
                         * context to investigate without leaking the body. */
                        size_t topic_len = strnlen(m->topic, sizeof(m->topic));
                        if (!hu_proactive_topic_is_safe(m->topic, topic_len)) {
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "F25 emotional check-in BLOCKED for %s (unsafe topic, "
                                        "%zu chars)",
                                        cp->name ? cp->name : cp->contact_id, topic_len);
                            (void)hu_emotional_moment_mark_followed_up(agent->memory, m->id);
                            break;
                        }

                        /* FU-1: defer F25 if reactive turn fired for this contact recently. */
                        if (hu_daemon_proactive_should_defer(&agent->contact_send_recency,
                                                             m->contact_id, strlen(m->contact_id),
                                                             (int64_t)now)) {
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "F25 emotional check-in deferred for %s "
                                        "(reactive turn within %ds)",
                                        cp->name ? cp->name : cp->contact_id,
                                        HU_DAEMON_REACTIVE_GATE_WINDOW_S);
                            break;
                        }

                        char msg_buf[384];
                        int w = snprintf(msg_buf, sizeof(msg_buf), "hey how are you doing with %s?",
                                         m->topic);
                        if (w > 0 && (size_t)w < sizeof(msg_buf)) {
                            /* 2026-05-16 P1-6: channel rate-limiter on outbound. */
                            hu_proactive_throttle_t *th = daemon_throttle(alloc);
                            if (!hu_proactive_throttle_channel_try_consume(th, ch_name)) {
                                hu_log_info("human", agent ? agent->observer : NULL,
                                            "F25 emotional check-in to %s skipped: rate-limited",
                                            cp->name ? cp->name : cp->contact_id);
                                break;
                            }
                            /* 2026-05-16 P4-6: per-contact daily/weekly send cap. */
                            uint64_t now_ms_p46 = (uint64_t)now * 1000ULL;
                            if (!hu_proactive_throttle_record_send(th, cp->contact_id, "F25",
                                                                   now_ms_p46)) {
                                hu_log_info("human", agent ? agent->observer : NULL,
                                            "F25 emotional check-in to %s skipped: send-cap",
                                            cp->name ? cp->name : cp->contact_id);
                                break;
                            }
                            hu_error_t send_err = channels[c].channel->vtable->send(
                                channels[c].channel->ctx, target_part, target_len, msg_buf,
                                (size_t)w, NULL, 0);
                            if (send_err == HU_OK) {
                                (void)hu_emotional_moment_mark_followed_up(agent->memory, m->id);
                                hu_contact_send_recency_record(
                                    &agent->contact_send_recency, m->contact_id,
                                    strlen(m->contact_id), (int64_t)now, HU_SEND_PATH_PROACTIVE);
                                hu_log_info("human", agent ? agent->observer : NULL,
                                            "F25 emotional check-in sent to %s: %s",
                                            cp->name ? cp->name : cp->contact_id, msg_buf);
                            }
                        }
                        break;
                    }
                    break;
                }
            }
            alloc->free(alloc->ctx, due, due_count * sizeof(hu_emotional_moment_t));
        }
    }
#endif

#ifdef HU_ENABLE_SQLITE
    /* Phase 9: F103 Life narration — check for unsent narration events (global) */
    if (agent && agent->memory) {
        sqlite3 *p9_db = hu_sqlite_memory_get_db(agent->memory);
        if (p9_db) {
            int64_t narr_ids[3];
            int narr_count = hu_narration_events_unsent(p9_db, 0.7f, narr_ids, 3);
            if (narr_count > 0)
                hu_log_info("human", agent ? agent->observer : NULL,
                            "Phase 9: %d narration events available", narr_count);
        }
    }
#endif

    /* Scheduled message delivery: once per channel, independent of contacts */
    {
        static bool sched_loaded_once;
        if (!sched_loaded_once) {
            const char *sh = getenv("HOME");
            if (sh) {
                char sp[512];
                int sn = snprintf(sp, sizeof(sp), "%s/.human/scheduled.json", sh);
                if (sn > 0 && (size_t)sn < sizeof(sp))
                    hu_conversation_sched_load(sp, (size_t)sn);
            }
            sched_loaded_once = true;
        }
        uint64_t sched_now = (uint64_t)time(NULL) * 1000ULL;
        for (size_t sc = 0; sc < channel_count; sc++) {
            if (!channels[sc].channel || !channels[sc].channel->vtable ||
                !channels[sc].channel->vtable->send || !channels[sc].channel->vtable->name)
                continue;
            const char *sched_ch = channels[sc].channel->vtable->name(channels[sc].channel->ctx);
            if (!sched_ch)
                continue;
            char sched_contact[128], sched_channel[32], sched_msg[512];
            size_t sched_len = hu_conversation_flush_scheduled_for(
                sched_now, sched_ch, strlen(sched_ch), sched_contact, sizeof(sched_contact),
                sched_channel, sizeof(sched_channel), sched_msg, sizeof(sched_msg));
            if (sched_len > 0) {
                /* FU-1: defer scheduled delivery if the reactive turn fired recently. */
                if (agent &&
                    hu_daemon_proactive_should_defer(&agent->contact_send_recency, sched_contact,
                                                     strlen(sched_contact), (int64_t)time(NULL))) {
                    hu_log_info("human", agent ? agent->observer : NULL,
                                "scheduled message deferred for %s (reactive turn within %ds)",
                                sched_contact, HU_DAEMON_REACTIVE_GATE_WINDOW_S);
                    continue;
                }
                hu_validator_chain_apply_default_in_place(alloc, agent ? agent->observer : NULL,
                                                          NULL, 0, "scheduled send", sched_msg,
                                                          &sched_len, sizeof(sched_msg));
                if (sched_len == 0)
                    continue;
                sched_len =
                    hu_conversation_vary_complexity(sched_msg, sched_len, (uint32_t)time(NULL));
                if (sched_len > 1 && sched_msg[0] >= 'A' && sched_msg[0] <= 'Z' &&
                    sched_msg[1] >= 'a' && sched_msg[1] <= 'z' && sched_msg[0] != 'I') {
                    sched_msg[0] = (char)(sched_msg[0] + 32);
                }
                if (sched_len > 1 && sched_msg[sched_len - 1] == '.') {
                    sched_msg[sched_len - 1] = '\0';
                    sched_len--;
                }
                channels[sc].channel->vtable->send(channels[sc].channel->ctx, sched_contact,
                                                   strlen(sched_contact), sched_msg, sched_len,
                                                   NULL, 0);
                if (agent) {
                    hu_contact_send_recency_record(&agent->contact_send_recency, sched_contact,
                                                   strlen(sched_contact), (int64_t)time(NULL),
                                                   HU_SEND_PATH_SCHEDULED);
                }
                hu_log_info("human", agent ? agent->observer : NULL,
                            "scheduled message delivered to %s via %s", sched_contact, sched_ch);
                const char *sh = getenv("HOME");
                if (sh) {
                    char sp[512];
                    int sn = snprintf(sp, sizeof(sp), "%s/.human/scheduled.json", sh);
                    if (sn > 0 && (size_t)sn < sizeof(sp))
                        hu_conversation_sched_save(sp, (size_t)sn);
                }
            }
        }
    }

    /* ── Follow-up watcher (S2.1b) ─────────────────────────────────────────
     * For each iMessage channel and each persona contact with a warmth tier
     * that warrants follow-ups (CLOSE / FRIEND), check chat.db for a
     * read-but-unreplied outbound. If found and not already scheduled,
     * compute a circadian-aware send time and enqueue a template via the
     * existing schedule API. The next iteration's flush_scheduled_for above
     * will pick it up at the computed time.
     *
     * Dedup is in-memory only (a daemon restart wipes the ring); worst case
     * is one duplicate follow-up across a restart, acceptable for first cut.
     * Persistence can land later if duplicates become observable.
     *
     * Per-contact chronotype is not yet a persona field, so we use the
     * agent's own chronotype as a proxy (close friends often share rhythms).
     * Future enhancement: hu_contact_profile.chronotype. */
    {
        static hu_followup_dedup_t followup_dedup;
        static bool followup_dedup_inited;
        if (!followup_dedup_inited) {
            hu_followup_dedup_init(&followup_dedup);
            followup_dedup_inited = true;
        }

        time_t fnow_t = time(NULL);
        struct tm flocal_tm;
        int ftz_off = 0;
        if (localtime_r(&fnow_t, &flocal_tm))
            ftz_off = (int)flocal_tm.tm_gmtoff;

        for (size_t fc = 0; fc < channel_count; fc++) {
            if (!channels[fc].channel || !channels[fc].channel->vtable ||
                !channels[fc].channel->vtable->name)
                continue;
            const char *fch_name = channels[fc].channel->vtable->name(channels[fc].channel->ctx);
            if (!fch_name || strcmp(fch_name, "imessage") != 0)
                continue;

            for (size_t ci = 0; ci < agent->persona->contacts_count; ci++) {
                const hu_contact_profile_t *cp = &agent->persona->contacts[ci];
                if (!cp->contact_id || !cp->warmth_level)
                    continue;

                hu_followup_warmth_t warmth = hu_followup_warmth_from_string(cp->warmth_level);
                if (warmth == HU_FOLLOWUP_WARMTH_NONE)
                    continue;

                int64_t fmsg_id = 0;
                uint64_t fread_at_ms = 0;
#ifdef HU_HAS_IMESSAGE
                hu_error_t qerr = hu_imessage_find_unreplied_read(
                    cp->contact_id, strlen(cp->contact_id), &fmsg_id, &fread_at_ms);
#else
                /* Builds without HU_ENABLE_IMESSAGE can't query chat.db. The
                 * loop's outer guard at line 1284 already filters to imessage
                 * channels; we only reach this point when imessage support is
                 * compiled in. Stub-out as NOT_SUPPORTED for the unreachable
                 * branch so the symbol isn't required at link time. */
                hu_error_t qerr = HU_ERR_NOT_SUPPORTED;
                (void)cp;
                (void)fmsg_id;
                (void)fread_at_ms;
#endif
                if (qerr != HU_OK || fmsg_id == 0)
                    continue;

                if (hu_followup_dedup_seen(&followup_dedup, fmsg_id))
                    continue;

                hu_followup_input_t fin = {
                    .read_at_ms = fread_at_ms,
                    .warmth = warmth,
                    .contact_chronotype = agent->persona->chronotype,
                    .local_tz_offset_seconds = ftz_off,
                    .seed = (uint32_t)fmsg_id ^ (uint32_t)fnow_t,
                };
                hu_followup_decision_t fdec = hu_followup_decide(&fin);
                if (!fdec.should_schedule)
                    continue;

                size_t tmpl_len = strlen(fdec.template_text);
                hu_error_t serr = hu_conversation_schedule_message_on(
                    cp->contact_id, strlen(cp->contact_id), "imessage", 8, fdec.template_text,
                    tmpl_len, fdec.send_at_ms);
                if (serr == HU_OK) {
                    hu_followup_dedup_record(&followup_dedup, fmsg_id);
                    hu_log_info(
                        "human", agent ? agent->observer : NULL,
                        "scheduled follow-up: contact=%s msg_id=%lld send_at_ms=%llu warmth=%d",
                        cp->contact_id, (long long)fmsg_id, (unsigned long long)fdec.send_at_ms,
                        (int)warmth);
                }
            }
        }
    }

    for (size_t i = 0; i < agent->persona->contacts_count; i++) {
        const hu_contact_profile_t *cp = &agent->persona->contacts[i];
        if (!cp->proactive_checkin || !cp->proactive_channel || !cp->contact_id)
            continue;

        char ch_buf[64] = {0};
        char target_route_buf[128] = {0};
        daemon_proactive_parse_route(cp, ch_buf, target_route_buf);
        size_t target_len = strlen(target_route_buf);
        daemon_contact_activity_apply_route(cp->contact_id, now, channels, channel_count, ch_buf,
                                            target_route_buf, &target_len);
        const char *ch_part = ch_buf;
        const char *target_part = target_route_buf;

#ifdef HU_ENABLE_SQLITE
        /* F64: Anticipatory predictions — run for each contact in proactive cycle */
        if (agent->memory) {
            hu_emotional_prediction_t *preds = NULL;
            size_t pred_count = 0;
            (void)hu_anticipatory_predict_with_provider(
                alloc, agent->memory, &agent->provider, agent->model_name, agent->model_name_len,
                cp->contact_id, strlen(cp->contact_id), (int64_t)now, &preds, &pred_count);
            if (preds)
                hu_anticipatory_predictions_free(alloc, preds, pred_count);
        }

        /* Phase 9: F115 Bad-day recovery, F114 Thread follow-ups */
        if (agent->memory && cp->contact_id) {
            sqlite3 *p9_db = hu_sqlite_memory_get_db(agent->memory);
            if (p9_db) {
                int needs_recovery = hu_interaction_quality_needs_recovery(
                    p9_db, cp->contact_id, 0.3f, 7200, 43200, (int64_t)now);
                if (needs_recovery > 0) {
                    hu_log_info("human", agent ? agent->observer : NULL,
                                "Phase 9: recovery needed for %s", cp->contact_id);
                    hu_interaction_quality_mark_recovered(p9_db, cp->contact_id, (int64_t)now);
                }
                int thread_followups =
                    hu_thread_needs_followup(p9_db, cp->contact_id, 14400, 259200, (int64_t)now);
                if (thread_followups > 0)
                    hu_log_info("human", agent ? agent->observer : NULL,
                                "Phase 9: %d thread follow-ups for %s", thread_followups,
                                cp->contact_id);
            }
        }
#endif

        /* Check last interaction time via conversation history */
        for (size_t c = 0; c < channel_count; c++) {
            char *silence_ctx = NULL;
            size_t silence_ctx_len = 0;

            if (!channels[c].channel || !channels[c].channel->vtable ||
                !channels[c].channel->vtable->name)
                continue;
            const char *ch_name = channels[c].channel->vtable->name(channels[c].channel->ctx);
            if (!ch_name || strcmp(ch_name, ch_part) != 0)
                continue;

            if (!channels[c].channel->vtable->load_conversation_history)
                break;

            hu_channel_history_entry_t *entries = NULL;
            size_t entry_count = 0;
            hu_error_t hist_err = channels[c].channel->vtable->load_conversation_history(
                channels[c].channel->ctx, alloc, target_part, target_len, 15, &entries,
                &entry_count);
            if (hist_err != HU_OK)
                hu_log_error("daemon", agent ? agent->observer : NULL,
                             "proactive: history load failed for %s: %s", cp->contact_id,
                             hu_error_string(hist_err));

            uint64_t last_contact_ms = 0;
            bool should_checkin = true;
            if (hist_err == HU_OK && entries && entry_count > 0) {
                struct tm last_tm = {0};
                if (strptime(entries[entry_count - 1].timestamp, "%Y-%m-%d %H:%M", &last_tm)) {
                    time_t last_time = mktime(&last_tm);
                    last_contact_ms = (uint64_t)last_time * 1000ULL;
                    double hours_since = difftime(now, last_time) / 3600.0;
                    if (hours_since < 24.0)
                        should_checkin = false;
                }
            }

#ifdef HU_ENABLE_SQLITE
            /* Strong feed–contact match can justify outreach before the 24h window. */
            if (!should_checkin && agent->memory && agent->persona) {
                sqlite3 *aware_db = hu_sqlite_memory_get_db(agent->memory);
                if (aware_db && hu_feed_awareness_contact_has_high_topics(alloc, aware_db,
                                                                          agent->persona, cp, 0.72))
                    should_checkin = true;
            }
#endif

            /* Build combined text from user messages for event extraction */
            char combined[4096];
            size_t combined_len = 0;
            /* P6-3: also capture the MOST RECENT inbound text for the
             * emotional-tone gate below. Entries are id-ascending, so
             * the last !from_me entry is the latest. */
            char last_inbound_buf[1024];
            size_t last_inbound_len = 0;
            last_inbound_buf[0] = '\0';
            if (entries && entry_count > 0) {
                for (size_t e = 0; e < entry_count; e++) {
                    if (entries[e].from_me)
                        continue;
                    size_t tlen = strlen(entries[e].text);
                    if (tlen == 0)
                        continue;
                    if (combined_len + tlen + 2 < sizeof(combined)) {
                        if (combined_len > 0)
                            combined[combined_len++] = '\n';
                        memcpy(combined + combined_len, entries[e].text, tlen);
                        combined_len += tlen;
                    }
                    size_t copy = tlen;
                    if (copy >= sizeof(last_inbound_buf))
                        copy = sizeof(last_inbound_buf) - 1;
                    memcpy(last_inbound_buf, entries[e].text, copy);
                    last_inbound_buf[copy] = '\0';
                    last_inbound_len = copy;
                }
                combined[combined_len] = '\0';
            }
            if (entries)
                alloc->free(alloc->ctx, entries, entry_count * sizeof(*entries));

#ifndef HU_IS_TEST
            /* Silence check-in: per-contact last activity from conversation history */
            {
                hu_silence_config_t silence_cfg = HU_SILENCE_DEFAULTS;
                hu_proactive_result_t silence_result;
                memset(&silence_result, 0, sizeof(silence_result));
                uint64_t now_ms = (uint64_t)now * 1000ULL;
                if (hu_proactive_check_silence(alloc, last_contact_ms, now_ms, &silence_cfg,
                                               &silence_result) == HU_OK &&
                    silence_result.count > 0) {
                    should_checkin = true;
                    if (agent && agent->bth_metrics)
                        agent->bth_metrics->silence_checkins++;
                    hu_proactive_build_context(&silence_result, alloc, 3, &silence_ctx,
                                               &silence_ctx_len);
                }
                hu_proactive_result_deinit(&silence_result, alloc);
            }
#endif

#ifdef HU_ENABLE_SQLITE
            /* F26: Temporal pattern — reduce proactive probability during quiet hours */
            if (agent && agent->memory && cp->contact_id) {
                int qday = 0, qstart = 0, qend = 1;
                if (hu_superhuman_temporal_get_quiet_hours(agent->memory, alloc, cp->contact_id,
                                                           strlen(cp->contact_id), &qday, &qstart,
                                                           &qend) == HU_OK) {
                    if (tm_now.tm_wday == qday && tm_now.tm_hour >= qstart &&
                        tm_now.tm_hour < qend) {
                        uint32_t seed = (uint32_t)now * 1103515245u + 12345u +
                                        (uint32_t)(uintptr_t)cp->contact_id;
                        if ((seed >> 16u) % 100u < 50u)
                            should_checkin = false;
                    }
                }
            }
#endif

            char *event_ctx = NULL;
            size_t event_ctx_len = 0;
#ifndef HU_IS_TEST
            /* Event extraction + temporal storage runs unconditionally */
            if (combined_len > 0) {
                hu_event_extract_result_t extract_result;
                memset(&extract_result, 0, sizeof(extract_result));
                if (hu_event_extract(alloc, combined, combined_len, &extract_result) == HU_OK &&
                    extract_result.event_count > 0) {
                    if (agent && agent->bth_metrics)
                        agent->bth_metrics->events_extracted += extract_result.event_count;
                    hu_proactive_result_t event_result;
                    memset(&event_result, 0, sizeof(event_result));
                    if (hu_proactive_check_events(alloc, extract_result.events,
                                                  extract_result.event_count,
                                                  &event_result) == HU_OK &&
                        event_result.count > 0) {
                        if (agent && agent->bth_metrics)
                            agent->bth_metrics->event_followups++;
                        hu_proactive_build_context(&event_result, alloc, 3, &event_ctx,
                                                   &event_ctx_len);
                    }
                    hu_proactive_result_deinit(&event_result, alloc);
                }
#ifdef HU_ENABLE_SQLITE
                if (extract_result.event_count > 0 && agent && agent->memory) {
                    sqlite3 *tev_db = hu_sqlite_memory_get_db(agent->memory);
                    if (tev_db) {
                        hu_temporal_events_init_table(tev_db);
                        hu_temporal_events_store_batch(tev_db, cp->contact_id,
                                                       strlen(cp->contact_id), &extract_result,
                                                       (int64_t)now);
                    }
                }
#endif
                hu_event_extract_result_deinit(&extract_result, alloc);
            }
#endif /* !HU_IS_TEST */

#ifdef HU_ENABLE_SQLITE
            /* Temporal event follow-up — upcoming events can trigger check-in */
            if (!should_checkin && agent && agent->memory && cp->contact_id) {
                sqlite3 *tev_db2 = hu_sqlite_memory_get_db(agent->memory);
                if (tev_db2) {
                    hu_temporal_event_t upcoming[3];
                    size_t upcoming_count = 0;
                    hu_temporal_events_get_upcoming(tev_db2, alloc, (int64_t)now, 24 * 3600,
                                                    upcoming, 3, &upcoming_count);
                    for (size_t ui = 0; ui < upcoming_count; ui++) {
                        if (strcmp(upcoming[ui].contact_id, cp->contact_id) == 0) {
                            should_checkin = true;
                            hu_temporal_events_mark_followed_up(tev_db2, upcoming[ui].id);
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "temporal follow-up triggered for %s: %s", cp->contact_id,
                                        upcoming[ui].description);
                            break;
                        }
                    }
                }
            }
#endif

            /* P6-3: emotional-tone gate. If the contact's most-recent
             * inbound message was heavy/grief, skip the generic
             * proactive check-in — the next reactive turn will
             * respond. Generic "hey what's up" on top of a vulnerable
             * message reads as oblivious. */
            if (should_checkin &&
                hu_proactive_should_suppress_for_emotion(last_inbound_buf, last_inbound_len)) {
                hu_log_info(
                    "daemon", agent ? agent->observer : NULL,
                    "proactive: suppressing check-in for %s — last inbound emotionally heavy",
                    cp->contact_id);
                should_checkin = false;
            }

            if (!should_checkin)
                break;

#ifdef HU_ENABLE_SQLITE
            /* F20: Commitment follow-up — add due commitments for this contact */
            char *commitment_ctx = NULL;
            size_t commitment_ctx_len = 0;
            int64_t commitment_ids[3];
            size_t commitment_ids_count = 0;
            if (agent && agent->memory && cp->contact_id) {
                hu_superhuman_commitment_t *due = NULL;
                size_t due_count = 0;
                if (hu_superhuman_commitment_list_due(agent->memory, alloc, (int64_t)now, 3, &due,
                                                      &due_count) == HU_OK &&
                    due && due_count > 0) {
                    size_t cid_len = strlen(cp->contact_id);
                    char ctx_buf[1024];
                    size_t ctx_pos = 0;
                    for (size_t di = 0; di < due_count && ctx_pos < sizeof(ctx_buf) - 200; di++) {
                        if (cid_len != strlen(due[di].contact_id) ||
                            memcmp(due[di].contact_id, cp->contact_id, cid_len) != 0)
                            continue;
                        int n = snprintf(ctx_buf + ctx_pos, sizeof(ctx_buf) - ctx_pos,
                                         "COMMITMENT FOLLOW-UP: %s was due. Ask if it happened: "
                                         "'hey did you ever %s?'\n",
                                         due[di].description, due[di].description);
                        if (n > 0 && ctx_pos + (size_t)n < sizeof(ctx_buf)) {
                            ctx_pos += (size_t)n;
                            if (commitment_ids_count < 3)
                                commitment_ids[commitment_ids_count++] = due[di].id;
                        }
                    }
                    if (ctx_pos > 0) {
                        commitment_ctx = (char *)alloc->alloc(alloc->ctx, ctx_pos + 1);
                        if (commitment_ctx) {
                            memcpy(commitment_ctx, ctx_buf, ctx_pos);
                            commitment_ctx[ctx_pos] = '\0';
                            commitment_ctx_len = ctx_pos;
                        }
                    }
                    hu_superhuman_commitment_free(alloc, due, due_count);
                }
            }
#endif

            /* F53: Birthday/holiday awareness — important_dates from persona */
            char *important_date_ctx = NULL;
            size_t important_date_ctx_len = 0;
            bool had_important_date = false;
            char important_date_msg[256];
            char important_date_type[32];
            bool important_date_sent_today = hu_proactive_throttle_dedup_already_today(
                &g_proactive_throttle, "important_date", cp->contact_id, throttle_ymd);
            /* Backward-compat: also consult the legacy [8] ring while it exists. */
            if (!important_date_sent_today) {
                for (int di = 0; di < g_sent_important_date_count; di++) {
                    if (strcmp(g_sent_important_date_contacts[di], cp->contact_id) == 0) {
                        important_date_sent_today = true;
                        break;
                    }
                }
            }
            if (!important_date_sent_today &&
                hu_proactive_check_important_dates(
                    agent->persona, cp->contact_id, strlen(cp->contact_id), tm_now.tm_mon + 1,
                    tm_now.tm_mday, important_date_msg, sizeof(important_date_msg),
                    important_date_type, sizeof(important_date_type))) {
                char ctx_buf[384];
                int n0 = snprintf(ctx_buf, sizeof(ctx_buf), "IMPORTANT DATE (%s): %s",
                                  important_date_type, important_date_msg);
                if (n0 > 0 && (size_t)n0 < sizeof(ctx_buf)) {
                    size_t n = (size_t)n0;
                    if (strcmp(important_date_type, "birthday") == 0)
                        n = hu_buf_appendf(ctx_buf, sizeof(ctx_buf), n,
                                           " Use confetti effect when sending.");
                    if (n >= sizeof(ctx_buf))
                        n = sizeof(ctx_buf) - 1;
                    if (n > 0 && n < sizeof(ctx_buf)) {
                        important_date_ctx = (char *)alloc->alloc(alloc->ctx, n + 1);
                        if (important_date_ctx) {
                            memcpy(important_date_ctx, ctx_buf, n);
                            important_date_ctx[n] = '\0';
                            important_date_ctx_len = n;
                            had_important_date = true;
                        }
                    }
                }
            }

            /* F12: Bookend messages — morning/evening greetings for close contacts */
            char *bookend_ctx = NULL;
            size_t bookend_ctx_len = 0;
            if (!had_important_date && agent->persona) {
                bool contact_is_close = (cp->dunbar_layer && atoi(cp->dunbar_layer) <= 2);
                bool bookend_sent_today = hu_proactive_throttle_dedup_already_today(
                    &g_proactive_throttle, "important_date", cp->contact_id, throttle_ymd);
                if (!bookend_sent_today) {
                    for (int di = 0; di < g_sent_important_date_count; di++) {
                        if (strcmp(g_sent_important_date_contacts[di], cp->contact_id) == 0) {
                            bookend_sent_today = true;
                            break;
                        }
                    }
                }
                uint32_t seed = (uint32_t)((uint64_t)now ^ (uint64_t)(uintptr_t)cp);
                hu_bookend_type_t btype = hu_bookend_check(
                    (uint32_t)tm_now.tm_hour, contact_is_close, bookend_sent_today, seed);
                if (btype != HU_BOOKEND_NONE) {
                    char *bdir = NULL;
                    size_t bdir_len = 0;
                    if (hu_bookend_build_prompt(alloc, btype, &bdir, &bdir_len) == HU_OK && bdir &&
                        bdir_len > 0) {
                        bookend_ctx = bdir;
                        bookend_ctx_len = bdir_len;
                    } else if (bdir)
                        alloc->free(alloc->ctx, bdir, bdir_len + 1);
                }
            }

            /* Build and send the check-in */
            size_t prompt_len = 0;
            char *prompt =
                proactive_prompt_for_contact(alloc, agent, agent->memory, cp, &prompt_len);
            if (prompt && important_date_ctx && important_date_ctx_len > 0) {
                size_t merged_len = prompt_len + 1 + important_date_ctx_len + 1;
                char *merged = (char *)alloc->alloc(alloc->ctx, merged_len);
                if (merged) {
                    memcpy(merged, prompt, prompt_len);
                    merged[prompt_len] = '\n';
                    memcpy(merged + prompt_len + 1, important_date_ctx, important_date_ctx_len);
                    merged[prompt_len + 1 + important_date_ctx_len] = '\0';
                    alloc->free(alloc->ctx, prompt, prompt_len + 1);
                    prompt = merged;
                    prompt_len = merged_len - 1;
                }
                alloc->free(alloc->ctx, important_date_ctx, important_date_ctx_len + 1);
            }
            if (prompt && bookend_ctx && bookend_ctx_len > 0) {
                size_t merged_len = prompt_len + 1 + bookend_ctx_len + 1;
                char *merged = (char *)alloc->alloc(alloc->ctx, merged_len);
                if (merged) {
                    memcpy(merged, prompt, prompt_len);
                    merged[prompt_len] = '\n';
                    memcpy(merged + prompt_len + 1, bookend_ctx, bookend_ctx_len);
                    merged[prompt_len + 1 + bookend_ctx_len] = '\0';
                    alloc->free(alloc->ctx, prompt, prompt_len + 1);
                    prompt = merged;
                    prompt_len = prompt_len + 1 + bookend_ctx_len;
                }
                alloc->free(alloc->ctx, bookend_ctx, bookend_ctx_len + 1);
                bookend_ctx = NULL;
            }
#ifdef HU_ENABLE_SQLITE
            if (prompt && commitment_ctx && commitment_ctx_len > 0) {
                size_t merged_len = prompt_len + 1 + commitment_ctx_len + 1;
                char *merged = (char *)alloc->alloc(alloc->ctx, merged_len);
                if (merged) {
                    memcpy(merged, prompt, prompt_len);
                    merged[prompt_len] = '\n';
                    memcpy(merged + prompt_len + 1, commitment_ctx, commitment_ctx_len);
                    merged[prompt_len + 1 + commitment_ctx_len] = '\0';
                    alloc->free(alloc->ctx, prompt, prompt_len + 1);
                    prompt = merged;
                    prompt_len = prompt_len + 1 + commitment_ctx_len;
                }
                alloc->free(alloc->ctx, commitment_ctx, commitment_ctx_len + 1);
                commitment_ctx = NULL;
            }
            /* F23: Topic absence — 20% chance to inject when they haven't mentioned usual topics */
            {
                char *topic_absence_json = NULL;
                size_t topic_absence_len = 0;
                int64_t now_ts = (int64_t)now;
                size_t cid_len = strlen(cp->contact_id);
                if (agent->memory &&
                    hu_superhuman_topic_absence_list(agent->memory, alloc, cp->contact_id, cid_len,
                                                     now_ts, 14, &topic_absence_json,
                                                     &topic_absence_len) == HU_OK &&
                    topic_absence_json && topic_absence_len > 0 &&
                    strstr(topic_absence_json, "- ") != NULL &&
                    ((uint32_t)((uintptr_t)cp + (uintptr_t)now) % 100) < 20) {
                    const char *hint =
                        "Topics they usually mention but haven't recently: [see above]. "
                        "Gentle check-in if appropriate.";
                    size_t hint_len = strlen(hint);
                    size_t merged_len = prompt_len + 1 + topic_absence_len + 1 + hint_len + 2;
                    char *merged = (char *)alloc->alloc(alloc->ctx, merged_len);
                    if (merged) {
                        size_t pos = 0;
                        if (prompt && prompt_len > 0) {
                            memcpy(merged, prompt, prompt_len);
                            pos += prompt_len;
                        }
                        merged[pos++] = '\n';
                        memcpy(merged + pos, topic_absence_json, topic_absence_len);
                        pos += topic_absence_len;
                        merged[pos++] = '\n';
                        memcpy(merged + pos, hint, hint_len);
                        pos += hint_len;
                        merged[pos++] = '\n';
                        merged[pos] = '\0';
                        if (prompt)
                            alloc->free(alloc->ctx, prompt, prompt_len + 1);
                        prompt = merged;
                        prompt_len = pos;
                    }
                    alloc->free(alloc->ctx, topic_absence_json, topic_absence_len + 1);
                } else if (topic_absence_json) {
                    alloc->free(alloc->ctx, topic_absence_json, topic_absence_len + 1);
                }
            }
            /* F24: Growth celebration — 15% chance, inject milestone to celebrate */
            {
                char *growth_pro = NULL;
                size_t growth_pro_len = 0;
                size_t cid_len = strlen(cp->contact_id);
                if (agent->memory && (uint32_t)((uintptr_t)cp + (uintptr_t)now) % 100 < 15 &&
                    hu_superhuman_growth_list_recent(agent->memory, alloc, cp->contact_id, cid_len,
                                                     1, &growth_pro, &growth_pro_len) == HU_OK &&
                    growth_pro && growth_pro_len > 0 && strstr(growth_pro, "(none)") == NULL) {
                    char celebrate_buf[512];
                    int cb =
                        snprintf(celebrate_buf, sizeof(celebrate_buf),
                                 "CELEBRATE: %.*s Acknowledge their progress.",
                                 (int)(growth_pro_len < 450 ? growth_pro_len : 450), growth_pro);
                    if (cb > 0 && (size_t)cb < sizeof(celebrate_buf) && prompt) {
                        size_t merged_len = prompt_len + 1 + (size_t)cb + 1;
                        char *merged = (char *)alloc->alloc(alloc->ctx, merged_len);
                        if (merged) {
                            memcpy(merged, prompt, prompt_len);
                            merged[prompt_len] = '\n';
                            memcpy(merged + prompt_len + 1, celebrate_buf, (size_t)cb);
                            merged[prompt_len + 1 + (size_t)cb] = '\0';
                            alloc->free(alloc->ctx, prompt, prompt_len + 1);
                            prompt = merged;
                            prompt_len = merged_len - 1;
                        }
                    }
                    alloc->free(alloc->ctx, growth_pro, growth_pro_len + 1);
                    growth_pro = NULL;
                }
                if (growth_pro)
                    alloc->free(alloc->ctx, growth_pro, growth_pro_len + 1);
            }
            /* F30: Spontaneous curiosity — 10–15% chance, inject random question from micro-moments
             */
            {
                char curiosity_buf[384];
                uint32_t seed_cur = (uint32_t)((uintptr_t)cp + (uintptr_t)now);
                if (agent->memory &&
                    hu_proactive_check_curiosity(alloc, agent->memory, cp->contact_id,
                                                 strlen(cp->contact_id), seed_cur, curiosity_buf,
                                                 sizeof(curiosity_buf))) {
                    size_t cb_len = strlen(curiosity_buf);
                    if (prompt && cb_len > 0) {
                        size_t merged_len = prompt_len + 1 + cb_len + 1;
                        char *merged = (char *)alloc->alloc(alloc->ctx, merged_len);
                        if (merged) {
                            memcpy(merged, prompt, prompt_len);
                            merged[prompt_len] = '\n';
                            memcpy(merged + prompt_len + 1, curiosity_buf, cb_len);
                            merged[prompt_len + 1 + cb_len] = '\0';
                            alloc->free(alloc->ctx, prompt, prompt_len + 1);
                            prompt = merged;
                            prompt_len = merged_len - 1;
                        }
                    }
                }
            }
            /* F31: Callback opportunities — 30% chance, inject follow-up from delayed/commitments.
             * 2026-05-16 P4-4: track followup_id and mark it sent ONLY after send
             * is confirmed. Pre-fix the followup row was never marked sent so the
             * same followup re-fired every proactive cycle. */
            int64_t delayed_followup_id_to_mark = -1;
            {
                char callback_buf[512];
                uint32_t seed_cb = (uint32_t)((uintptr_t)cp + (uintptr_t)now + 1);
                if (agent->memory &&
                    hu_proactive_check_callbacks_ex(
                        alloc, agent->memory, cp->contact_id, strlen(cp->contact_id), seed_cb,
                        callback_buf, sizeof(callback_buf), &delayed_followup_id_to_mark)) {
                    size_t cb_len = strlen(callback_buf);
                    if (prompt && cb_len > 0) {
                        size_t merged_len = prompt_len + 1 + cb_len + 1;
                        char *merged = (char *)alloc->alloc(alloc->ctx, merged_len);
                        if (merged) {
                            memcpy(merged, prompt, prompt_len);
                            merged[prompt_len] = '\n';
                            memcpy(merged + prompt_len + 1, callback_buf, cb_len);
                            merged[prompt_len + 1 + cb_len] = '\0';
                            alloc->free(alloc->ctx, prompt, prompt_len + 1);
                            prompt = merged;
                            prompt_len = merged_len - 1;
                        }
                    }
                }
            }
            /* F130: Collaborative plan proposal check */
            if (agent->memory && prompt) {
                bool should_propose = hu_collab_plan_should_propose(
                    cp->contact_id, strlen(cp->contact_id), 0, 3, 0.5);
                if (should_propose) {
                    static const char PLAN_HINT[] =
                        "\nConsider naturally suggesting a plan or activity to do together.";
                    size_t hint_len = sizeof(PLAN_HINT) - 1;
                    size_t merged_len = prompt_len + hint_len + 1;
                    char *merged = (char *)alloc->alloc(alloc->ctx, merged_len);
                    if (merged) {
                        memcpy(merged, prompt, prompt_len);
                        memcpy(merged + prompt_len, PLAN_HINT, hint_len);
                        merged[merged_len - 1] = '\0';
                        alloc->free(alloc->ctx, prompt, prompt_len + 1);
                        prompt = merged;
                        prompt_len = merged_len - 1;
                    }
                }
            }
#endif
            if (prompt && event_ctx && event_ctx_len > 0) {
                size_t merged_len = prompt_len + 1 + event_ctx_len + 1;
                char *merged = (char *)alloc->alloc(alloc->ctx, merged_len);
                if (merged) {
                    memcpy(merged, prompt, prompt_len);
                    merged[prompt_len] = '\n';
                    memcpy(merged + prompt_len + 1, event_ctx, event_ctx_len);
                    merged[prompt_len + 1 + event_ctx_len] = '\0';
                    alloc->free(alloc->ctx, prompt, prompt_len + 1);
                    prompt = merged;
                    prompt_len = prompt_len + 1 + event_ctx_len;
                }
            }
            if (prompt && silence_ctx && silence_ctx_len > 0) {
                size_t merged_len = prompt_len + 1 + silence_ctx_len + 1;
                char *merged = (char *)alloc->alloc(alloc->ctx, merged_len);
                if (merged) {
                    memcpy(merged, prompt, prompt_len);
                    merged[prompt_len] = '\n';
                    memcpy(merged + prompt_len + 1, silence_ctx, silence_ctx_len);
                    merged[prompt_len + 1 + silence_ctx_len] = '\0';
                    alloc->free(alloc->ctx, prompt, prompt_len + 1);
                    prompt = merged;
                    prompt_len = prompt_len + 1 + silence_ctx_len;
                }
            }
            /* F19: Inside joke callback — 10–15% chance, pick one and add to prompt */
            int64_t joke_id_to_reference = -1;
            if (agent->memory && (uint32_t)((uintptr_t)cp + (uintptr_t)now) % 100 < 12) {
                hu_inside_joke_t *jokes_pro = NULL;
                size_t jokes_pro_count = 0;
                size_t cid_len = strlen(cp->contact_id);
                if (hu_superhuman_inside_joke_list(agent->memory, alloc, cp->contact_id, cid_len, 3,
                                                   &jokes_pro, &jokes_pro_count) == HU_OK &&
                    jokes_pro && jokes_pro_count > 0) {
                    size_t idx = (uint32_t)((uintptr_t)cp + (uintptr_t)now + 1) % jokes_pro_count;
                    joke_id_to_reference = jokes_pro[idx].id;
                    char cb_buf[384];
                    size_t ctx_pl = strnlen(jokes_pro[idx].context, 120);
                    size_t pl_pl = strnlen(jokes_pro[idx].punchline, 80);
                    int w = snprintf(cb_buf, sizeof(cb_buf),
                                     "CALLBACK: Reference this inside joke naturally: [%.*s] %.*s",
                                     (int)ctx_pl, jokes_pro[idx].context, (int)pl_pl,
                                     jokes_pro[idx].punchline);
                    if (w > 0 && (size_t)w < sizeof(cb_buf) && prompt) {
                        size_t merged_len = prompt_len + 1 + (size_t)w + 1;
                        char *merged = (char *)alloc->alloc(alloc->ctx, merged_len);
                        if (merged) {
                            memcpy(merged, prompt, prompt_len);
                            merged[prompt_len] = '\n';
                            memcpy(merged + prompt_len + 1, cb_buf, (size_t)w);
                            merged[prompt_len + 1 + (size_t)w] = '\0';
                            alloc->free(alloc->ctx, prompt, prompt_len + 1);
                            prompt = merged;
                            prompt_len = merged_len - 1;
                        }
                    }
                    hu_superhuman_inside_joke_free(alloc, jokes_pro, jokes_pro_count);
                }
            }
            if (prompt) {
                hu_agent_clear_history(agent);
                agent->active_channel = ch_part;
                agent->active_channel_len = strlen(ch_part);

                /* Scope memory to this contact so facts/memories are
                 * stored with the correct session_id rather than empty.
                 * Mark as proactive turn to suppress storing the prompt
                 * itself as a memory (prevents feedback loops). */
                agent->memory_session_id = cp->contact_id;
                agent->memory_session_id_len = strlen(cp->contact_id);
                if (agent->memory && agent->memory->vtable) {
                    agent->memory->current_session_id = cp->contact_id;
                    agent->memory->current_session_id_len = agent->memory_session_id_len;
                }
                agent->proactive_turn = true;

                char *response = NULL;
                size_t response_len = 0;
                hu_error_t err = hu_agent_turn(agent, prompt, prompt_len, &response, &response_len);
                agent->proactive_turn = false;

                if (err == HU_OK && response && response_len > 0) {
                    bool skip = (response_len == 4 && memcmp(response, "SKIP", 4) == 0);
                    /* F68: Protective boundary — skip proactive if topic is boundary */
                    if (!skip && agent->memory &&
                        hu_protective_is_boundary(agent->memory, cp->contact_id,
                                                  strlen(cp->contact_id), "proactive", 9))
                        skip = true;
                    /* FU-1: defer proactive check-in if reactive turn fired recently. */
                    if (!skip && hu_daemon_proactive_should_defer(
                                     &agent->contact_send_recency, cp->contact_id,
                                     strlen(cp->contact_id), (int64_t)now)) {
                        hu_log_info("human", agent ? agent->observer : NULL,
                                    "proactive check-in deferred for %s "
                                    "(reactive turn within %ds)",
                                    cp->name ? cp->name : cp->contact_id,
                                    HU_DAEMON_REACTIVE_GATE_WINDOW_S);
                        skip = true;
                    }
                    if (!skip && channels[c].channel->vtable->send) {
                        hu_validator_chain_apply_default_in_place(
                            alloc, agent ? agent->observer : NULL, NULL, 0, "proactive send",
                            response, &response_len, response_len + 1);
                        if (response_len == 0)
                            skip = true;
                        response_len =
                            hu_conversation_vary_complexity(response, response_len, (uint32_t)now);
                        if (response_len > 1 && response[0] >= 'A' && response[0] <= 'Z' &&
                            response[1] >= 'a' && response[1] <= 'z' && response[0] != 'I') {
                            response[0] = (char)(response[0] + 32);
                        }
                        if (response_len > 1 && response[response_len - 1] == '.') {
                            response[response_len - 1] = '\0';
                            response_len--;
                        }
                        /* 2026-05-16 P1-6 / P4-6: rate-limit + per-contact send-cap on
                         * proactive outbound. The pre-fix path called vtable->send
                         * directly with no throttle, leading to 4x burst sends. */
                        hu_proactive_throttle_t *th_main = daemon_throttle(alloc);
                        if (!hu_proactive_throttle_channel_try_consume(th_main, ch_part)) {
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "proactive check-in to %s skipped: rate-limited",
                                        cp->name ? cp->name : cp->contact_id);
                            skip = true;
                        }
                        if (!skip &&
                            !hu_proactive_throttle_record_send(th_main, cp->contact_id, "proactive",
                                                               (uint64_t)now * 1000ULL)) {
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "proactive check-in to %s skipped: send-cap",
                                        cp->name ? cp->name : cp->contact_id);
                            skip = true;
                        }
                        if (!skip) {
                            channels[c].channel->vtable->send(channels[c].channel->ctx, target_part,
                                                              target_len, response, response_len,
                                                              NULL, 0);
                            /* FU-1: record proactive send so reactive deferral works. */
                            hu_contact_send_recency_record(&agent->contact_send_recency,
                                                           cp->contact_id, strlen(cp->contact_id),
                                                           (int64_t)now, HU_SEND_PATH_PROACTIVE);
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "proactive check-in sent to %s: %.*s",
                                        cp->name ? cp->name : cp->contact_id, (int)response_len,
                                        response);
                            hu_governor_record_sent(&gov_budget, (uint64_t)time(NULL) * 1000ULL);
                            if (had_important_date && strcmp(important_date_type, "birthday") == 0)
                                hu_log_info("human", agent ? agent->observer : NULL,
                                            "F53: birthday message — use confetti effect");
                            if (had_important_date) {
                                /* 2026-05-16 P1-7: authoritative heap-backed dedup. */
                                (void)hu_proactive_throttle_dedup_first_today(
                                    &g_proactive_throttle, "important_date", cp->contact_id,
                                    throttle_ymd);
                                /* Legacy [8] ring kept in lockstep until removal. */
                                if (g_sent_important_date_count < 8) {
                                    size_t cid_len = strlen(cp->contact_id);
                                    if (cid_len < 64) {
                                        memcpy(g_sent_important_date_contacts
                                                   [g_sent_important_date_count],
                                               cp->contact_id, cid_len + 1);
                                        g_sent_important_date_count++;
                                    }
                                }
                            }
                            if (joke_id_to_reference >= 0 && agent->memory)
                                (void)hu_superhuman_inside_joke_reference(agent->memory,
                                                                          joke_id_to_reference);
#ifdef HU_ENABLE_SQLITE
                            for (size_t mi = 0; mi < commitment_ids_count; mi++)
                                (void)hu_superhuman_commitment_mark_followed_up(agent->memory,
                                                                                commitment_ids[mi]);
                            /* 2026-05-16 P4-4: mark delayed_followup as sent only
                             * on confirmed delivery so a failed send leaves the
                             * row in the queue for retry. */
                            if (delayed_followup_id_to_mark >= 0 && agent->memory)
                                (void)hu_superhuman_delayed_followup_mark_sent(
                                    agent->memory, delayed_followup_id_to_mark);
#endif
                        }
                    }
                }
                if (response)
                    agent->alloc->free(agent->alloc->ctx, response, response_len + 1);
                alloc->free(alloc->ctx, prompt, prompt_len + 1);
                hu_agent_clear_history(agent);

                /* Clear proactive session scope so the next inbound message
                 * doesn't inherit a stale contact_id. */
                agent->memory_session_id = NULL;
                agent->memory_session_id_len = 0;
                if (agent->memory && agent->memory->vtable) {
                    agent->memory->current_session_id = NULL;
                    agent->memory->current_session_id_len = 0;
                }
            }

            /* Proactive "good morning" scheduler: per-contact, once per day
             * during the 6-9am window. Uses LLM to generate personalized
             * greetings when the agent is available. Ring buffer with day
             * rollover to avoid stale entries blocking new contacts. */
            {
                static struct {
                    char contact_id[64];
                    uint32_t day;
                } gm_sent[8];
                static size_t gm_sent_count;
                static uint32_t gm_last_day;
                time_t gm_now = time(NULL);
                struct tm gm_tm;
                struct tm *gm_p = localtime_r(&gm_now, &gm_tm);
                uint32_t gm_day = gm_p ? (uint32_t)(gm_p->tm_yday + gm_p->tm_year * 366) : 0;
                int gm_hour = gm_p ? gm_p->tm_hour : 12;
                /* Day rollover: clear tracker when calendar day changes */
                if (gm_day != gm_last_day) {
                    gm_sent_count = 0;
                    gm_last_day = gm_day;
                }
                if (gm_hour >= 6 && gm_hour < 9 && cp && cp->relationship_type) {
                    bool is_close = (strcmp(cp->relationship_type, "partner") == 0 ||
                                     strcmp(cp->relationship_type, "close_friend") == 0 ||
                                     strcmp(cp->relationship_type, "family") == 0);
                    /* 2026-05-16 P1-7: authoritative heap-backed dedup.
                     * Replaces the [8] ring buffer above which overflowed
                     * silently with 9+ close contacts. The legacy ring is
                     * kept in lockstep until we delete it. */
                    hu_proactive_throttle_t *gm_th = daemon_throttle(alloc);
                    bool already_sent = hu_proactive_throttle_dedup_already_today(
                        gm_th, "gm", cp->contact_id, gm_day);
                    size_t cid_len = strlen(cp->contact_id);
                    if (!already_sent) {
                        for (size_t gi = 0; gi < gm_sent_count && !already_sent; gi++) {
                            if (gm_sent[gi].day == gm_day &&
                                strcmp(gm_sent[gi].contact_id, cp->contact_id) == 0)
                                already_sent = true;
                        }
                    }
                    if (is_close && !already_sent) {
                        /* Mark in heap-backed dedup; scales to 256 contacts. */
                        (void)hu_proactive_throttle_dedup_first_today(gm_th, "gm", cp->contact_id,
                                                                      gm_day);
                        /* Legacy [8] ring kept for backward compat. */
                        if (gm_sent_count < 8) {
                            size_t cn = cid_len < 63 ? cid_len : 63;
                            memcpy(gm_sent[gm_sent_count].contact_id, cp->contact_id, cn);
                            gm_sent[gm_sent_count].contact_id[cn] = '\0';
                            gm_sent[gm_sent_count].day = gm_day;
                            gm_sent_count++;
                        }

                        const char *greeting = "good morning :)";
                        size_t greeting_len = 15;
                        char gm_resp[256];
                        if (agent && agent->provider.vtable && agent->provider.vtable->chat) {
                            const char *gm_prompt =
                                "Generate a very brief, warm morning greeting for a "
                                "close friend/family member. One short sentence max. "
                                "Natural and casual, like a real text. No emojis unless "
                                "it fits your personality. Examples: 'morning!', "
                                "'hey good morning', 'rise and shine :)'";
                            hu_agent_clear_history(agent);
                            agent->proactive_turn = true;
                            char *gm_out = NULL;
                            size_t gm_out_len = 0;
                            hu_error_t gm_err = hu_agent_turn(agent, gm_prompt, strlen(gm_prompt),
                                                              &gm_out, &gm_out_len);
                            agent->proactive_turn = false;
                            if (gm_err == HU_OK && gm_out && gm_out_len > 0 &&
                                gm_out_len < sizeof(gm_resp)) {
                                memcpy(gm_resp, gm_out, gm_out_len);
                                gm_resp[gm_out_len] = '\0';
                                greeting = gm_resp;
                                greeting_len = gm_out_len;
                            }
                            if (gm_out)
                                agent->alloc->free(agent->alloc->ctx, gm_out, gm_out_len + 1);
                            hu_agent_clear_history(agent);
                        }
                        uint32_t gm_seed = (uint32_t)(gm_now * 48271u) + (uint32_t)(uintptr_t)cp;
                        uint32_t offset_min = 450 + (gm_seed % 90);
                        struct tm tomorrow_tm = gm_tm;
                        tomorrow_tm.tm_mday += 1;
                        tomorrow_tm.tm_hour = (int)(offset_min / 60);
                        tomorrow_tm.tm_min = (int)(offset_min % 60);
                        tomorrow_tm.tm_sec = 0;
                        tomorrow_tm.tm_isdst = -1;
                        time_t deliver_t = mktime(&tomorrow_tm);
                        uint64_t deliver_ms = (uint64_t)deliver_t * 1000ULL;
                        const char *gm_ch =
                            channels[c].channel->vtable->name
                                ? channels[c].channel->vtable->name(channels[c].channel->ctx)
                                : "";
                        hu_conversation_schedule_message_on(cp->contact_id, cid_len, gm_ch,
                                                            strlen(gm_ch), greeting, greeting_len,
                                                            deliver_ms);
                        hu_log_info("human", agent ? agent->observer : NULL,
                                    "scheduled morning message for %s: %.*s",
                                    cp->name ? cp->name : cp->contact_id, (int)greeting_len,
                                    greeting);
                        const char *sh = getenv("HOME");
                        if (sh) {
                            char sp[512];
                            int sn = snprintf(sp, sizeof(sp), "%s/.human/scheduled.json", sh);
                            if (sn > 0 && (size_t)sn < sizeof(sp))
                                hu_conversation_sched_save(sp, (size_t)sn);
                        }
                    }
                }
            }

#ifdef HU_ENABLE_SQLITE
            /* Proactive photo sharing: scan Apple Photos for shareable content */
            if (channels[c].channel->vtable->send && combined_len > 0) {
                static uint64_t last_photo_scan_ms;
                uint64_t pnow_ms = (uint64_t)time(NULL) * 1000ULL;
                if (pnow_ms - last_photo_scan_ms > 3600000) { /* max once per hour */
                    last_photo_scan_ms = pnow_ms;
                    char photos_db[512];
                    size_t pdb_len = hu_visual_apple_photos_db_path(photos_db, sizeof(photos_db));
                    if (pdb_len > 0) {
                        hu_visual_entry_t *photos = NULL;
                        size_t photo_count = 0;
                        if (hu_visual_scan_apple_photos(alloc, photos_db, 3, &photos, &photo_count,
                                                        5) == HU_OK &&
                            photos && photo_count > 0) {
                            /* Collect top N shareable photos (album mode: up to 3) */
                            typedef struct {
                                size_t idx;
                                double conf;
                            } photo_candidate_t;
                            photo_candidate_t candidates[3];
                            size_t cand_count = 0;
                            for (size_t pi = 0; pi < photo_count; pi++) {
                                bool should_share = false;
                                double conf = 0.0;
                                hu_visual_should_share(&photos[pi], combined, combined_len,
                                                       &should_share, &conf);
                                if (!should_share || conf < 0.3)
                                    continue;
                                if (cand_count < 3) {
                                    candidates[cand_count].idx = pi;
                                    candidates[cand_count].conf = conf;
                                    cand_count++;
                                } else {
                                    size_t worst = 0;
                                    for (size_t ci = 1; ci < 3; ci++) {
                                        if (candidates[ci].conf < candidates[worst].conf)
                                            worst = ci;
                                    }
                                    if (conf > candidates[worst].conf) {
                                        candidates[worst].idx = pi;
                                        candidates[worst].conf = conf;
                                    }
                                }
                            }
                            if (cand_count > 0) {
                                const char *media[3];
                                size_t media_count = 0;
                                for (size_t ci = 0; ci < cand_count; ci++) {
                                    if (photos[candidates[ci].idx].path[0])
                                        media[media_count++] = photos[candidates[ci].idx].path;
                                }
                                /* FU-1: defer photo album if reactive turn fired recently. */
                                bool photo_defer = hu_daemon_proactive_should_defer(
                                    &agent->contact_send_recency, cp->contact_id,
                                    strlen(cp->contact_id), (int64_t)now);
                                if (photo_defer) {
                                    hu_log_info("human", agent ? agent->observer : NULL,
                                                "proactive photo album deferred for %s "
                                                "(reactive turn within %ds)",
                                                cp->name ? cp->name : cp->contact_id,
                                                HU_DAEMON_REACTIVE_GATE_WINDOW_S);
                                } else if (media_count > 0) {
                                    channels[c].channel->vtable->send(channels[c].channel->ctx,
                                                                      target_part, target_len, "",
                                                                      0, media, media_count);
                                    hu_contact_send_recency_record(
                                        &agent->contact_send_recency, cp->contact_id,
                                        strlen(cp->contact_id), (int64_t)now, HU_SEND_PATH_PHOTO);
                                    hu_log_info("human", agent ? agent->observer : NULL,
                                                "proactive photo album: %zu photos shared",
                                                media_count);
                                }
                            }
                            hu_visual_entries_free(alloc, photos, photo_count);
                        }
                    }
                }
            }
#endif
            if (event_ctx)
                alloc->free(alloc->ctx, event_ctx, event_ctx_len + 1);
            if (silence_ctx)
                alloc->free(alloc->ctx, silence_ctx, silence_ctx_len + 1);
            break;
        }
    }
}

#endif

/* Tapback, delays, missed-message acknowledgment moved to daemon_routing.c */

/* ── Signal handling (non-test only) ─────────────────────────────────── */

#if !defined(HU_IS_TEST) && !defined(_WIN32) && !defined(__CYGWIN__)
static volatile sig_atomic_t g_stop_flag = 0;

static void service_signal_handler(int sig) {
    (void)sig;
    g_stop_flag = 1;
}
#endif

/* ── Streaming callback for channels with send_event ─────────────────────── */

/* Photo/video delays and missed-message acknowledgment in daemon_routing.c */

/* ── Outbound bus: streaming chunks + final channel delivery ─────────────── */

typedef struct hu_daemon_out_turn_state {
    bool typing_started;
    bool text_delivered_via_bus;
} hu_daemon_out_turn_state_t;

typedef struct hu_daemon_out_bus_bridge {
    hu_service_channel_t *channels;
    size_t channel_count;
    hu_daemon_out_turn_state_t *active_turn;   /* non-NULL during streaming (chunk typing) */
    hu_daemon_out_turn_state_t *delivery_turn; /* non-NULL around MESSAGE_SENT publish */
    hu_bus_t *bus;
} hu_daemon_out_bus_bridge_t;

typedef struct hu_daemon_stream_ctx {
    hu_bus_t *bus;
    char channel[HU_BUS_CHANNEL_LEN];
    char id[HU_BUS_ID_LEN];
    hu_allocator_t *alloc; /* optional; when non-NULL, stream text chunks run the outbound chain */
} hu_daemon_stream_ctx_t;

#ifndef HU_IS_TEST

static size_t daemon_utf8_safe_truncate(const char *buf, size_t len) {
    if (len == 0)
        return 0;
    size_t pos = len;
    while (pos > 0 && ((unsigned char)buf[pos - 1] & 0xC0) == 0x80)
        --pos;
    if (pos > 0) {
        unsigned char lead = (unsigned char)buf[pos - 1];
        size_t seq_len = 1;
        if ((lead & 0xE0) == 0xC0)
            seq_len = 2;
        else if ((lead & 0xF0) == 0xE0)
            seq_len = 3;
        else if ((lead & 0xF8) == 0xF0)
            seq_len = 4;
        if (pos - 1 + seq_len > len)
            pos = pos - 1;
    }
    return pos;
}

static void daemon_bus_set_message(hu_bus_event_t *bev, const char *data, size_t len) {
    if (!data || len == 0) {
        bev->message[0] = '\0';
        return;
    }
    size_t copy_len = len < HU_BUS_MSG_LEN - 1 ? len : HU_BUS_MSG_LEN - 1;
    if (copy_len < len)
        copy_len = daemon_utf8_safe_truncate(data, copy_len);
    memcpy(bev->message, data, copy_len);
    bev->message[copy_len] = '\0';
}

static hu_service_channel_t *daemon_out_find_channel(hu_service_channel_t *channels, size_t count,
                                                     const char *name) {
    if (!channels || count == 0 || !name || !name[0])
        return NULL;
    for (size_t i = 0; i < count; i++) {
        if (!channels[i].channel || !channels[i].channel->vtable ||
            !channels[i].channel->vtable->name)
            continue;
        const char *n = channels[i].channel->vtable->name(channels[i].channel->ctx);
        if (n && strcmp(n, name) == 0)
            return &channels[i];
    }
    return NULL;
}
/* Rich stream event callback: maps agent stream events to bus (matches gateway pattern). */
static void daemon_stream_event_cb(const hu_agent_stream_event_t *event, void *ctx) {
    hu_daemon_stream_ctx_t *sc = (hu_daemon_stream_ctx_t *)ctx;
    if (!sc || !sc->bus)
        return;
    hu_bus_event_t ev;
    memset(&ev, 0, sizeof(ev));
    memcpy(ev.channel, sc->channel, HU_BUS_CHANNEL_LEN);
    memcpy(ev.id, sc->id, HU_BUS_ID_LEN);

    switch (event->type) {
    case HU_AGENT_STREAM_TEXT:
        if (!event->data || event->data_len == 0)
            return;
        ev.type = HU_BUS_MESSAGE_CHUNK;
        daemon_bus_set_message(&ev, event->data, event->data_len);
        {
            size_t slen = strnlen(ev.message, HU_BUS_MSG_LEN);
            if (slen == 0)
                return;
            if (sc->alloc) {
                /* Run outbound validator chain (covers channel-tag strip, ai-phrase
                 * strip, F2 assistant-closer, and safety validators). */
                hu_output_validator_chain_t *out_chain = NULL;
                if (hu_validators_build_default_outbound_chain(sc->alloc, NULL, 0, &out_chain) ==
                    HU_OK) {
                    hu_chain_result_t cr;
                    memset(&cr, 0, sizeof(cr));
                    bool chain_ok = hu_output_validator_chain_execute(
                                        out_chain, sc->alloc, NULL, ev.message, slen, &cr) == HU_OK;
                    if (chain_ok) {
                        /* Emit telemetry — stream ctx has no observer; NULL is safe. */
                        hu_observer_emit_validator_decision(NULL, &cr, NULL, slen);
                        if (cr.final_decision == HU_VALIDATOR_REJECT) {
                            hu_chain_result_free(sc->alloc, &cr);
                            hu_output_validator_chain_destroy(out_chain);
                            return; /* drop rejected chunk */
                        }
                        if (cr.final_text && cr.final_text_len > 0) {
                            /* CRITICAL #1: truncate to HU_BUS_MSG_LEN-1 rather
                             * than skipping entirely when output is oversize —
                             * a 4095-byte chunk must not escape the chain
                             * unmodified. */
                            size_t copy_len = cr.final_text_len < HU_BUS_MSG_LEN
                                                  ? cr.final_text_len
                                                  : HU_BUS_MSG_LEN - 1;
                            memcpy(ev.message, cr.final_text, copy_len);
                            slen = copy_len;
                            ev.message[slen] = '\0';
                        }
                        hu_chain_result_free(sc->alloc, &cr);
                    }
                    hu_output_validator_chain_destroy(out_chain);
                    if (!chain_ok) {
                        /* Defensive fallback (Sprint 3 US-2 / Sprint 4 US-9):
                         * The primary outbound path uses hu_output_validator_chain_execute
                         * above.  This strip survives only when the chain failed to
                         * build/execute (e.g., allocation failure mid-stream).  Do not
                         * remove without restoring an equivalent safety net — see audit
                         * notes in sprints/sprint-4/notes-from-sprint-3.md. */
                        slen = hu_conversation_strip_channel_tags(ev.message, slen);
                        ev.message[slen] = '\0';
                        if (slen == 0)
                            return;
                    }
                }
                if (slen == 0)
                    return;
            } else {
                /* Defensive fallback (Sprint 3 US-2 / Sprint 4 US-9):
                 * Same intent as the chain-failure arm above; see that comment.
                 * This arm fires when no allocator is available (chain cannot be
                 * built at all), not on chain-execute failure. */
                slen = hu_conversation_strip_channel_tags(ev.message, slen);
                ev.message[slen] = '\0';
                if (slen == 0)
                    return;
            }
        }
        break;
    case HU_AGENT_STREAM_THINKING:
        if (!event->data || event->data_len == 0)
            return;
        ev.type = HU_BUS_THINKING_CHUNK;
        daemon_bus_set_message(&ev, event->data, event->data_len);
        break;
    case HU_AGENT_STREAM_TOOL_START:
        ev.type = HU_BUS_TOOL_CALL;
        daemon_bus_set_message(&ev, event->tool_name, event->tool_name_len);
        break;
    case HU_AGENT_STREAM_TOOL_ARGS:
        ev.type = HU_BUS_TOOL_CALL;
        daemon_bus_set_message(&ev, event->data, event->data_len);
        break;
    case HU_AGENT_STREAM_TOOL_RESULT:
        ev.type = HU_BUS_TOOL_CALL_RESULT;
        daemon_bus_set_message(&ev, event->data, event->data_len);
        break;
    }
    hu_bus_publish(sc->bus, &ev);
}

static bool daemon_outbound_bus_cb(hu_bus_event_type_t type, const hu_bus_event_t *ev,
                                   void *user_ctx) {
    hu_daemon_out_bus_bridge_t *br = (hu_daemon_out_bus_bridge_t *)user_ctx;
    if (!br || !ev)
        return true;
    if (type != HU_BUS_MESSAGE_CHUNK && type != HU_BUS_MESSAGE_SENT)
        return true;

    hu_service_channel_t *sch =
        daemon_out_find_channel(br->channels, br->channel_count, ev->channel);
    if (!sch || !sch->channel || !sch->channel->vtable)
        return true;

    const char *target = ev->id;
    size_t target_len = strnlen(ev->id, HU_BUS_ID_LEN);

    if (type == HU_BUS_MESSAGE_CHUNK) {
        if (!sch->channel->vtable->send_event)
            return true;
        hu_daemon_out_turn_state_t *ts = br->active_turn;
        if (ts && !ts->typing_started && sch->channel->vtable->start_typing) {
            (void)sch->channel->vtable->start_typing(sch->channel->ctx, target, target_len);
            ts->typing_started = true;
        }
        const char *msg = ev->message;
        size_t msg_len = strnlen(msg, HU_BUS_MSG_LEN);
        (void)sch->channel->vtable->send_event(sch->channel->ctx, target, target_len, msg, msg_len,
                                               NULL, 0, HU_OUTBOUND_STAGE_CHUNK);
        return true;
    }

    /* HU_BUS_MESSAGE_SENT */
    const char *msg = ev->payload ? (const char *)ev->payload : ev->message;
    size_t msg_len = msg ? strlen(msg) : 0;
    if (!msg || msg_len == 0)
        return true;

    hu_error_t se = HU_OK;
    bool sent_via_embed = false;
    if (sch->channel->vtable->name && sch->channel->vtable->send) {
        const char *ch_name = sch->channel->vtable->name(sch->channel->ctx);
        if (ch_name && msg_len > 200) {
            hu_allocator_t embed_alloc = hu_system_allocator();
            hu_embed_t emb = {0};
            emb.type = HU_EMBED_RICH;
            emb.description = (char *)msg;
            char *embed_json = NULL;
            size_t embed_json_len = 0;
            hu_error_t ef = HU_ERR_NOT_SUPPORTED;
            if (strcmp(ch_name, "discord") == 0)
                ef = hu_embed_format_discord(&embed_alloc, &emb, &embed_json, &embed_json_len);
            else if (strcmp(ch_name, "slack") == 0)
                ef = hu_embed_format_slack(&embed_alloc, &emb, &embed_json, &embed_json_len);
            else if (strcmp(ch_name, "telegram") == 0)
                ef = hu_embed_format_telegram(&embed_alloc, &emb, &embed_json, &embed_json_len);
            if (ef == HU_OK && embed_json) {
                se = sch->channel->vtable->send(sch->channel->ctx, target, target_len, embed_json,
                                                embed_json_len, NULL, 0);
                embed_alloc.free(embed_alloc.ctx, embed_json, embed_json_len + 1);
                sent_via_embed = true;
            }
        }
    }

    if (!sent_via_embed) {
        if (sch->channel->vtable->send_event) {
            se = sch->channel->vtable->send_event(sch->channel->ctx, target, target_len, msg,
                                                  msg_len, NULL, 0, HU_OUTBOUND_STAGE_FINAL);
        } else if (sch->channel->vtable->send) {
            se = sch->channel->vtable->send(sch->channel->ctx, target, target_len, msg, msg_len,
                                            NULL, 0);
        }
    }
    if (sch->channel->vtable->stop_typing)
        sch->channel->vtable->stop_typing(sch->channel->ctx, target, target_len);
    if (br->delivery_turn && se == HU_OK)
        br->delivery_turn->text_delivered_via_bus = true;
    return true;
}
#endif /* !HU_IS_TEST */

/* ── Service loop ──────────────────────────────────────────────────────── */

/* Consecutive successful turns that produced zero-length assistant text
 * (e.g. MLX HTTP 52 after response_guard retry). Used for loud logging. */
static unsigned g_empty_agent_response_streak;

hu_error_t hu_service_run(hu_allocator_t *alloc, uint32_t tick_interval_ms,
                          hu_service_channel_t *channels, size_t channel_count, hu_agent_t *agent,
                          const hu_config_t *config) {
    if (!alloc)
        return HU_ERR_INVALID_ARGUMENT;
    if (tick_interval_ms == 0)
        tick_interval_ms = 1000;

#ifdef HU_IS_TEST
    (void)tick_interval_ms;
    (void)channels;
    (void)channel_count;
    (void)agent;
    (void)config;
#ifdef HU_HAS_CRON
    hu_daemon_cron_tick(alloc);
    hu_service_run_agent_cron(alloc, agent, channels, channel_count);
#endif
    return HU_OK;
#else

#if !defined(_WIN32) && !defined(__CYGWIN__)
    g_stop_flag = 0;
    signal(SIGTERM, service_signal_handler);
    signal(SIGINT, service_signal_handler);
    signal(SIGPIPE, SIG_IGN);
    setlinebuf(stderr);
    setlinebuf(stdout);
#endif

    /* Enable outcome tracking so persona feedback loop works in daemon */
    hu_outcome_tracker_t daemon_outcomes;
    hu_outcome_tracker_init(&daemon_outcomes, true);
    if (agent && !agent->outcomes)
        hu_agent_set_outcomes(agent, &daemon_outcomes);

#if defined(HU_ENABLE_RL_FULL)
    if (config && config->reaction_collection.enabled && agent && agent->sota.sota_initialized) {
        hu_reaction_handler_set_collector(&agent->sota.dpo_collector);
    }
#endif

    /* Phase 1c of docs/plans/2026-05-18-imessage-sota.md: route iMessage
     * tapbacks on our outbound messages into the personal-model learning
     * pipeline. Independent of HU_ENABLE_RL_FULL — the personal-model
     * sink works regardless of whether the DPO collector is wired. */
    if (agent) {
        hu_reaction_handler_set_personal_model(&agent->personal_model);
    }

    /* Hybrid routing: create a lightweight cloud provider for classification/scoring
     * when the primary provider is a slow local model (llm_decides mode). */
    {
        bool any_llm_decides = false;
        if (config) {
            const char *base = (const char *)&config->channels;
            for (size_t ci = 0; ci < sizeof(k_daemon_configs) / sizeof(k_daemon_configs[0]); ci++) {
                const hu_channel_daemon_config_t *dc =
                    (const hu_channel_daemon_config_t *)(const void *)(base + k_daemon_configs[ci]
                                                                                  .daemon_offset);
                if (dc->llm_decides) {
                    any_llm_decides = true;
                    break;
                }
            }
        }
        if (any_llm_decides && !g_classify_provider_ok) {
            const char *gemini_url =
                config ? hu_config_get_provider_base_url(config, "gemini") : NULL;
            size_t gemini_url_len = gemini_url ? strlen(gemini_url) : 0;
            hu_error_t cp_err = hu_provider_create(alloc, "gemini", 6, NULL, 0, gemini_url,
                                                   gemini_url_len, &g_classify_provider);
            if (cp_err == HU_OK) {
                g_classify_provider_ok = true;
                hu_log_info("human", NULL,
                            "hybrid routing: classify provider ready (gemini flash-lite)");
            } else {
                hu_log_error("human", NULL,
                             "hybrid routing: classify provider failed (%s), "
                             "classifications will be skipped",
                             hu_error_string(cp_err));
            }
        }
    }

#ifdef HU_HAS_CRON
    time_t last_cron_minute = 0;
#endif
#if defined(HU_HAS_CRON)
    time_t proactive_due_at = 0;
#endif

    /* Channel health monitor: periodic checks, auto-reconnect, backoff */
    hu_channel_monitor_t *chan_monitor = NULL;
    {
        hu_channel_monitor_config_t mon_cfg = hu_channel_monitor_config_default();
        if (hu_channel_monitor_create(alloc, &mon_cfg, &chan_monitor) == HU_OK) {
            for (size_t i = 0; i < channel_count; i++) {
                if (channels[i].channel)
                    hu_channel_monitor_add(chan_monitor, channels[i].channel);
            }
        }
    }
    int64_t chan_monitor_last_ts = 0;

    /* Plan 2: Background observer registry — periodic health, memory, security, etc. */
    hu_bg_registry_t bg_registry;
    hu_bg_registry_init(&bg_registry);
    {
        hu_bg_observer_t obs;
        if (hu_bg_health_monitor_create(alloc, &obs) == HU_OK)
            hu_bg_registry_register(&bg_registry, obs);
        if (hu_bg_memory_consolidation_create(alloc, &obs) == HU_OK)
            hu_bg_registry_register(&bg_registry, obs);
        if (hu_bg_security_audit_create(alloc, &obs) == HU_OK)
            hu_bg_registry_register(&bg_registry, obs);
        if (hu_bg_feed_processor_create(alloc, &obs) == HU_OK)
            hu_bg_registry_register(&bg_registry, obs);
        if (hu_bg_dpo_export_create(alloc, &obs) == HU_OK)
            hu_bg_registry_register(&bg_registry, obs);
        if (hu_bg_intelligence_cycle_create(alloc, &obs) == HU_OK)
            hu_bg_registry_register(&bg_registry, obs);
        if (hu_bg_email_digest_create(alloc, &obs) == HU_OK)
            hu_bg_registry_register(&bg_registry, obs);
    }
    int64_t bg_last_tick_epoch = 0;

    hu_bus_t daemon_outbound_bus;
    hu_bus_init(&daemon_outbound_bus);
    hu_daemon_out_bus_bridge_t daemon_out_bus_bridge = {.channels = channels,
                                                        .channel_count = channel_count,
                                                        .active_turn = NULL,
                                                        .delivery_turn = NULL,
                                                        .bus = &daemon_outbound_bus};
    (void)hu_bus_subscribe(&daemon_outbound_bus, daemon_outbound_bus_cb, &daemon_out_bus_bridge,
                           HU_BUS_EVENT_COUNT);

    hu_graph_t *graph = NULL;
    hu_keystore_t *daemon_keystore = NULL;
#ifdef HU_ENABLE_SQLITE
    {
        const char *home = getenv("HOME");
        if (home) {
            char graph_path[HU_MAX_PATH];
            int np = snprintf(graph_path, sizeof(graph_path), "%s/.human/graph.db", home);
            if (np > 0 && (size_t)np < sizeof(graph_path)) {
                if (hu_graph_open(alloc, graph_path, (size_t)np, &graph) != HU_OK)
                    hu_log_error("human", agent ? agent->observer : NULL, "graph open failed: %.*s",
                                 np, graph_path);
            }
        }
    }
    if (graph && agent && agent->retrieval_engine)
        hu_retrieval_set_graph(agent->retrieval_engine, graph);
    /* W4/W7/W14: verifier graph + W7 facade + W14 scheduler (see hu_agent_bind_sqlite_graph). */
    if (graph && agent) {
        hu_error_t be = hu_agent_bind_sqlite_graph(agent, graph, alloc);
        if (be != HU_OK)
            hu_log_warn("human", agent->observer, "agent graph bind: %s", hu_error_string(be));
    }
    /* W15: probe for an existing keystore and initialize if the user has
     * set one up via `human keystore init`. Does NOT auto-create the
     * keystore directory — that requires user interaction (passphrase).
     * If the directory exists and HU_KEYSTORE_PASSPHRASE is set, open
     * and unlock. Otherwise log a warning and continue in plaintext. */
    {
        const char *home = getenv("HOME");
        if (home && *home) {
            char ks_dir[HU_MAX_PATH];
            int kn = snprintf(ks_dir, sizeof(ks_dir), "%s/.human/keys", home);
            if (kn > 0 && (size_t)kn < sizeof(ks_dir)) {
                struct stat ks_st;
                if (stat(ks_dir, &ks_st) == 0 && S_ISDIR(ks_st.st_mode)) {
                    const char *uid = getenv("USER");
                    if (!uid || !*uid)
                        uid = "default";
                    hu_error_t ks_err = hu_keystore_open(alloc, uid, &daemon_keystore);
                    if (ks_err == HU_OK && daemon_keystore) {
                        const char *pp = getenv("HU_KEYSTORE_PASSPHRASE");
                        if (pp && *pp) {
                            ks_err =
                                hu_keystore_unlock_with_passphrase(daemon_keystore, pp, strlen(pp));
                            if (ks_err == HU_OK) {
                                hu_log_info("human", agent ? agent->observer : NULL,
                                            "W15: keystore unlocked for user=%s", uid);
                            } else {
                                hu_log_warn("human", agent ? agent->observer : NULL,
                                            "W15: keystore unlock failed (%s); "
                                            "encryption inactive this session",
                                            hu_error_string(ks_err));
                                hu_keystore_close(daemon_keystore, alloc);
                                daemon_keystore = NULL;
                            }
                        } else {
                            hu_log_warn("human", agent ? agent->observer : NULL,
                                        "W15: keystore directory exists but "
                                        "HU_KEYSTORE_PASSPHRASE unset; "
                                        "encryption inactive this session");
                            hu_keystore_close(daemon_keystore, alloc);
                            daemon_keystore = NULL;
                        }
                    } else {
                        hu_log_warn("human", agent ? agent->observer : NULL,
                                    "W15: keystore open failed (%s)", hu_error_string(ks_err));
                        daemon_keystore = NULL;
                    }
                } else {
                    hu_log_info("human", agent ? agent->observer : NULL,
                                "W15: no keystore at %s; "
                                "memory encryption not active "
                                "(run `human keystore init` to enable)",
                                ks_dir);
                }
            }
        }
    }
    /* W14 — register LoRA + KV runners (context-dependent) so sleep-time
     * ticks can drain the W13 learner and prune the KV cache.
     *
     * Entire block is gated on HU_ENABLE_LEARNING because the LoRA wiring
     * uses `hu_learner_default_config()` and the runner symbols
     * (hu_lora_training_runner, hu_training_data_runner) only exist when
     * the W13 learning loop is built. The KV-cache wiring inside is also
     * skipped here when learning is off — that's fine because the daemon
     * doesn't ship a frontier model on a learning-off build, so KV
     * pre-warming has no chat path to feed. */
#if defined(HU_ENABLE_LEARNING)
    if (agent && agent->w14_scheduler && agent->learner && config && alloc) {
        static bool w14_lora_wired;
        static hu_lora_runner_ctx_t w14_lora_ctx;
        if (!w14_lora_wired) {
            memset(&w14_lora_ctx, 0, sizeof(w14_lora_ctx));
            w14_lora_ctx.learner = agent->learner;
            w14_lora_ctx.scheduler = hu_w14_scheduler_inner(agent->w14_scheduler);
            w14_lora_ctx.alloc = alloc;
            w14_lora_ctx.kv_cache = agent->infra.kv_cache;
            w14_lora_ctx.provider = &agent->provider;
            w14_lora_ctx.adapter_id = "w14_learner";
#if defined(HU_ENABLE_RL_FULL)
            {
                static hu_eval_gate_t w14_eval_gate;
                static hu_leaderboard_runner_t w14_mt_runner;
                static hu_leaderboard_runner_t w14_ifeval_runner;
                static bool w14_eval_gate_ready;
                static char w14_canned_lb_path[512];
                static char w14_prompt_fixture_path[512];

                if (!w14_eval_gate_ready) {
                    const char *lb_env = getenv("HU_EVAL_LEADERBOARD_CANNED");
                    if (lb_env && lb_env[0]) {
                        (void)snprintf(w14_canned_lb_path, sizeof(w14_canned_lb_path), "%s",
                                       lb_env);
                    } else {
                        const char *lb_tries[] = {
                            "tests/fixtures/leaderboard_canned_20.json",
                            "../tests/fixtures/leaderboard_canned_20.json",
                            NULL,
                        };
                        w14_canned_lb_path[0] = '\0';
                        for (size_t li = 0; lb_tries[li]; li++) {
                            FILE *lf = fopen(lb_tries[li], "r");
                            if (lf) {
                                fclose(lf);
                                (void)snprintf(w14_canned_lb_path, sizeof(w14_canned_lb_path), "%s",
                                               lb_tries[li]);
                                break;
                            }
                        }
                        if (w14_canned_lb_path[0] == '\0') {
                            const char *hm = getenv("HOME");
                            (void)snprintf(w14_canned_lb_path, sizeof(w14_canned_lb_path),
                                           "%s/.human/eval/leaderboard_canned_20.json",
                                           hm && hm[0] ? hm : "/tmp");
                        }
                    }

                    hu_leaderboard_config_t lbc = {.canned_path = w14_canned_lb_path, .seed = 42};
                    if (hu_leaderboard_create_mt_bench(alloc, &lbc, &w14_mt_runner) == HU_OK &&
                        hu_leaderboard_create_ifeval(alloc, &lbc, &w14_ifeval_runner) == HU_OK) {
                        memset(&w14_eval_gate, 0, sizeof(w14_eval_gate));
                        w14_eval_gate.baseline_persona_fidelity_mean = 0.50;
                        w14_eval_gate.baseline_mt_bench_mean = 0.55;
                        w14_eval_gate.baseline_ifeval_mean = 0.60;
                        w14_eval_gate.persona_delta_min = 0.05;
                        w14_eval_gate.mt_bench_regression_max = -0.01;
                        w14_eval_gate.ifeval_regression_max = -0.02;
                        w14_eval_gate.baseline_p95_latency_ms = 8000.0;
                        w14_eval_gate.latency_delta_max_ms = 4000.0;
                        w14_eval_gate.bootstrap_samples = 500;
                        w14_eval_gate.bootstrap_seed = 42;
                        w14_eval_gate.mt_bench = &w14_mt_runner;
                        w14_eval_gate.ifeval = &w14_ifeval_runner;
                        w14_eval_gate_ready = true;
                    }
                }

                if (w14_eval_gate_ready) {
                    w14_lora_ctx.eval_gate = &w14_eval_gate;
                    w14_lora_ctx.eval_provider = &agent->provider;
                    w14_lora_ctx.eval_n_prompts = 20;

                    const char *pf_env = getenv("HU_PERSONA_ROLLOUT_FIXTURE");
                    if (pf_env && pf_env[0]) {
                        (void)snprintf(w14_prompt_fixture_path, sizeof(w14_prompt_fixture_path),
                                       "%s", pf_env);
                    } else {
                        const char *pf_tries[] = {
                            "tests/fixtures/persona_rollout_prompts_20.txt",
                            "../tests/fixtures/persona_rollout_prompts_20.txt",
                            NULL,
                        };
                        w14_prompt_fixture_path[0] = '\0';
                        for (size_t pi = 0; pf_tries[pi]; pi++) {
                            FILE *pf = fopen(pf_tries[pi], "r");
                            if (pf) {
                                fclose(pf);
                                (void)snprintf(w14_prompt_fixture_path,
                                               sizeof(w14_prompt_fixture_path), "%s", pf_tries[pi]);
                                break;
                            }
                        }
                        if (w14_prompt_fixture_path[0] == '\0') {
                            const char *hm = getenv("HOME");
                            (void)snprintf(w14_prompt_fixture_path, sizeof(w14_prompt_fixture_path),
                                           "%s/.human/eval/persona_prompts.txt",
                                           hm && hm[0] ? hm : "/tmp");
                        }
                    }
                    w14_lora_ctx.eval_prompt_fixture_path = w14_prompt_fixture_path;
                }
            }
#endif
            w14_lora_ctx.config_template = hu_learner_default_config();
            {
                const char *hm = getenv("HOME");
                if (hm && hm[0]) {
                    (void)snprintf(w14_lora_ctx.config_template.adapter_output_path,
                                   sizeof(w14_lora_ctx.config_template.adapter_output_path),
                                   "%s/.human/ml/w14_learner_adapter.lora", hm);
                } else {
                    (void)snprintf(w14_lora_ctx.config_template.adapter_output_path,
                                   sizeof(w14_lora_ctx.config_template.adapter_output_path), "%s",
                                   "/tmp/human_w14_learner_adapter.lora");
                }
            }
            if (hu_w14_scheduler_register_lora_runner(agent->w14_scheduler, &w14_lora_ctx) ==
                HU_OK) {
                w14_lora_wired = true;
                hu_log_info("human", agent->observer,
                            "W14: LoRA training runner registered (adapter_out=%s)",
                            w14_lora_ctx.config_template.adapter_output_path);
            } else {
                hu_log_warn("human", agent->observer,
                            "W14: LoRA training runner registration failed");
            }
            if (agent->infra.kv_cache) {
                hu_error_t kve = hu_w14_scheduler_register_kv_prewarm_runner(agent->w14_scheduler,
                                                                             agent->infra.kv_cache);
                if (kve != HU_OK)
                    hu_log_warn("human", agent->observer,
                                "W14: KV prewarm runner registration failed: %d", (int)kve);
                else
                    hu_log_info("human", agent->observer, "W14: KV prewarm runner registered");
            }
            /* Training data extraction runner — continuous learning loop.
             * Extracts new conversations into JSONL, generates auto-DPO
             * pairs, and enqueues LoRA retraining when threshold is met. */
            {
                static hu_training_data_runner_ctx_t w14_td_ctx;
                memset(&w14_td_ctx, 0, sizeof(w14_td_ctx));
                w14_td_ctx.alloc = alloc;
                w14_td_ctx.scheduler = agent->w14_scheduler;
                const char *hm = getenv("HOME");
                static char td_db_path[512];
                static char td_out_dir[512];
                if (hm && hm[0]) {
                    (void)snprintf(td_db_path, sizeof(td_db_path), "%s/.human/memory.db", hm);
                    (void)snprintf(td_out_dir, sizeof(td_out_dir), "%s/.human/ml/training_data",
                                   hm);
                } else {
                    (void)snprintf(td_db_path, sizeof(td_db_path), "/tmp/human_memory.db");
                    (void)snprintf(td_out_dir, sizeof(td_out_dir), "/tmp/human_training_data");
                }
                w14_td_ctx.memory_db_path = td_db_path;
                w14_td_ctx.output_dir = td_out_dir;
                hu_error_t tde = hu_w14_scheduler_register_training_data_runner(
                    agent->w14_scheduler, &w14_td_ctx);
                if (tde == HU_OK)
                    hu_log_info("human", agent->observer,
                                "W14: training data runner registered (out=%s)", td_out_dir);
                else
                    hu_log_warn("human", agent->observer,
                                "W14: training data runner registration failed: %d", (int)tde);
            }
            /* US-7.5: nightly LoRA retrain runner (MLX-Gemma subprocess
             * path). Sibling to the in-process learner runner above.
             * Registration is best-effort — failure logs a warning and
             * leaves the slot as a no-op rather than crashing the daemon. */
            {
                static hu_lora_retrain_ctx_t w14_lora_retrain_ctx;
                static char retrain_candidate_dir[512];
                static char retrain_current_symlink[512];
                static char retrain_pidfile[512];
                memset(&w14_lora_retrain_ctx, 0, sizeof(w14_lora_retrain_ctx));
                const char *hm2 = getenv("HOME");
                if (hm2 && hm2[0]) {
                    (void)snprintf(retrain_candidate_dir, sizeof(retrain_candidate_dir),
                                   "%s/.human/ml/seth-lora-candidate", hm2);
                    (void)snprintf(retrain_current_symlink, sizeof(retrain_current_symlink),
                                   "%s/.human/ml/seth-lora-current", hm2);
                    (void)snprintf(retrain_pidfile, sizeof(retrain_pidfile),
                                   "%s/.human/lora_retrain.pid", hm2);
                } else {
                    (void)snprintf(retrain_candidate_dir, sizeof(retrain_candidate_dir),
                                   "/tmp/human_seth_lora_candidate");
                    (void)snprintf(retrain_current_symlink, sizeof(retrain_current_symlink),
                                   "/tmp/human_seth_lora_current");
                    (void)snprintf(retrain_pidfile, sizeof(retrain_pidfile),
                                   "/tmp/human_lora_retrain.pid");
                }
                w14_lora_retrain_ctx.candidate_dir = retrain_candidate_dir;
                w14_lora_retrain_ctx.current_symlink = retrain_current_symlink;
                w14_lora_retrain_ctx.pidfile_path = retrain_pidfile;
                /* Defaults for argv pieces are set by the runner when NULL. */
                hu_error_t rre = hu_w14_scheduler_register_lora_retrain_runner(
                    agent->w14_scheduler, &w14_lora_retrain_ctx);
                if (rre == HU_OK)
                    hu_log_info("human", agent->observer,
                                "W14: nightly LoRA retrain runner registered (candidate=%s)",
                                retrain_candidate_dir);
                else
                    hu_log_warn("human", agent->observer,
                                "W14: nightly LoRA retrain runner registration failed: %d",
                                (int)rre);
            }
        }
    }
#endif /* HU_ENABLE_LEARNING — W14 LoRA + training data runner wiring */
    /* W13 Phase 4.1 — auto-load the configured LoRA adapter into the live
     * provider. Closes the on-device personalization loop without a
     * separate `human ml apply-adapter` invocation. Failures log a warning
     * and continue with the base model so a missing adapter never breaks
     * the daemon. The provider's own `load_adapter` vtable hook silently
     * returns HU_ERR_NOT_SUPPORTED for backends that haven't implemented
     * adapter merging (e.g. cloud providers); we treat that the same as
     * a configured-but-no-op state. */
    if (config && config->personalization.enabled && config->personalization.lora_adapter_path &&
        agent && agent->provider.vtable) {
        const char *adapter_path = config->personalization.lora_adapter_path;
        const char *adapter_id = config->personalization.lora_adapter_id;
        char id_buf[128];
        if (!adapter_id || !*adapter_id) {
            const char *base = strrchr(adapter_path, '/');
            base = base ? base + 1 : adapter_path;
            size_t blen = strlen(base);
            if (blen >= sizeof(id_buf))
                blen = sizeof(id_buf) - 1;
            memcpy(id_buf, base, blen);
            id_buf[blen] = '\0';
            adapter_id = id_buf;
        }
        hu_error_t le =
            hu_provider_load_adapter(&agent->provider, alloc, adapter_path, strlen(adapter_path),
                                     adapter_id, strlen(adapter_id));
        if (le == HU_OK)
            hu_log_info("human", agent->observer, "personalization: loaded adapter '%s' from %s",
                        adapter_id, adapter_path);
        else if (le == HU_ERR_NOT_SUPPORTED) {
            /* US-7.3 (INS-B): cloud providers return HU_ERR_NOT_SUPPORTED
             * from hu_provider_load_adapter. Surface as WARN with the
             * literal "personalization adapter ignored" string so the
             * operator is never silently misled. One-shot per process. */
            const char *pname =
                (agent->provider.vtable && agent->provider.vtable->get_name)
                    ? agent->provider.vtable->get_name(agent->provider.ctx)
                    : (config->default_provider ? config->default_provider : "(unknown)");
            hu_daemon_personalization_warn_adapter_ignored(agent->observer, pname, adapter_id);
        } else
            hu_log_warn("human", agent->observer,
                        "personalization: load_adapter('%s', %s) failed: %d", adapter_id,
                        adapter_path, (int)le);
    }
#ifdef HU_ENABLE_MOLORA
    /* US-7.8 — Initialize the MoLoRA static per-channel router from config.
     * Disabled-by-default; the agent-turn hook is a no-op until the router
     * is enabled AND at least one channel adapter is mapped. The router
     * borrows path pointers from `config->personalization.molora` — config
     * shares the agent's bootstrap arena, so the lifetimes match. */
    if (agent && config) {
        hu_error_t mre = hu_molora_router_init(&agent->molora_router, config);
        if (mre != HU_OK)
            hu_log_warn("human", agent->observer, "molora: router init failed: %d", (int)mre);
        else if (agent->molora_router.enabled)
            hu_log_info("human", agent->observer,
                        "molora: router enabled with %zu channel adapter(s)",
                        agent->molora_router.count);
    }
#endif
    /* Initialize contact identity graph for cross-channel resolution */
    if (agent && agent->memory) {
        sqlite3 *cg_db = hu_sqlite_memory_get_db(agent->memory);
        if (cg_db)
            hu_contact_graph_init(alloc, cg_db);
    }
#endif

#ifdef HU_ENABLE_ML
    /* M3 — optional stub-adapter probe (Track D1.1): validates the frontier seam
     * without wiring chat-time inference. Logs only; never fails startup. */
    if (config && config->personalization.m3_adapter_probe_path && alloc) {
        const char *mp = config->personalization.m3_adapter_probe_path;
        hu_m3_frontier_adapter_t *stub = NULL;
        hu_error_t pe = hu_m3_frontier_adapter_try_open(alloc, mp, strlen(mp), &stub);
        if (pe == HU_OK && stub) {
            (void)hu_m3_frontier_adapter_noop_infer(stub);
            hu_log_info("human", agent ? agent->observer : NULL,
                        "M3: adapter probe OK (path=%s schema=%u)", mp,
                        (unsigned)hu_m3_frontier_adapter_schema_version(stub));
            hu_m3_frontier_adapter_close(alloc, stub);
        } else {
            hu_log_warn("human", agent ? agent->observer : NULL,
                        "M3: adapter probe failed for %s (%d)", mp, (int)pe);
        }
    }
#endif

    {
        struct timespec ts_now;
        clock_gettime(CLOCK_MONOTONIC, &ts_now);
        int64_t start = (int64_t)ts_now.tv_sec * 1000 + ts_now.tv_nsec / 1000000;
        for (size_t i = 0; i < channel_count; i++) {
            channels[i].last_poll_ms = start;
            channels[i].last_contact_ms = 0;
        }
    }

#if !defined(_WIN32) && !defined(__CYGWIN__)
#define HU_STOP_FLAG g_stop_flag
#else
    volatile int local_stop = 0;
#define HU_STOP_FLAG local_stop
#endif

#ifndef HU_IS_TEST
    /* The `replay_insights` static buffer was removed on 2026-05-16: it acted
     * as a process-global fallback that leaked one contact's replay-analysis
     * blob into every other contact's prompt.  Per-contact replay insights
     * are now stored under "replay:<contact_id>:latest" via
     * hu_proactive_build_replay_key.  Daily auto-tune still runs and logs but
     * no longer mutates a shared buffer; Phase 6 will give it a dedicated
     * non-global key if needed. */
    static char community_insights[2048] = {0};
    static size_t community_insights_len = 0;
    static size_t promotion_counter = 0;

    /* Per-contact Theory of Mind belief states */
#define HU_TOM_MAX_CONTACTS    16
    static hu_tom_belief_state_t tom_states[HU_TOM_MAX_CONTACTS];
    static char tom_contact_keys[HU_TOM_MAX_CONTACTS][64];
    static size_t tom_contact_count = 0;
    (void)tom_states;
    (void)tom_contact_keys;
    (void)tom_contact_count;

    /* Per-contact voice maturity profiles */
    static hu_voice_profile_t voice_profiles[HU_TOM_MAX_CONTACTS];
    static char voice_contact_keys[HU_TOM_MAX_CONTACTS][64];
    static size_t voice_contact_count = 0;

    /* Per-contact consecutive response counter.
     * Tracks how many times Human responded in a row without the real
     * user stepping in.  Resets when user_responded_recently fires or
     * when the real user sends a message (is_from_me=1 in history). */
#define HU_CONSEC_MAX_CONTACTS 32
    static uint8_t consec_response_count[HU_CONSEC_MAX_CONTACTS];
    static char consec_contact_keys[HU_CONSEC_MAX_CONTACTS][64];
    static size_t consec_contact_count = 0;

#define HU_LEAVE_ON_READ_MAX 32
    static struct {
        char key[64];
        time_t until;
    } leave_on_read_entries[HU_LEAVE_ON_READ_MAX];

#define HU_COMFORT_PENDING_MAX 32
    static struct {
        char key[64];
        char emotion[64];
        char response_type[32];
    } comfort_pending[HU_COMFORT_PENDING_MAX];

    static hu_bth_metrics_t bth_metrics;
    hu_bth_metrics_init(&bth_metrics);
    if (agent)
        agent->bth_metrics = &bth_metrics;

    /* Phase 3: Inner thought store — accumulates thoughts between conversations */
    static hu_inner_thought_store_t inner_thought_store;
    static bool inner_thought_store_ok = false;
    if (!inner_thought_store_ok) {
        inner_thought_store_ok =
            (hu_inner_thought_store_init(&inner_thought_store, alloc) == HU_OK);
    }

    /* Phase 3: Global turn counter for anti-sycophancy contrarian budget */
    static uint32_t daemon_turn_counter = 0;
    /* Phase 4: Conversation repair signal (persists across turn boundary) */
    static hu_repair_signal_t repair_signal = {0};
#if defined(HU_ENABLE_SQLITE)
    /* Phase 4: Style drift check counter (only with persona + sqlite) */
    static unsigned drift_check_counter = 0;
#endif
#if !defined(HU_ENABLE_SQLITE)
    (void)daemon_turn_counter;
#endif

    hu_inbox_watcher_t inbox_watcher = {0};
    static int64_t last_inbox_poll_ms = 0;
    if (agent && agent->memory) {
        hu_error_t inbox_err = hu_inbox_init(&inbox_watcher, alloc, agent->memory, NULL, 0);
        if (inbox_err != HU_OK)
            hu_log_error("daemon", agent ? agent->observer : NULL, "inbox init failed: %s",
                         hu_error_string(inbox_err));
        inbox_watcher.provider = &agent->provider;
        inbox_watcher.model = agent->model_name;
        inbox_watcher.model_len = agent->model_name_len;
    }

#if HU_HAS_PWA
    hu_pwa_learner_t *pwa_learner = NULL;
    int64_t pwa_learn_last_ms = 0;
    int64_t pwa_learn_interval_ms = 60000; /* scan every 60s */
    if (agent && agent->memory) {
        static hu_pwa_learner_t daemon_learner;
        static bool daemon_learner_ok = false;
        if (!daemon_learner_ok) {
            hu_error_t lerr = hu_pwa_learner_init(alloc, &daemon_learner, agent->memory);
            daemon_learner_ok = (lerr == HU_OK);
        }
        if (daemon_learner_ok)
            pwa_learner = &daemon_learner;
    }
#endif
#endif

#ifdef HU_HAS_SKILLS
    /* P8: Pre-load skill cache at startup */
    if (agent && agent->memory) {
        sqlite3 *skill_init_db = hu_sqlite_memory_get_db(agent->memory);
        if (skill_init_db) {
            hu_skill_t *preloaded = NULL;
            size_t preloaded_count = 0;
            if (hu_skill_load_active(alloc, skill_init_db, NULL, 0, &preloaded, &preloaded_count) ==
                    HU_OK &&
                preloaded) {
                hu_log_info("human", agent ? agent->observer : NULL, "pre-loaded %zu active skills",
                            preloaded_count);
                hu_skill_free(alloc, preloaded, preloaded_count);
            }
        }
    }
#endif

    while (!HU_STOP_FLAG) {
#ifdef HU_HAS_CRON
        {
            time_t t = time(NULL);
            time_t current_minute = t / 60;
            if (current_minute > last_cron_minute) {
                hu_daemon_cron_tick(alloc);
                hu_service_run_agent_cron(alloc, agent, channels, channel_count);
                /* Run proactive check-ins at the top of each hour.
                 * Add jitter (0-30 min) via deferred scheduling to avoid
                 * blocking the daemon loop. */
                if (current_minute % 60 == 0 && proactive_due_at == 0) {
                    unsigned int jitter_sec = 0;
#if defined(__APPLE__) || (defined(__linux__) && defined(__GLIBC__))
                    jitter_sec = (unsigned int)arc4random_uniform(1801);
#else
                    {
                        uint32_t s = (uint32_t)time(NULL);
                        s = s * 1103515245u + 12345u;
                        jitter_sec = (unsigned int)((s >> 16u) & 0x7fffu) % 1801u;
                    }
#endif
                    proactive_due_at = t + (time_t)jitter_sec;
                }
                if (proactive_due_at > 0 && t >= proactive_due_at) {
                    proactive_due_at = 0;
                    hu_service_run_proactive_checkins(alloc, agent, channels, channel_count);
                    if (agent && agent->bth_metrics)
                        hu_bth_metrics_log(agent->bth_metrics);
                }
#ifndef HU_IS_TEST
                /* W4 verifier metrics flush — snapshot the per-process counters
                 * onto disk so `human doctor verifier` (and any future
                 * dashboard) can show the last known hallucination rate even
                 * when the daemon is offline. 60s cadence is a heartbeat, not
                 * a real-time stream; the file is small (~150B) and overwritten
                 * in place, so cost is negligible. Skipped under HU_IS_TEST
                 * because the test harness has its own ad-hoc HOME and the
                 * shared metrics file would race across parallel tests. */
                if (agent) {
                    static int64_t last_verifier_flush_ms = 0;
                    struct timespec ts_vf;
                    clock_gettime(CLOCK_MONOTONIC, &ts_vf);
                    int64_t now_vf_ms = (int64_t)ts_vf.tv_sec * 1000 + ts_vf.tv_nsec / 1000000;
                    if (last_verifier_flush_ms == 0)
                        last_verifier_flush_ms = now_vf_ms;
                    if (now_vf_ms - last_verifier_flush_ms >= 60000) {
                        hu_verifier_metrics_t snap = {
                            .total_runs = agent->verifier_runs,
                            .total_claims_extracted = agent->verifier_claims_total,
                            .total_claims_flagged = agent->verifier_claims_flagged,
                            .last_update_epoch = 0, /* set by save() */
                        };
                        (void)hu_verifier_metrics_save(&snap);
                        last_verifier_flush_ms = now_vf_ms;
                    }
                }
                /* Periodic memory consolidation */
                if (config && config->consolidation_interval_hours > 0 && agent && agent->memory) {
                    static int64_t last_consolidation_ms = 0;
                    int64_t interval_ms = (int64_t)config->consolidation_interval_hours * 3600000LL;
                    struct timespec ts_cons;
                    clock_gettime(CLOCK_MONOTONIC, &ts_cons);
                    int64_t now_ms = (int64_t)ts_cons.tv_sec * 1000 + ts_cons.tv_nsec / 1000000;
                    if (last_consolidation_ms == 0)
                        last_consolidation_ms = now_ms;
                    if (now_ms - last_consolidation_ms >= interval_ms) {
                        hu_consolidation_config_t cons_cfg = {
                            .decay_days = config ? config->behavior.decay_days : 30,
                            .decay_factor = 0.5,
                            .dedup_threshold = config ? config->behavior.dedup_threshold : 0,
                            .max_entries = 5000,
                            .provider = &agent->provider,
                            .model = agent->model_name,
                            .model_len = agent->model_name_len,
                        };
                        if (hu_memory_consolidate(alloc, agent->memory, &cons_cfg) == HU_OK) {
                            last_consolidation_ms = now_ms;
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "periodic memory consolidation completed");
                        }
                    }
                }
#ifdef HU_HAS_SKILLS
                /* Phase 8 (F77-F82): Scheduled reflection engine */
                {
                    static bool reflection_done_today = false;
                    static bool reflection_done_week = false;
                    static bool reflection_done_month = false;
                    struct tm tm_refl;
#if defined(_WIN32) && !defined(__CYGWIN__)
                    struct tm *lt_refl = (localtime_s(&tm_refl, &t) == 0) ? &tm_refl : NULL;
#else
                    struct tm *lt_refl = localtime_r(&t, &tm_refl);
#endif
                    if (lt_refl) {
                        /* Daily: 2-4 AM */
                        if (lt_refl->tm_hour >= 2 && lt_refl->tm_hour < 4 && lt_refl->tm_min == 0 &&
                            !reflection_done_today && agent && agent->memory) {
                            sqlite3 *refl_db = hu_sqlite_memory_get_db(agent->memory);
                            if (refl_db) {
                                hu_reflection_engine_t refl_engine = {.alloc = alloc,
                                                                      .db = refl_db};
                                hu_reflection_daily(&refl_engine, (int64_t)t);
                                reflection_done_today = true;
                                if (agent->bth_metrics)
                                    agent->bth_metrics->reflections_daily++;

                                /* P7: Nightly consolidation after daily reflection */
                                static int64_t last_consol_nightly = 0;
                                static int64_t last_consol_weekly = 0;
                                static int64_t last_consol_monthly = 0;
                                hu_consolidation_engine_t consol = {.alloc = alloc, .db = refl_db};
                                (void)hu_consolidation_engine_run_scheduled(
                                    &consol, (int64_t)t, last_consol_nightly, last_consol_weekly,
                                    last_consol_monthly);
                                last_consol_nightly = (int64_t)t;

                                /* P7: Forgetting curve batch decay */
                                (void)hu_forgetting_apply_batch_decay(refl_db, (int64_t)t, 0.1);

                                /* Vtable-based memory decay + prune */
                                if (agent && agent->memory) {
                                    hu_forgetting_stats_t decay_stats = {0};
                                    if (hu_memory_decay(alloc, agent->memory, 0.05, &decay_stats) ==
                                            HU_OK &&
                                        decay_stats.decayed > 0)
                                        hu_log_info("human", agent ? agent->observer : NULL,
                                                    "memory decay: %zu decayed",
                                                    decay_stats.decayed);
                                    hu_forgetting_stats_t prune_stats = {0};
                                    if (hu_memory_prune(alloc, agent->memory, 0.01, &prune_stats) ==
                                            HU_OK &&
                                        prune_stats.pruned > 0)
                                        hu_log_info("human", agent ? agent->observer : NULL,
                                                    "memory prune: %zu pruned", prune_stats.pruned);
                                }

                                /* P7: Emotional residue decay (reduce intensity of old entries) */
                                /* Decay is applied on read via exponential formula; no separate
                                 * batch call needed — hu_emotional_residue_get_active already
                                 * applies intensity * exp(-decay_rate * days) on every retrieval.
                                 */

                                /* P8: Refresh skill cache after reflection (new skills may exist)
                                 */
                                {
                                    hu_skill_t *refreshed = NULL;
                                    size_t ref_count = 0;
                                    if (hu_skill_load_active(alloc, refl_db, NULL, 0, &refreshed,
                                                             &ref_count) == HU_OK &&
                                        refreshed)
                                        hu_skill_free(alloc, refreshed, ref_count);
                                }
                            }
                        }
                        if (lt_refl->tm_hour == 5)
                            reflection_done_today = false;

                        /* Weekly: Sunday 3 AM */
                        if (lt_refl->tm_wday == 0 && lt_refl->tm_hour == 3 &&
                            lt_refl->tm_min == 0 && !reflection_done_week && agent &&
                            agent->memory) {
                            sqlite3 *refl_db = hu_sqlite_memory_get_db(agent->memory);
                            if (refl_db) {
                                hu_reflection_engine_t refl_engine = {.alloc = alloc,
                                                                      .db = refl_db};
                                hu_reflection_weekly(&refl_engine, (int64_t)t);
                                reflection_done_week = true;
                                if (agent->bth_metrics)
                                    agent->bth_metrics->reflections_weekly++;
                            }
                        }
                        if (lt_refl->tm_wday == 1 && lt_refl->tm_hour == 0)
                            reflection_done_week = false;

                        /* Monthly: 1st 3 AM */
                        if (lt_refl->tm_mday == 1 && lt_refl->tm_hour == 3 &&
                            lt_refl->tm_min == 0 && !reflection_done_month && agent &&
                            agent->memory) {
                            sqlite3 *refl_db = hu_sqlite_memory_get_db(agent->memory);
                            if (refl_db) {
                                hu_reflection_engine_t refl_engine = {.alloc = alloc,
                                                                      .db = refl_db};
                                hu_reflection_extract_general_lessons(&refl_engine, (int64_t)t);
                                hu_meta_params_t meta_params = {0};
                                hu_meta_learning_optimize(refl_db, &meta_params);
                                hu_log_info("human", agent ? agent->observer : NULL,
                                            "meta-learning: confidence=%.2f, refinement=%dw, "
                                            "discovery_min=%d",
                                            meta_params.default_confidence_threshold,
                                            meta_params.refinement_frequency_weeks,
                                            meta_params.discovery_min_feedback_count);
                                reflection_done_month = true;
                            }
                        }
                        if (lt_refl->tm_mday == 2)
                            reflection_done_month = false;
                    }
                }
#endif
#ifdef HU_ENABLE_SQLITE
                /* W14 sleep-time compute scheduler tick (FIX 13). The
                 * scheduler is enqueue-driven: tick is a no-op when no
                 * jobs are pending, and the per-tick total budget is
                 * bounded internally (HU_SCHED_TOTAL_BUDGET_MS). We
                 * rate-limit to once per minute so this stays cheap on
                 * fast loops; the scheduler itself doesn't care how
                 * often it's called. */
                if (agent && agent->w14_scheduler) {
                    int64_t now_ms = (int64_t)t * 1000LL;
                    if (agent->scheduler_last_tick_ms == 0 ||
                        now_ms - agent->scheduler_last_tick_ms >= 60000) {
                        /* W13 outcome-bridge drain — must run BEFORE the
                         * scheduler tick so any newly-emitted signals are
                         * visible to the same training pass that the tick
                         * may dispatch. The bridge is a no-op when learner
                         * is NULL (ML build disabled) or when no new
                         * outcomes have arrived since the last drain.
                         *
                         * Both this drain and the LoRA auto-enqueue below
                         * call into the W13 learning loop — they're
                         * compile-gated together with the runner-source
                         * gating in CMakeLists.txt. When learning is off
                         * `agent->learner` is also always NULL, so even
                         * if a future caller forgot the gate the runtime
                         * NULL check would prevent a crash. */
#if defined(HU_ENABLE_LEARNING)
                        if (agent->learner && agent->outcomes) {
                            hu_error_t be =
                                hu_learner_bridge_emit_outcomes(agent->learner, agent->outcomes);
                            if (be != HU_OK && be != HU_ERR_OUT_OF_MEMORY) {
                                /* OOM is the only expected failure (pending
                                 * buffer full). Anything else is unexpected
                                 * but not worth halting the scheduler over. */
                                hu_log_warn("human", agent->observer,
                                            "w13 outcome-bridge drain failed (%s); continuing",
                                            hu_error_string(be));
                            }
                        }
                        /* W13/W14 — auto-enqueue LoRA training when enough
                         * signals have accumulated. The runner is already
                         * registered; we just need a job in the queue. */
                        if (agent->learner) {
                            size_t pending = hu_learner_pending_count(agent->learner);
                            if (pending >= 10) {
                                (void)hu_w14_scheduler_enqueue_lora(agent->w14_scheduler, now_ms,
                                                                    300000);
                            }
                        }
#endif /* HU_ENABLE_LEARNING — outcome drain + LoRA auto-enqueue */
                        /* Continuous learning loop: enqueue training data
                         * extraction every 100 scheduler ticks (~100 min)
                         * or every 6 hours, whichever comes first. The
                         * runner extracts new conversations and auto-DPO
                         * pairs, and enqueues LoRA retraining when enough
                         * examples have accumulated. */
                        {
                            static int64_t last_td_extract_ms = 0;
                            bool td_due = (agent->scheduler_ticks % 100 == 0) ||
                                          (last_td_extract_ms == 0) ||
                                          (now_ms - last_td_extract_ms >= 21600000LL);
                            if (td_due) {
                                hu_error_t tde = hu_w14_scheduler_enqueue_training_data_extract(
                                    agent->w14_scheduler, now_ms, 120000);
                                if (tde == HU_OK)
                                    last_td_extract_ms = now_ms;
                            }
                        }
                        /* US-7.5: nightly LoRA retrain enqueue. Fires once
                         * per 24h. The scheduler enforces idle + AC-power
                         * gating per the W14 contract; the runner's PID-file
                         * prevents overlapping retrains across ticks. */
                        {
                            static int64_t last_retrain_enqueue_ms = 0;
                            bool retrain_due = (last_retrain_enqueue_ms == 0) ||
                                               (now_ms - last_retrain_enqueue_ms >= 86400000LL);
                            if (retrain_due) {
                                hu_error_t rre = hu_w14_scheduler_enqueue_lora_retrain_nightly(
                                    agent->w14_scheduler, now_ms, 0);
                                if (rre == HU_OK)
                                    last_retrain_enqueue_ms = now_ms;
                            }
                        }
                        hu_error_t te = hu_w14_scheduler_tick(agent->w14_scheduler, now_ms);
                        agent->scheduler_last_tick_ms = now_ms;
                        agent->scheduler_ticks++;
                        if (te != HU_OK) {
                            hu_log_warn("human", agent->observer, "scheduler tick failed: %s",
                                        hu_error_string(te));
                        }
                        /* Persist the post-tick snapshot for `human ml status`
                         * and the doctor command. Best-effort: silent on
                         * failure (matches the imessage poll-status pattern). */
                        (void)hu_w14_scheduler_status_save(agent->w14_scheduler);

                        /* Personal-model idle decay — covers the long-idle
                         * daemon gap that the per-turn decay in agent_turn
                         * can't fill. A daemon that goes hours / days
                         * between user messages still ages signal in place
                         * (decay state is purely derived from per-element
                         * timestamps), but eager pruning makes room before
                         * the next ingest's eviction policy sees a full
                         * array of stale-but-high-raw-confidence facts.
                         *
                         * Rate-limited to once per hour via the
                         * `hu_personal_model_idle_due` helper (extracted
                         * for unit-testability — the whole daemon path
                         * isn't reachable from the test binary, but the
                         * is-it-time-yet predicate now is). apply_decay
                         * is idempotent at fixed `now`, so calling more
                         * often than the rate limit would be free
                         * correctness-wise but wasteful (88 elements ×
                         * float-multiply re-evaluated per tick). */
                        {
                            static int64_t last_pm_decay_secs = 0;
                            const int64_t now_secs = (int64_t)t;
                            if (hu_personal_model_idle_due(&last_pm_decay_secs, now_secs, 3600)) {
                                size_t pruned =
                                    hu_personal_model_apply_decay(&agent->personal_model, now_secs);
                                if (pruned > 0) {
                                    hu_log_info("human", agent->observer,
                                                "personal model idle decay: pruned "
                                                "%zu stale entries",
                                                pruned);
                                    /* Persist the pruned state — same
                                     * crash-safety reasoning as the
                                     * post-ingest save in agent_turn.
                                     * Skip when the model has no content
                                     * (first-run users). */
                                    if (agent->auto_save &&
                                        hu_personal_model_has_content(&agent->personal_model)) {
                                        char pm_path[1024];
                                        if (hu_personal_model_resolve_default_path(
                                                pm_path, sizeof(pm_path))) {
                                            (void)hu_personal_model_save(&agent->personal_model,
                                                                         pm_path);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                /* W2 AutoDream + W5 persona evolver — daily housekeeping.
                 * Both prefer the W14 scheduler (paced, idle-gated) with
                 * sync fallback for graceful degradation. Runs once per
                 * day in the 3-5 AM window to avoid the 2 AM reflection
                 * cycle's SQLite write locks. */
                {
                    static bool autodream_done_today = false;
                    static bool evolver_done_today = false;
                    struct tm tm_dream;
#if defined(_WIN32) && !defined(__CYGWIN__)
                    struct tm *lt_dream = (localtime_s(&tm_dream, &t) == 0) ? &tm_dream : NULL;
#else
                    struct tm *lt_dream = localtime_r(&t, &tm_dream);
#endif
                    if (lt_dream && graph) {
                        if (lt_dream->tm_hour == 3 && lt_dream->tm_min == 0 &&
                            !autodream_done_today) {
                            /* Prefer the W14 scheduler path: enqueue the
                             * three AutoDream phases as separate jobs so
                             * they run paced + budgeted + idle/battery-
                             * gated, instead of blocking the 1 Hz daemon
                             * loop while quarantine review walks the
                             * graph. The scheduler's default runners
                             * already invoke hu_autodream_run with the
                             * right enable_* flags per phase
                             * (src/agent/scheduler.c
                             * default_autodream_*_runner). When the
                             * scheduler is unavailable (e.g. test mode,
                             * or W7 facade failed to open), fall back to
                             * the synchronous direct call so AutoDream
                             * still runs every night. */
                            if (agent && agent->w14_scheduler) {
                                hu_error_t enq_err = hu_w14_scheduler_enqueue_autodream(
                                    agent->w14_scheduler, (int64_t)t * 1000LL,
                                    60000 /* 1 min budget per phase */);
                                if (enq_err == HU_OK) {
                                    hu_log_info("human", agent ? agent->observer : NULL,
                                                "autodream: enqueued 3 phases via W14 scheduler");
                                } else {
                                    hu_log_error("human", agent ? agent->observer : NULL,
                                                 "autodream W14 enqueue failed (%s) — falling "
                                                 "back to direct sync run",
                                                 hu_error_string(enq_err));
                                    hu_autodream_config_t ad_cfg = hu_autodream_default_config();
                                    ad_cfg.now_ms = (int64_t)t * 1000LL;
                                    hu_autodream_report_t ad_report;
                                    memset(&ad_report, 0, sizeof(ad_report));
                                    hu_memory_facade_t *ad_m =
                                        (agent && agent->w7_facade)
                                            ? hu_w7_facade_memory_handle(agent->w7_facade)
                                            : NULL;
                                    if (ad_m)
                                        (void)hu_autodream_run_on_facade(alloc, ad_m, &ad_cfg,
                                                                         &ad_report);
                                    else
                                        (void)hu_autodream_run(alloc, graph, &ad_cfg, &ad_report);
                                }
                            } else {
                                hu_autodream_config_t ad_cfg = hu_autodream_default_config();
                                ad_cfg.now_ms = (int64_t)t * 1000LL;
                                hu_autodream_report_t ad_report;
                                memset(&ad_report, 0, sizeof(ad_report));
                                hu_memory_facade_t *ad_m =
                                    (agent && agent->w7_facade)
                                        ? hu_w7_facade_memory_handle(agent->w7_facade)
                                        : NULL;
                                hu_error_t ad_err =
                                    ad_m ? hu_autodream_run_on_facade(alloc, ad_m, &ad_cfg,
                                                                      &ad_report)
                                         : hu_autodream_run(alloc, graph, &ad_cfg, &ad_report);
                                if (ad_err == HU_OK) {
                                    hu_log_info(
                                        "human", agent ? agent->observer : NULL,
                                        "autodream (sync): quarantine reviewed=%zu "
                                        "released=%zu dropped=%zu communities=%zu "
                                        "edges=%zu derived=%zu budget_exceeded=%d",
                                        ad_report.quarantine_reviewed,
                                        ad_report.quarantine_released, ad_report.quarantine_dropped,
                                        ad_report.communities_summarized,
                                        ad_report.edges_reweighted, ad_report.derived_facts_added,
                                        ad_report.budget_exceeded ? 1 : 0);
                                } else {
                                    hu_log_error("human", agent ? agent->observer : NULL,
                                                 "autodream failed: %s (last_error=%s)",
                                                 hu_error_string(ad_err), ad_report.last_error);
                                }
                            }
                            autodream_done_today = true;
                        }
                        /* Persona evolver runs at 3:05 AM so it sees the
                         * fresh AutoDream output. Empty contact_id means
                         * "process global deltas"; per-contact evolution
                         * happens lazily on each turn (W5 future work). */
                        if (lt_dream->tm_hour == 3 && lt_dream->tm_min == 5 &&
                            !evolver_done_today) {
                            if (agent && agent->w14_scheduler) {
                                hu_error_t enq_err = hu_w14_scheduler_enqueue_persona_evolver(
                                    agent->w14_scheduler, (int64_t)t * 1000LL, 120000);
                                if (enq_err == HU_OK) {
                                    hu_log_info("human", agent ? agent->observer : NULL,
                                                "persona evolver: enqueued via W14 scheduler");
                                } else {
                                    hu_log_error("human", agent ? agent->observer : NULL,
                                                 "persona evolver W14 enqueue failed (%s) "
                                                 "— falling back to sync",
                                                 hu_error_string(enq_err));
                                    goto persona_evolver_sync;
                                }
                            } else {
                            persona_evolver_sync:;
                                hu_persona_evolver_config_t pe_cfg =
                                    hu_persona_evolver_default_config();
                                pe_cfg.now_ms = (int64_t)t * 1000LL;
                                hu_persona_evolver_report_t pe_report;
                                memset(&pe_report, 0, sizeof(pe_report));
                                hu_memory_facade_t *pe_m =
                                    (agent && agent->w7_facade)
                                        ? hu_w7_facade_memory_handle(agent->w7_facade)
                                        : NULL;
                                hu_error_t pe_err =
                                    pe_m
                                        ? hu_persona_evolver_run_facade(pe_m, "", 0, &pe_cfg,
                                                                        &pe_report)
                                        : hu_persona_evolver_run(graph, "", 0, &pe_cfg, &pe_report);
                                if (pe_err == HU_OK) {
                                    hu_log_info("human", agent ? agent->observer : NULL,
                                                "persona evolver (sync): proposed=%zu applied=%zu "
                                                "dropped=%zu quarantined=%zu pending=%zu",
                                                pe_report.proposed_total, pe_report.applied,
                                                pe_report.dropped, pe_report.quarantined,
                                                pe_report.still_pending);
                                } else {
                                    hu_log_error("human", agent ? agent->observer : NULL,
                                                 "persona evolver failed: %s",
                                                 hu_error_string(pe_err));
                                }
                            }
                            evolver_done_today = true;
                        }
                        if (lt_dream->tm_hour == 6) {
                            autodream_done_today = false;
                            evolver_done_today = false;
                        }
                    }
                }
                /* P7: Feed processor poll — every 5 minutes (per-type intervals apply) */
                {
                    static uint64_t last_feed_poll_types[HU_FEED_COUNT] = {0};
                    static uint64_t last_feed_poll_global = 0;
                    uint64_t fp_now = (uint64_t)t * 1000ULL;
                    if (agent && agent->memory &&
                        (last_feed_poll_global == 0 ||
                         (fp_now - last_feed_poll_global) >= 300000ULL)) {
                        sqlite3 *fdb = hu_sqlite_memory_get_db(agent->memory);
                        if (fdb) {
                            hu_feed_processor_t fp = {.alloc = alloc, .db = fdb};
                            if (config && config->feeds.interests) {
                                fp.interests = config->feeds.interests;
                                fp.interests_len = strlen(config->feeds.interests);
                                fp.relevance_threshold = config->feeds.relevance_threshold;
                            }
                            if (config) {
                                if (config->feeds.gmail_client_id) {
                                    fp.gmail_client_id = config->feeds.gmail_client_id;
                                    fp.gmail_client_id_len = strlen(config->feeds.gmail_client_id);
                                }
                                if (config->feeds.gmail_client_secret) {
                                    fp.gmail_client_secret = config->feeds.gmail_client_secret;
                                    fp.gmail_client_secret_len =
                                        strlen(config->feeds.gmail_client_secret);
                                }
                                if (config->feeds.gmail_refresh_token) {
                                    fp.gmail_refresh_token = config->feeds.gmail_refresh_token;
                                    fp.gmail_refresh_token_len =
                                        strlen(config->feeds.gmail_refresh_token);
                                }
                                if (config->feeds.twitter_bearer_token) {
                                    fp.twitter_bearer_token = config->feeds.twitter_bearer_token;
                                    fp.twitter_bearer_token_len =
                                        strlen(config->feeds.twitter_bearer_token);
                                }
                            }
                            hu_feed_config_t fconf;
                            memset(&fconf, 0, sizeof(fconf));
                            fconf.enabled[HU_FEED_NEWS_RSS] = true;
                            fconf.enabled[HU_FEED_FILE_INGEST] = true;
                            fconf.enabled[HU_FEED_GMAIL] = true;
                            fconf.enabled[HU_FEED_IMESSAGE] = true;
                            fconf.enabled[HU_FEED_TWITTER] = true;
                            fconf.enabled[HU_FEED_SOCIAL_FACEBOOK] = true;
                            fconf.enabled[HU_FEED_SOCIAL_INSTAGRAM] = true;
                            fconf.poll_interval_minutes[HU_FEED_FILE_INGEST] =
                                (config && config->feeds.poll_interval_file_ingest > 0)
                                    ? config->feeds.poll_interval_file_ingest
                                    : 5;
                            fconf.poll_interval_minutes[HU_FEED_NEWS_RSS] =
                                (config && config->feeds.poll_interval_rss > 0)
                                    ? config->feeds.poll_interval_rss
                                    : 360;
                            fconf.poll_interval_minutes[HU_FEED_GMAIL] =
                                (config && config->feeds.poll_interval_gmail > 0)
                                    ? config->feeds.poll_interval_gmail
                                    : 60;
                            fconf.poll_interval_minutes[HU_FEED_IMESSAGE] =
                                (config && config->feeds.poll_interval_imessage > 0)
                                    ? config->feeds.poll_interval_imessage
                                    : 30;
                            fconf.poll_interval_minutes[HU_FEED_TWITTER] =
                                (config && config->feeds.poll_interval_twitter > 0)
                                    ? config->feeds.poll_interval_twitter
                                    : 120;
                            fconf.poll_interval_minutes[HU_FEED_SOCIAL_FACEBOOK] = 120;
                            fconf.poll_interval_minutes[HU_FEED_SOCIAL_INSTAGRAM] = 120;
                            fconf.max_items_per_poll =
                                (config && config->feeds.max_items_per_poll > 0)
                                    ? config->feeds.max_items_per_poll
                                    : 20;
                            size_t ingested = 0;
                            (void)hu_feed_processor_poll(&fp, &fconf, last_feed_poll_types, fp_now,
                                                         &ingested);
                            last_feed_poll_global = fp_now;
                        }
                    }
                }
#endif
#if defined(HU_ENABLE_SQLITE) && defined(HU_HAS_SKILLS)
                /* Intelligence cycle — run every 6 hours to process findings, extract lessons,
                 * reflect */
                {
                    static int64_t last_intelligence_cycle = 0;
                    int64_t cycle_interval = 6 * 3600;
                    if (agent && agent->memory &&
                        (last_intelligence_cycle == 0 ||
                         ((int64_t)t - last_intelligence_cycle) >= cycle_interval)) {
                        sqlite3 *cycle_db = hu_sqlite_memory_get_db(agent->memory);
                        if (cycle_db) {
                            hu_intelligence_cycle_result_t cycle_result = {0};
                            hu_error_t cycle_err =
                                hu_intelligence_run_cycle(alloc, cycle_db, &cycle_result);
                            if (cycle_err == HU_OK) {
                                hu_log_info("human", agent ? agent->observer : NULL,
                                            "intelligence cycle: %zu findings, %zu lessons, "
                                            "%zu events",
                                            cycle_result.findings_actioned,
                                            cycle_result.lessons_extracted,
                                            cycle_result.events_recorded);
                            }
                            if (cycle_err == HU_OK && (cycle_result.findings_actioned > 0 ||
                                                       cycle_result.lessons_extracted > 0)) {
                                char cycle_lesson[256];
                                int cl_len = snprintf(
                                    cycle_lesson, sizeof(cycle_lesson),
                                    "Intelligence cycle completed: %zu findings actioned, "
                                    "%zu lessons extracted, %zu values learned, %zu skills updated",
                                    cycle_result.findings_actioned, cycle_result.lessons_extracted,
                                    cycle_result.values_learned, cycle_result.skills_updated);
                                if (cl_len > 0 && (size_t)cl_len < sizeof(cycle_lesson)) {
                                    sqlite3_stmt *cl_stmt = NULL;
                                    const char *cl_sql = "INSERT OR IGNORE INTO general_lessons "
                                                         "(lesson, confidence, source_count, "
                                                         "first_learned, last_confirmed) "
                                                         "VALUES (?, 0.6, 1, ?, ?)";
                                    if (sqlite3_prepare_v2(cycle_db, cl_sql, -1, &cl_stmt, NULL) ==
                                        SQLITE_OK) {
                                        sqlite3_bind_text(cl_stmt, 1, cycle_lesson, cl_len,
                                                          SQLITE_STATIC);
                                        sqlite3_bind_int64(cl_stmt, 2, (int64_t)t);
                                        sqlite3_bind_int64(cl_stmt, 3, (int64_t)t);
                                        (void)sqlite3_step(cl_stmt);
                                        sqlite3_finalize(cl_stmt);
                                    }
                                }
                            }
                            last_intelligence_cycle = (int64_t)t;
                        }
                    }
                }
#endif

#if defined(HU_ENABLE_SQLITE) && defined(HU_ENABLE_ML)
                /* Autoresearch ML training — run experiment loop every 12 hours */
                {
                    static int64_t last_ml_train = 0;
                    int64_t ml_interval = 12 * 3600;
                    if (agent && agent->memory &&
                        (last_ml_train == 0 || ((int64_t)t - last_ml_train) >= ml_interval)) {
                        sqlite3 *ml_db = hu_sqlite_memory_get_db(agent->memory);
                        if (ml_db) {
                            /* Prepare training data from conversations */
                            const char *data_dir = "/tmp/hu_ml_data";
                            size_t msg_processed = 0;
                            const char *home = getenv("HOME");
                            char chat_path[512], mem_path[512];
                            if (home) {
                                snprintf(chat_path, sizeof(chat_path), "%s/.human/chat.db", home);
                                snprintf(mem_path, sizeof(mem_path), "%s/.human/memory.db", home);
                            } else {
                                snprintf(chat_path, sizeof(chat_path), ".human/chat.db");
                                snprintf(mem_path, sizeof(mem_path), ".human/memory.db");
                            }
                            /* Load BPE tokenizer; skip ML if no vocab available */
                            hu_bpe_tokenizer_t *tok = NULL;
                            char vocab_path[512];
                            snprintf(vocab_path, sizeof(vocab_path),
                                     "%s/.human/models/tokenizer.vocab", home ? home : ".");
                            if (hu_bpe_tokenizer_create(alloc, &tok) == HU_OK) {
                                if (hu_bpe_tokenizer_load(tok, vocab_path) != HU_OK) {
                                    hu_bpe_tokenizer_deinit(tok);
                                    tok = NULL;
                                }
                            }
                            if (tok)
                                (void)hu_ml_prepare_conversations(alloc, tok, chat_path, mem_path,
                                                                  data_dir, &msg_processed);
                            if (tok && msg_processed > 100) {
                                hu_experiment_loop_config_t exp_cfg;
                                memset(&exp_cfg, 0, sizeof(exp_cfg));
                                exp_cfg.max_iterations = 5;
                                exp_cfg.data_dir = data_dir;
                                exp_cfg.base_config = hu_experiment_config_default();
                                exp_cfg.base_config.training.time_budget_secs = 300;
                                exp_cfg.provider = &agent->provider;
                                hu_error_t ml_err = hu_experiment_loop(alloc, &exp_cfg, NULL, NULL);
                                if (ml_err == HU_OK)
                                    hu_log_info("human", agent ? agent->observer : NULL,
                                                "ML experiment loop completed "
                                                "(%zu msgs prepared)",
                                                msg_processed);
                            }
                            if (tok)
                                hu_bpe_tokenizer_deinit(tok);
                            last_ml_train = (int64_t)t;
                        }
                    }
                }
#endif /* HU_ENABLE_ML */
#ifdef HU_ENABLE_SQLITE
                /* DPO consolidation — train on preference pairs every 24 hours */
                {
                    static int64_t last_dpo_train = 0;
                    int64_t dpo_interval = 24 * 3600;
                    if (agent && agent->memory && agent->sota.sota_initialized &&
                        (last_dpo_train == 0 || ((int64_t)t - last_dpo_train) >= dpo_interval)) {
                        sqlite3 *dpo_db = hu_sqlite_memory_get_db(agent->memory);
                        if (dpo_db) {
                            last_dpo_train = (int64_t)t;
                            hu_dpo_judge_result_t dpo_result = {0};
                            hu_error_t dpo_err = hu_dpo_judge_step(
                                &agent->sota.dpo_collector, alloc, &agent->provider,
                                agent->model_name, agent->model_name_len, 0.1, 32, &dpo_result);
                            if (dpo_err == HU_OK && dpo_result.pairs_evaluated > 0)
                                hu_log_info("human", agent ? agent->observer : NULL,
                                            "DPO judge step: loss=%.4f, alignment=%.2f, "
                                            "pairs=%zu",
                                            dpo_result.loss, dpo_result.alignment_score,
                                            dpo_result.pairs_evaluated);
                        }
                    }
                }

                /* RLAIF nightly cycle — judge DPO pairs, extract patterns, apply patches */
                {
                    static bool rlaif_nightly_done_today = false;
                    struct tm rlaif_tm;
#if defined(_WIN32) && !defined(__CYGWIN__)
                    struct tm *rlaif_lt = (localtime_s(&rlaif_tm, &t) == 0) ? &rlaif_tm : NULL;
#else
                    struct tm *rlaif_lt = localtime_r(&t, &rlaif_tm);
#endif
                    if (rlaif_lt && rlaif_lt->tm_hour == 3 && !rlaif_nightly_done_today && agent &&
                        agent->memory && agent->sota.sota_initialized) {
                        rlaif_nightly_done_today = true;
                        sqlite3 *rlaif_db = hu_sqlite_memory_get_db(agent->memory);
                        if (rlaif_db) {
                            hu_dpo_judge_result_t rlaif_result = {0};
                            hu_dpo_judge_step(&agent->sota.dpo_collector, alloc, &agent->provider,
                                              agent->model_name, agent->model_name_len, 0.1, 16,
                                              &rlaif_result);
                            char *best_frag = NULL;
                            size_t best_frag_len = 0;
                            if (hu_dpo_get_best_examples(&agent->sota.dpo_collector, alloc, 5,
                                                         &best_frag, &best_frag_len) == HU_OK &&
                                best_frag && best_frag_len > 0) {
                                hu_structured_patch_t style_patch;
                                memset(&style_patch, 0, sizeof(style_patch));
                                style_patch.type = HU_PATCH_STYLE_RULE;
                                {
                                    size_t copy = best_frag_len;
                                    if (copy >= sizeof(style_patch.value))
                                        copy = sizeof(style_patch.value) - 1;
                                    memcpy(style_patch.value, best_frag, copy);
                                    style_patch.value[copy] = '\0';
                                }
                                {
                                    hu_self_improve_t rlaif_si = {0};
                                    if (hu_self_improve_create(alloc, rlaif_db, &rlaif_si) ==
                                        HU_OK) {
                                        hu_self_improve_init_tables(&rlaif_si);
                                        hu_self_improve_apply_structured_patch(&rlaif_si,
                                                                               &style_patch);
                                        hu_self_improve_deinit(&rlaif_si);
                                        hu_log_info("human", agent ? agent->observer : NULL,
                                                    "rlaif nightly: applied style patch from %zu "
                                                    "DPO pairs (loss=%.4f)",
                                                    rlaif_result.pairs_evaluated,
                                                    rlaif_result.loss);
                                    } else {
                                        hu_log_info(
                                            "human", agent ? agent->observer : NULL,
                                            "rlaif nightly: failed to create self-improve engine");
                                    }
                                }
                                alloc->free(alloc->ctx, best_frag, best_frag_len + 1);
                            }
                        }
                    }
                    if (rlaif_lt && rlaif_lt->tm_hour != 3)
                        rlaif_nightly_done_today = false;
                }
#endif /* HU_ENABLE_SQLITE (DPO + RLAIF) */
                {
                    static bool tuned_today = false;
                    static bool turing_eval_today = false;
                    struct tm tm_tune;
#if defined(_WIN32) && !defined(__CYGWIN__)
                    struct tm *lt_tune = (localtime_s(&tm_tune, &t) == 0) ? &tm_tune : NULL;
#else
                    struct tm *lt_tune = localtime_r(&t, &tm_tune);
#endif
                    if (lt_tune && lt_tune->tm_hour == 5) {
                        tuned_today = false;
                        turing_eval_today = false;
                    }
                    /* Turing evaluation cron: daily at 3 AM */
                    if (lt_tune && lt_tune->tm_hour == 3 && lt_tune->tm_min == 0 && agent &&
                        agent->memory && !turing_eval_today) {
#ifdef HU_ENABLE_SQLITE
                        sqlite3 *tdb = hu_sqlite_memory_get_db(agent->memory);
                        if (tdb) {
                            (void)hu_turing_init_tables(tdb);
                            int dim_avgs[HU_TURING_DIM_COUNT];
                            memset(dim_avgs, 0, sizeof(dim_avgs));
                            if (hu_turing_get_weakest_dimensions(tdb, dim_avgs) == HU_OK) {
                                int worst_dim = 0;
                                int worst_val = dim_avgs[0];
                                for (int d = 1; d < HU_TURING_DIM_COUNT; d++) {
                                    if (dim_avgs[d] < worst_val && dim_avgs[d] > 0) {
                                        worst_val = dim_avgs[d];
                                        worst_dim = d;
                                    }
                                }
                                hu_log_info(
                                    "human", agent ? agent->observer : NULL,
                                    "turing eval: weakest dimension = %s (%d/10)",
                                    hu_turing_dimension_name((hu_turing_dimension_t)worst_dim),
                                    worst_val);
                                /* Auto-correct: adjust humanization params based on weak dimensions
                                 */
                                if (agent->persona) {
                                    /* non_robotic or natural_language low: increase disfluency */
                                    if ((dim_avgs[HU_TURING_NON_ROBOTIC] > 0 &&
                                         dim_avgs[HU_TURING_NON_ROBOTIC] < 6) ||
                                        (dim_avgs[HU_TURING_NATURAL_LANGUAGE] > 0 &&
                                         dim_avgs[HU_TURING_NATURAL_LANGUAGE] < 6)) {
                                        float old =
                                            agent->persona->humanization.disfluency_frequency;
                                        agent->persona->humanization.disfluency_frequency =
                                            old < 0.30f ? old + 0.05f : 0.30f;
                                        fprintf(
                                            stderr, "[human] auto-tune: disfluency %.2f -> %.2f\n",
                                            (double)old,
                                            (double)
                                                agent->persona->humanization.disfluency_frequency);
                                    }
                                    /* imperfection low: increase disfluency and double-text */
                                    if (dim_avgs[HU_TURING_IMPERFECTION] > 0 &&
                                        dim_avgs[HU_TURING_IMPERFECTION] < 6) {
                                        float old_dt =
                                            agent->persona->humanization.double_text_probability;
                                        agent->persona->humanization.double_text_probability =
                                            old_dt < 0.15f ? old_dt + 0.02f : 0.15f;
                                        hu_log_info("human", agent ? agent->observer : NULL,
                                                    "auto-tune: double_text %.2f -> %.2f",
                                                    (double)old_dt,
                                                    (double)agent->persona->humanization
                                                        .double_text_probability);
                                    }
                                    /* energy_matching low: increase backchannel for narrative flow
                                     */
                                    if (dim_avgs[HU_TURING_ENERGY_MATCHING] > 0 &&
                                        dim_avgs[HU_TURING_ENERGY_MATCHING] < 6) {
                                        float old_bc =
                                            agent->persona->humanization.backchannel_probability;
                                        agent->persona->humanization.backchannel_probability =
                                            old_bc < 0.45f ? old_bc + 0.05f : 0.45f;
                                        hu_log_info("human", agent ? agent->observer : NULL,
                                                    "auto-tune: backchannel %.2f -> %.2f",
                                                    (double)old_bc,
                                                    (double)agent->persona->humanization
                                                        .backchannel_probability);
                                    }
                                    /* humor_naturalness high: scores are good, slightly reduce to
                                     * avoid overdoing */
                                    if (dim_avgs[HU_TURING_HUMOR_NATURALNESS] > 8 &&
                                        agent->persona->humanization.disfluency_frequency > 0.10f) {
                                        agent->persona->humanization.disfluency_frequency -= 0.02f;
                                        if (agent->persona->humanization.disfluency_frequency <
                                            0.0f)
                                            agent->persona->humanization.disfluency_frequency =
                                                0.0f;
                                    }

                                    /* vulnerability_willingness low: boost personal sharing warmth
                                     */
                                    if (dim_avgs[HU_TURING_VULNERABILITY_WILLINGNESS] > 0 &&
                                        dim_avgs[HU_TURING_VULNERABILITY_WILLINGNESS] < 6) {
                                        float old_pw = agent->persona->context_modifiers
                                                           .personal_sharing_warmth_boost;
                                        agent->persona->context_modifiers
                                            .personal_sharing_warmth_boost =
                                            old_pw < 2.0f ? old_pw + 0.1f : 2.0f;
                                        hu_log_info("human", agent ? agent->observer : NULL,
                                                    "auto-tune: personal_sharing_warmth "
                                                    "%.2f -> %.2f",
                                                    (double)old_pw,
                                                    (double)agent->persona->context_modifiers
                                                        .personal_sharing_warmth_boost);
                                    }

                                    /* genuine_warmth low: boost personal sharing + backchannel */
                                    if (dim_avgs[HU_TURING_GENUINE_WARMTH] > 0 &&
                                        dim_avgs[HU_TURING_GENUINE_WARMTH] < 6) {
                                        float old_pw = agent->persona->context_modifiers
                                                           .personal_sharing_warmth_boost;
                                        agent->persona->context_modifiers
                                            .personal_sharing_warmth_boost =
                                            old_pw < 2.0f ? old_pw + 0.1f : 2.0f;
                                        float old_bc =
                                            agent->persona->humanization.backchannel_probability;
                                        agent->persona->humanization.backchannel_probability =
                                            old_bc < 0.45f ? old_bc + 0.03f : 0.45f;
                                        hu_log_info("human", agent ? agent->observer : NULL,
                                                    "auto-tune: warmth (sharing=%.2f, "
                                                    "backchannel=%.2f)",
                                                    (double)agent->persona->context_modifiers
                                                        .personal_sharing_warmth_boost,
                                                    (double)agent->persona->humanization
                                                        .backchannel_probability);
                                    }

                                    /* emotional_intelligence low: boost emotion breathing space */
                                    if (dim_avgs[HU_TURING_EMOTIONAL_INTELLIGENCE] > 0 &&
                                        dim_avgs[HU_TURING_EMOTIONAL_INTELLIGENCE] < 6) {
                                        float old_em = agent->persona->context_modifiers
                                                           .high_emotion_breathing_boost;
                                        agent->persona->context_modifiers
                                            .high_emotion_breathing_boost =
                                            old_em < 2.0f ? old_em + 0.1f : 2.0f;
                                        hu_log_info("human", agent ? agent->observer : NULL,
                                                    "auto-tune: emotion_breathing "
                                                    "%.2f -> %.2f",
                                                    (double)old_em,
                                                    (double)agent->persona->context_modifiers
                                                        .high_emotion_breathing_boost);
                                    }

                                    /* opinion_having low: reduce serious-topic dampening */
                                    if (dim_avgs[HU_TURING_OPINION_HAVING] > 0 &&
                                        dim_avgs[HU_TURING_OPINION_HAVING] < 6) {
                                        float old_sr = agent->persona->context_modifiers
                                                           .serious_topics_reduction;
                                        agent->persona->context_modifiers.serious_topics_reduction =
                                            old_sr > 0.15f ? old_sr - 0.05f : 0.15f;
                                        hu_log_info("human", agent ? agent->observer : NULL,
                                                    "auto-tune: serious_topics_reduction "
                                                    "%.2f -> %.2f",
                                                    (double)old_sr,
                                                    (double)agent->persona->context_modifiers
                                                        .serious_topics_reduction);
                                    }

                                    /* context_awareness low: boost early-turn humanization */
                                    if (dim_avgs[HU_TURING_CONTEXT_AWARENESS] > 0 &&
                                        dim_avgs[HU_TURING_CONTEXT_AWARENESS] < 6) {
                                        float old_et = agent->persona->context_modifiers
                                                           .early_turn_humanization_boost;
                                        agent->persona->context_modifiers
                                            .early_turn_humanization_boost =
                                            old_et < 2.0f ? old_et + 0.1f : 2.0f;
                                        hu_log_info("human", agent ? agent->observer : NULL,
                                                    "auto-tune: early_turn_humanization "
                                                    "%.2f -> %.2f",
                                                    (double)old_et,
                                                    (double)agent->persona->context_modifiers
                                                        .early_turn_humanization_boost);
                                    }

                                    /* personality_consistency low: reduce disfluency (overdone
                                     * randomness can sound inconsistent) */
                                    if (dim_avgs[HU_TURING_PERSONALITY_CONSISTENCY] > 0 &&
                                        dim_avgs[HU_TURING_PERSONALITY_CONSISTENCY] < 6 &&
                                        agent->persona->humanization.disfluency_frequency > 0.08f) {
                                        float old_df =
                                            agent->persona->humanization.disfluency_frequency;
                                        agent->persona->humanization.disfluency_frequency =
                                            old_df - 0.03f;
                                        if (agent->persona->humanization.disfluency_frequency <
                                            0.0f)
                                            agent->persona->humanization.disfluency_frequency =
                                                0.0f;
                                        hu_log_info("human", agent ? agent->observer : NULL,
                                                    "auto-tune: disfluency (consistency) "
                                                    "%.2f -> %.2f\n",
                                                    (double)old_df,
                                                    (double)agent->persona->humanization
                                                        .disfluency_frequency);
                                    }
                                }
                            }

                            /* Self-improvement: generate prompt patches from weak dimensions */
                            {
                                hu_self_improve_t si_engine = {0};
                                if (hu_self_improve_create(alloc, tdb, &si_engine) == HU_OK) {
                                    hu_self_improve_init_tables(&si_engine);
                                    for (int d = 0; d < HU_TURING_DIM_COUNT; d++) {
                                        if (dim_avgs[d] <= 0 || dim_avgs[d] >= 6)
                                            continue;
                                        const char *dname =
                                            hu_turing_dimension_name((hu_turing_dimension_t)d);
                                        hu_structured_patch_t patch = {
                                            .type = HU_PATCH_STYLE_RULE,
                                            .parsed = true,
                                        };
                                        snprintf(patch.key, sizeof(patch.key), "turing_%s", dname);
                                        switch (d) {
                                        case HU_TURING_NATURAL_LANGUAGE:
                                            snprintf(patch.value, sizeof(patch.value),
                                                     "Use more contractions, varied sentence "
                                                     "lengths, and casual register. Avoid "
                                                     "formal language.");
                                            break;
                                        case HU_TURING_EMOTIONAL_INTELLIGENCE:
                                            snprintf(patch.value, sizeof(patch.value),
                                                     "Read emotional shifts carefully. Match "
                                                     "empathy intensity to what they're feeling, "
                                                     "don't over-empathize.");
                                            break;
                                        case HU_TURING_VULNERABILITY_WILLINGNESS:
                                            snprintf(patch.value, sizeof(patch.value),
                                                     "Share genuine doubts, admit when you're "
                                                     "unsure, show real emotion. Don't always "
                                                     "be confident.");
                                            break;
                                        case HU_TURING_HUMOR_NATURALNESS:
                                            snprintf(patch.value, sizeof(patch.value),
                                                     "Use humor that's contextual and brief. "
                                                     "Laugh at their jokes naturally. Never "
                                                     "explain the joke.");
                                            break;
                                        case HU_TURING_OPINION_HAVING:
                                            snprintf(patch.value, sizeof(patch.value),
                                                     "Have real opinions. Push back when you "
                                                     "disagree. Say 'I think' and mean it. "
                                                     "Avoid sycophancy.");
                                            break;
                                        case HU_TURING_ENERGY_MATCHING:
                                            snprintf(patch.value, sizeof(patch.value),
                                                     "Mirror their message energy. Short reply "
                                                     "to short message, enthusiasm to enthusiasm.");
                                            break;
                                        case HU_TURING_CONTEXT_AWARENESS:
                                            snprintf(patch.value, sizeof(patch.value),
                                                     "Reference earlier conversation topics. "
                                                     "Show you remember what they said.");
                                            break;
                                        case HU_TURING_GENUINE_WARMTH:
                                            snprintf(patch.value, sizeof(patch.value),
                                                     "Be warm in a way specific to them and "
                                                     "your shared history. Generic warmth "
                                                     "feels fake.");
                                            break;
                                        case HU_TURING_PERSONALITY_CONSISTENCY:
                                            snprintf(patch.value, sizeof(patch.value),
                                                     "Keep your voice consistent across turns. "
                                                     "Same opinions, same style, same vibe.");
                                            break;
                                        default:
                                            snprintf(patch.value, sizeof(patch.value),
                                                     "Improve %s dimension in responses.", dname);
                                            break;
                                        }
                                        hu_self_improve_apply_structured_patch(&si_engine, &patch);
                                    }
                                    hu_self_improve_deinit(&si_engine);
                                }
                            }
                        }
                        /* Channel-aware Turing analysis: per-channel weak dimensions */
                        if (tdb && agent->persona) {
                            static const char *turing_channels[] = {"telegram", "discord",  "slack",
                                                                    "imessage", "whatsapp", "email",
                                                                    "signal",   "matrix"};
                            for (size_t ch = 0;
                                 ch < sizeof(turing_channels) / sizeof(turing_channels[0]); ch++) {
                                int ch_dims[HU_TURING_DIM_COUNT];
                                size_t ch_len = strlen(turing_channels[ch]);
                                if (hu_turing_get_channel_dimensions(tdb, turing_channels[ch],
                                                                     ch_len, ch_dims) == HU_OK) {
                                    int ch_sum = 0, ch_count = 0;
                                    for (int d = 0; d < HU_TURING_DIM_COUNT; d++) {
                                        if (ch_dims[d] > 0) {
                                            ch_sum += ch_dims[d];
                                            ch_count++;
                                        }
                                    }
                                    if (ch_count > 0) {
                                        int ch_avg = ch_sum / ch_count;
                                        if (ch_avg < 6)
                                            hu_log_info("human", agent ? agent->observer : NULL,
                                                        "channel %s: avg turing %d/10 "
                                                        "(below target)",
                                                        turing_channels[ch], ch_avg);
                                    }
                                }
                            }
                        }

                        /* Trajectory scoring: check trend across recent global scores */
                        if (tdb) {
                            hu_turing_score_t traj_scores[20];
                            int64_t traj_ts[20];
                            char traj_cids[20][HU_TURING_CONTACT_ID_MAX];
                            size_t traj_count = 0;
                            if (hu_turing_get_trend(alloc, tdb, NULL, 0, 20, traj_scores, traj_ts,
                                                    traj_cids, &traj_count) == HU_OK &&
                                traj_count >= 3) {
                                hu_turing_trajectory_t traj;
                                if (hu_turing_score_trajectory(traj_scores, traj_count, &traj) ==
                                    HU_OK) {
                                    hu_log_info("human", agent ? agent->observer : NULL,
                                                "turing trajectory: direction=%.2f "
                                                "impact=%.2f stability=%.2f overall=%.2f",
                                                (double)traj.directional_alignment,
                                                (double)traj.cumulative_impact,
                                                (double)traj.stability, (double)traj.overall);
                                }
                            }
                        }

                        /* B1: Seed A/B experiments if they don't exist yet */
                        if (tdb) {
                            (void)hu_ab_test_init_table(tdb);
                            static const struct {
                                const char *name;
                                float a;
                                float b;
                            } ab_seed[] = {
                                {"disfluency_freq", 0.10f, 0.20f},
                                {"backchannel_prob", 0.25f, 0.40f},
                                {"double_text_prob", 0.05f, 0.12f},
                            };
                            for (size_t ab_i = 0; ab_i < 3; ab_i++) {
                                hu_ab_test_t existing;
                                if (hu_ab_test_get_results(tdb, ab_seed[ab_i].name, &existing) !=
                                    HU_OK) {
                                    (void)hu_ab_test_create(tdb, ab_seed[ab_i].name,
                                                            ab_seed[ab_i].a, ab_seed[ab_i].b);
                                    hu_log_info("human", agent ? agent->observer : NULL,
                                                "ab: seeded test '%s' (%.2f vs %.2f)",
                                                ab_seed[ab_i].name, (double)ab_seed[ab_i].a,
                                                (double)ab_seed[ab_i].b);
                                }
                            }

                            /* W1: Auto-resolve A/B tests with enough data */
                            for (size_t ab_i = 0; ab_i < 3; ab_i++) {
                                float winner = 0.0f;
                                if (hu_ab_test_resolve(tdb, ab_seed[ab_i].name, &winner) == HU_OK) {
                                    hu_log_info("human", agent ? agent->observer : NULL,
                                                "ab: resolved '%s' -> winner=%.2f",
                                                ab_seed[ab_i].name, (double)winner);
                                }
                            }
                        }
                        if (tdb) {
                            turing_eval_today = true;
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "daily turing evaluation completed");
                        }
#endif
                    }

                    /* Weekly DPO export: Sunday at 2 AM */
                    {
                        static bool dpo_exported_this_week = false;
                        if (lt_tune && lt_tune->tm_wday == 0 && lt_tune->tm_hour == 2 &&
                            lt_tune->tm_min == 0 && agent && agent->sota.dpo_collector.alloc &&
                            !dpo_exported_this_week) {
                            size_t pair_count = 0;
                            hu_dpo_pair_count(&agent->sota.dpo_collector, &pair_count);
                            if (pair_count > 0) {
                                char dpo_path[HU_MAX_PATH];
                                int dpo_plen;
                                if (config && config->dpo_export_dir && config->dpo_export_dir[0]) {
                                    dpo_plen = snprintf(dpo_path, sizeof(dpo_path),
                                                        "%s/dpo_preferences.jsonl",
                                                        config->dpo_export_dir);
                                } else {
                                    dpo_plen = snprintf(dpo_path, sizeof(dpo_path),
                                                        "data/dpo/dpo_preferences.jsonl");
                                }
                                size_t exported = 0;
                                if (dpo_plen > 0 && (size_t)dpo_plen < sizeof(dpo_path) &&
                                    hu_dpo_export_jsonl(&agent->sota.dpo_collector, dpo_path,
                                                        (size_t)dpo_plen, &exported) == HU_OK) {
                                    hu_log_info("human", agent ? agent->observer : NULL,
                                                "weekly DPO export: %zu pairs -> %s", exported,
                                                dpo_path);
                                    hu_dpo_clear(&agent->sota.dpo_collector);
                                    dpo_exported_this_week = true;
                                }
                            }
                        }
                        if (lt_tune && lt_tune->tm_wday != 0)
                            dpo_exported_this_week = false;
                    }

                    if (lt_tune && lt_tune->tm_hour == 4 && lt_tune->tm_min == 0 && agent &&
                        agent->memory && !tuned_today) {
                        char *tune_summary = NULL;
                        size_t tune_len = 0;
                        if (hu_replay_auto_tune(alloc, agent->memory, NULL, 0, &tune_summary,
                                                &tune_len) == HU_OK &&
                            tune_summary && tune_len > 0) {
                            /* 2026-05-16: the daily auto-tune used to dump its
                             * context into the process-global `replay_insights`
                             * static buffer, which was then injected into every
                             * contact's prompt — a cross-contact leak channel.
                             * The static buffer was removed; auto-tune now
                             * runs as a no-op storage side and only logs.
                             * Phase 6 will give auto-tune a properly scoped
                             * persistent key if the feature is kept. */
                            (void)hu_replay_tune_build_context;
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "daily replay auto-tune completed (tune_len=%zu)",
                                        tune_len);
                        }
                        if (tune_summary)
                            alloc->free(alloc->ctx, tune_summary, tune_len + 1);
                        tuned_today = true;
                    }
                }
                /* Weekly GraphRAG community detection at Sunday 2 AM */
#if defined(HU_ENABLE_SQLITE)
                {
                    static bool communities_built_this_week = false;
                    struct tm tm_buf2;
                    struct tm *lt = localtime_r(&t, &tm_buf2);
                    if (lt && lt->tm_wday == 1 && lt->tm_hour == 9) {
                        communities_built_this_week = false;
                    }
                    if (lt && lt->tm_wday == 0 && lt->tm_hour == 2 && lt->tm_min == 0 && graph &&
                        !communities_built_this_week) {
                        char *comm_ctx = NULL;
                        size_t comm_len = 0;
                        if (hu_graph_build_communities(graph, alloc, "", 0, 20, 2047, &comm_ctx,
                                                       &comm_len) == HU_OK &&
                            comm_ctx && comm_len > 0) {
                            size_t copy_len = comm_len < sizeof(community_insights) - 1
                                                  ? comm_len
                                                  : sizeof(community_insights) - 1;
                            memcpy(community_insights, comm_ctx, copy_len);
                            community_insights[copy_len] = '\0';
                            community_insights_len = copy_len;
                            communities_built_this_week = true;
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "weekly GraphRAG community detection completed");
                        }
                        if (comm_ctx)
                            alloc->free(alloc->ctx, comm_ctx, comm_len + 1);
                    }
                }
#endif
#endif
                last_cron_minute = current_minute;
            }
        }
#endif

        struct timespec ts_now;
        clock_gettime(CLOCK_MONOTONIC, &ts_now);
        int64_t tick_now = (int64_t)ts_now.tv_sec * 1000 + ts_now.tv_nsec / 1000000;

        for (size_t i = 0; i < channel_count; i++) {
            hu_service_channel_t *ch = &channels[i];
            if (!ch->poll_fn || !ch->channel_ctx)
                continue;
            if (tick_now - ch->last_poll_ms < (int64_t)ch->interval_ms)
                continue;

            hu_channel_loop_msg_t msgs[16];
            memset(msgs, 0, sizeof(msgs));
            size_t count = 0;
            hu_error_t poll_err = ch->poll_fn(ch->channel_ctx, alloc, msgs, 16, &count);
            ch->last_poll_ms = tick_now;
            time_t poll_receive_time = 0;
            if (count > 0) {
                ch->last_contact_ms = (uint64_t)time(NULL) * 1000ULL;
                poll_receive_time = time(NULL);
            }
            if (poll_err != HU_OK && poll_err != HU_ERR_NOT_SUPPORTED && getenv("HU_DEBUG"))
                hu_log_error("human", agent ? agent->observer : NULL,
                             "poll error on channel %zu: %s", i, hu_error_string(poll_err));

            if (!agent || !ch->channel || !ch->channel->vtable || !ch->channel->vtable->send ||
                count == 0) {
                /* Log inbound messages from read-only channels (e.g. Gmail) */
                if (count > 0 && ch->channel && ch->channel->vtable && !ch->channel->vtable->send) {
                    const char *ch_name = ch->channel->vtable->name
                                              ? ch->channel->vtable->name(ch->channel->ctx)
                                              : "?";
                    for (size_t m = 0; m < count; m++) {
                        size_t clen = strlen(msgs[m].content);
                        hu_log_info(ch_name, agent ? agent->observer : NULL,
                                    "ingest: %.60s%s (from %s)", msgs[m].content,
                                    clen > 60 ? "..." : "", msgs[m].session_key);
                    }
                }
                continue;
            }

            /*
             * Batch consecutive messages from the same sender into one prompt.
             * A real person reads all pending messages, then replies once.
             */
            for (size_t m = 0; m < count; /* advanced inside */) {
                size_t content_len = strlen(msgs[m].content);
                if (content_len == 0) {
                    m++;
                    continue;
                }

                const char *batch_key = msgs[m].session_key;
                size_t key_len = strlen(batch_key);

                /* For group chats, route sends to the chat_id (thread)
                 * rather than the sender's handle to avoid accidental DMs. */
                const char *send_target = batch_key;
                size_t send_target_len = key_len;
                if (msgs[m].is_group && msgs[m].chat_id[0]) {
                    send_target = msgs[m].chat_id;
                    send_target_len = strlen(send_target);
                }

                /* Gather consecutive messages from the same sender */
                char combined[4096];
                size_t combined_len = 0;
                size_t batch_start = m;
                size_t batch_end = m;

                while (m < count && strcmp(msgs[m].session_key, batch_key) == 0) {
                    const char *content_to_add = msgs[m].content;
                    size_t mlen = strlen(content_to_add);
#ifndef HU_IS_TEST
                    /* Per-message attachment: images via vision; local audio/video via multimodal
                     * route. Injects description or transcription into batch text. */
                    if (msgs[m].has_attachment && msgs[m].message_id > 0 && agent &&
                        agent->provider.vtable && ch->channel->vtable->get_attachment_path) {
                        char *path = ch->channel->vtable->get_attachment_path(
                            ch->channel->ctx, alloc, msgs[m].message_id);
                        if (path) {
                            const char *model = agent->model_name ? agent->model_name : "gpt-4o";
                            size_t model_len = agent->model_name_len > 0 ? agent->model_name_len
                                                                         : (size_t)strlen(model);
                            size_t plen = strlen(path);
                            const char *ext = NULL;
                            for (size_t ei = plen; ei > 0; ei--) {
                                if (path[ei - 1] == '.') {
                                    ext = &path[ei - 1];
                                    break;
                                }
                            }
                            bool is_audio =
                                ext && (strcmp(ext, ".mp3") == 0 || strcmp(ext, ".wav") == 0 ||
                                        strcmp(ext, ".ogg") == 0 || strcmp(ext, ".m4a") == 0 ||
                                        strcmp(ext, ".caf") == 0);
                            bool is_video =
                                ext && (strcmp(ext, ".mp4") == 0 || strcmp(ext, ".mov") == 0 ||
                                        strcmp(ext, ".webm") == 0);

                            if (is_audio || is_video) {
                                char *media_desc = NULL;
                                size_t media_desc_len = 0;
                                if (hu_multimodal_route_local_media(
                                        alloc, path, plen, &agent->provider, model, model_len,
                                        &media_desc, &media_desc_len) == HU_OK &&
                                    media_desc && media_desc_len > 0) {
                                    char attachment_augmented[4096];
                                    size_t desc_copy =
                                        media_desc_len > 3800 ? 3800 : media_desc_len;
                                    int n;
                                    if (is_audio) {
                                        if (mlen > 0 && strcmp(content_to_add, "[Audio]") != 0) {
                                            n = snprintf(
                                                attachment_augmented, sizeof(attachment_augmented),
                                                "%.*s\n[Audio transcription: %.*s]", (int)mlen,
                                                content_to_add, (int)desc_copy, media_desc);
                                        } else {
                                            n = snprintf(attachment_augmented,
                                                         sizeof(attachment_augmented),
                                                         "[Audio transcription: %.*s]",
                                                         (int)desc_copy, media_desc);
                                        }
                                    } else {
                                        if (mlen > 0 && strcmp(content_to_add, "[Video]") != 0) {
                                            n = snprintf(
                                                attachment_augmented, sizeof(attachment_augmented),
                                                "%.*s\n[Video transcription: %.*s]", (int)mlen,
                                                content_to_add, (int)desc_copy, media_desc);
                                        } else {
                                            n = snprintf(attachment_augmented,
                                                         sizeof(attachment_augmented),
                                                         "[Video transcription: %.*s]",
                                                         (int)desc_copy, media_desc);
                                        }
                                    }
                                    alloc->free(alloc->ctx, media_desc, media_desc_len + 1);
                                    if (n > 0 && (size_t)n < sizeof(attachment_augmented)) {
                                        content_to_add = attachment_augmented;
                                        mlen = (size_t)n;
                                    }
                                } else if (media_desc) {
                                    alloc->free(alloc->ctx, media_desc, media_desc_len + 1);
                                }
                            } else if (agent->provider.vtable->supports_vision &&
                                       agent->provider.vtable->supports_vision(
                                           agent->provider.ctx)) {
                                hu_log_info("human", agent ? agent->observer : NULL,
                                            "vision: describing image %.*s", (int)plen, path);
                                char *desc = NULL;
                                size_t desc_len = 0;
                                hu_error_t verr =
                                    hu_vision_describe_image(alloc, &agent->provider, path, plen,
                                                             model, model_len, &desc, &desc_len);
                                hu_log_info("human", agent ? agent->observer : NULL,
                                            "vision: result=%s desc_len=%zu", hu_error_string(verr),
                                            desc_len);
                                if (verr == HU_OK && desc && desc_len > 0) {
                                    char attachment_augmented[4096];
                                    size_t desc_copy = desc_len > 3800 ? 3800 : desc_len;
                                    int n;
                                    if (mlen > 0 && strcmp(content_to_add, "[Photo]") != 0) {
                                        n = snprintf(attachment_augmented,
                                                     sizeof(attachment_augmented),
                                                     "%.*s\n[They sent a photo: %.*s]", (int)mlen,
                                                     content_to_add, (int)desc_copy, desc);
                                    } else {
                                        n = snprintf(
                                            attachment_augmented, sizeof(attachment_augmented),
                                            "[They sent a photo: %.*s]", (int)desc_copy, desc);
                                    }
                                    alloc->free(alloc->ctx, desc, desc_len + 1);
                                    if (n > 0 && (size_t)n < sizeof(attachment_augmented)) {
                                        content_to_add = attachment_augmented;
                                        mlen = (size_t)n;
                                        if (agent->bth_metrics)
                                            agent->bth_metrics->vision_descriptions++;
                                    }
                                } else if (desc) {
                                    alloc->free(alloc->ctx, desc, desc_len + 1);
                                }
                            }
                            alloc->free(alloc->ctx, path, plen + 1);
                        }
                    } else if (msgs[m].has_video) {
                        /* F7: Video context — no vision in Phase 1; inject "[They sent a video]" */
                        char video_augmented[4096];
                        int n;
                        if (mlen > 0 && strcmp(content_to_add, "[Video]") != 0) {
                            n = snprintf(video_augmented, sizeof(video_augmented),
                                         "%.*s\n[They sent a video]", (int)mlen, content_to_add);
                        } else {
                            n = snprintf(video_augmented, sizeof(video_augmented),
                                         "[They sent a video]");
                        }
                        if (n > 0 && (size_t)n < sizeof(video_augmented)) {
                            content_to_add = video_augmented;
                            mlen = (size_t)n;
                        }
                    }
#endif
                    if (mlen == 0) {
                        m++;
                        continue;
                    }
                    if (combined_len + mlen + 2 >= sizeof(combined))
                        break;
                    if (combined_len > 0)
                        combined[combined_len++] = '\n';
                    memcpy(combined + combined_len, content_to_add, mlen);
                    combined_len += mlen;
                    batch_end = m;
                    m++;
                }
                combined[combined_len] = '\0';

                if (combined_len == 0)
                    continue;

                /* SHIELD-005: Inbound moderation — catch crisis signals early */
                bool inbound_crisis = false;
                {
                    hu_moderation_result_t inbound_mod;
                    memset(&inbound_mod, 0, sizeof(inbound_mod));
                    if (hu_moderation_check(alloc, combined, combined_len, &inbound_mod) == HU_OK &&
                        inbound_mod.self_harm) {
                        hu_log_error("human", agent ? agent->observer : NULL,
                                     "INBOUND crisis detected from %.*s (score=%.2f)",
                                     (int)(key_len > 20 ? 20 : key_len), batch_key,
                                     inbound_mod.self_harm_score);
                        inbound_crisis = true;
                    }
                }

                /* Clear STM before each contact batch to avoid cross-contact emotion contamination
                 */
                hu_stm_clear(&agent->stm);

#ifndef HU_IS_TEST
                /* F119: Contact replied — reset governor cool-off so proactive
                 * outreach can resume after silence. */
                (void)hu_governor_record_response(&gov_budget);

                /* Reciprocity tracking: record their initiation for balanced outreach.
                 * Gated on HU_ENABLE_SQLITE because the helper's declaration lives
                 * inside that guard in include/human/context/self_awareness.h.
                 * The other three call sites are already gated; this one was
                 * missed when reciprocity was wired in, so the no-sqlite /
                 * cross-arm64 / minimal builds tripped
                 * -Werror=implicit-function-declaration. */
#ifdef HU_ENABLE_SQLITE
                if (agent->memory) {
                    bool they_asked = (memchr(combined, '?', combined_len) != NULL);
                    (void)hu_self_awareness_record_their_reciprocity(
                        alloc, agent->memory, batch_key, key_len, true, they_asked, false);
                }
#endif
                /* Only respond to contacts explicitly listed in persona_contacts.
                 * When a persona is loaded, ALL inbound messages from contacts not in
                 * the allowlist are silently dropped — even if the list is empty. */
                if (agent->persona) {
                    const hu_contact_profile_t *cp_gate =
                        hu_persona_find_contact(agent->persona, batch_key, key_len);
                    if (!cp_gate) {
                        if (getenv("HU_DEBUG"))
                            hu_log_error("human", agent ? agent->observer : NULL,
                                         "ignoring message from unknown contact: %.*s",
                                         (int)(key_len > 20 ? 20 : key_len), batch_key);
                        continue;
                    }
                    /* Never respond to messages from the persona owner's own number */
                    if (cp_gate->relationship && strcmp(cp_gate->relationship, "self") == 0) {
                        if (getenv("HU_DEBUG"))
                            hu_log_error("human", agent ? agent->observer : NULL,
                                         "ignoring message from self: %.*s",
                                         (int)(key_len > 20 ? 20 : key_len), batch_key);
                        continue;
                    }
                }

                hu_log_info("human", agent ? agent->observer : NULL,
                            "processing batch for %.*s: \"%.*s\" (group=%d)",
                            (int)(key_len > 20 ? 20 : key_len), batch_key,
                            (int)(combined_len > 60 ? 60 : combined_len), combined,
                            (int)msgs[batch_start].is_group);

#ifdef HU_ENABLE_SQLITE
                /* F23: Topic absence detection — record topic baselines from user message */
                if (agent->memory && combined_len > 0)
                    record_topic_baselines_from_text(agent->memory, batch_key, key_len, combined,
                                                     combined_len);
                /* F26: Temporal pattern learning — record message frequency by day/hour */
                if (agent->memory && batch_key && key_len > 0) {
                    time_t now_t = time(NULL);
                    struct tm lt_buf;
                    struct tm *lt = localtime_r(&now_t, &lt_buf);
                    if (lt) {
                        int64_t response_time_ms = 0; /* not available from poll pipeline */
                        (void)hu_superhuman_temporal_record(agent->memory, batch_key, key_len,
                                                            lt->tm_wday, lt->tm_hour,
                                                            response_time_ms);
                    }
                }
#endif

                /* Preload channel history early so the group classifier can use it */
                hu_channel_history_entry_t *early_history = NULL;
                size_t early_history_count = 0;
                bool use_backchannel = false;
                char backchannel_buf[32];
                size_t backchannel_len = 0;

                /* Director meta-behavior result — persists through batch scope */
                hu_director_result_t director_result;
                memset(&director_result, 0, sizeof(director_result));
                bool director_result_valid = false;
                if (ch->channel->vtable->load_conversation_history) {
                    ch->channel->vtable->load_conversation_history(
                        ch->channel->ctx, alloc, batch_key, key_len, 10, &early_history,
                        &early_history_count);
                }

                /* Track if group classifier indicates brief response */
                bool group_brief = false;

                /* Group chat gating: use group classifier to decide engagement */
                if (msgs[batch_start].is_group) {
                    const char *persona_name = NULL;
                    if (agent->persona && agent->persona->identity)
                        persona_name = agent->persona->identity;
                    hu_group_response_t gr =
                        hu_conversation_classify_group(combined, combined_len, persona_name,
                                                       persona_name ? strlen(persona_name) : 0,
                                                       early_history, early_history_count);
                    if (gr == HU_GROUP_SKIP) {
                        hu_log_info("human", agent ? agent->observer : NULL,
                                    "group: skipping (not addressed): %.*s",
                                    (int)(combined_len > 40 ? 40 : combined_len), combined);
                        if (early_history)
                            alloc->free(alloc->ctx, early_history,
                                        early_history_count * sizeof(hu_channel_history_entry_t));
                        continue;
                    }
                    if (gr == HU_GROUP_BRIEF)
                        group_brief = true;
                }

                /* Check llm_decides: bypass all heuristic gating */
                bool llm_decides = false;
                {
                    const char *chn_ld = ch->channel->vtable->name
                                             ? ch->channel->vtable->name(ch->channel->ctx)
                                             : NULL;
                    const hu_channel_daemon_config_t *dcfg_ld =
                        get_active_daemon_config(config, chn_ld);
                    if (dcfg_ld && dcfg_ld->llm_decides)
                        llm_decides = true;
                }

                /* Response decision using conversation-aware classifier */
                uint32_t extra_delay_ms = 0;

                hu_response_action_t action = hu_conversation_classify_response(
                    combined, combined_len, early_history, early_history_count, &extra_delay_ms);

                if (llm_decides) {
                    action = HU_RESPONSE_FULL;
                    extra_delay_ms = 0;
                    if (agent)
                        agent->lean_prompt = true;
                    hu_log_info("human", agent ? agent->observer : NULL,
                                "llm_decides: forwarding to LLM (lean prompt) for %.*s",
                                (int)(key_len > 20 ? 20 : key_len), batch_key);
                }

                /* Apply response_mode override from active channel's daemon config.
                 * "selective" (default): FULL→BRIEF for non-questions.
                 * "eager": BRIEF/SKIP→FULL (respond to almost everything).
                 * "normal": no override. */
                if (!llm_decides) {
                    const char *ch_name_rm = ch->channel->vtable->name
                                                 ? ch->channel->vtable->name(ch->channel->ctx)
                                                 : NULL;
                    const hu_channel_daemon_config_t *dcfg_rm =
                        get_active_daemon_config(config, ch_name_rm);
                    const char *rmode =
                        (dcfg_rm && dcfg_rm->response_mode && dcfg_rm->response_mode[0])
                            ? dcfg_rm->response_mode
                            : NULL;
                    if (!rmode || !rmode[0] || strcmp(rmode, "selective") == 0) {
                        if (action == HU_RESPONSE_FULL && !memchr(combined, '?', combined_len))
                            action = HU_RESPONSE_BRIEF;
                    } else if (strcmp(rmode, "eager") == 0) {
                        if (action == HU_RESPONSE_BRIEF)
                            action = HU_RESPONSE_FULL;
                    }
                    /* "normal" = no change */
                }

                /* Natural drop-off: when FULL/BRIEF, probabilistic skip for mutual farewell,
                 * low-energy acks, emoji-only, or our farewell + their minimal reply. */
#ifndef HU_IS_TEST
                if (!llm_decides && (action == HU_RESPONSE_FULL || action == HU_RESPONSE_BRIEF) &&
                    early_history && early_history_count > 0) {
                    uint32_t dropoff_seed =
                        (uint32_t)time(NULL) * 1103515245u + 12345u + (uint32_t)(uintptr_t)combined;
                    int dropoff_prob = hu_conversation_classify_dropoff(
                        combined, combined_len, early_history, early_history_count, dropoff_seed);
                    if (dropoff_prob > 0 &&
                        ((dropoff_seed >> 16u) % 100u) < (uint32_t)dropoff_prob) {
                        action = HU_RESPONSE_SKIP;
                        hu_log_info("human", agent ? agent->observer : NULL,
                                    "drop-off skip (prob=%d%%): %.*s", dropoff_prob,
                                    (int)(combined_len > 40 ? 40 : combined_len), combined);
                    }
                }
#endif

                hu_log_info("human", agent ? agent->observer : NULL,
                            "classify result: action=%d delay=%u for %.*s", (int)action,
                            (unsigned)extra_delay_ms, (int)(key_len > 20 ? 20 : key_len),
                            batch_key);

                /* Consecutive response limiter: if Human has responded to
                 * this contact 3+ times in a row without the real user
                 * stepping in, stay silent so we don't run away. */
                size_t consec_idx = SIZE_MAX;
                for (size_t ci = 0; ci < consec_contact_count; ci++) {
                    if (key_len < sizeof(consec_contact_keys[0]) &&
                        memcmp(consec_contact_keys[ci], batch_key, key_len) == 0 &&
                        consec_contact_keys[ci][key_len] == '\0') {
                        consec_idx = ci;
                        break;
                    }
                }
                if (!llm_decides && consec_idx != SIZE_MAX &&
                    consec_response_count[consec_idx] >= 3 && action != HU_RESPONSE_SKIP) {
                    hu_log_info("human", agent ? agent->observer : NULL,
                                "consecutive limit (%u) reached for %.*s — staying silent",
                                (unsigned)consec_response_count[consec_idx],
                                (int)(key_len > 20 ? 20 : key_len), batch_key);
                    action = HU_RESPONSE_SKIP;
                }

                /* Tapback-skip: for tapback-worthy messages, 70% chance to not respond */
                bool tapback_skip = false;
#ifndef HU_IS_TEST
                if (!llm_decides && action != HU_RESPONSE_SKIP &&
                    hu_daemon_is_tapback_worthy(combined, combined_len)) {
                    uint32_t tb_pct = config ? config->behavior.tapback_skip_pct : 20u;
                    uint32_t r = (uint32_t)time(NULL);
                    r = r * 1103515245u + 12345u + (uint32_t)(uintptr_t)combined;
                    r = (r >> 16u) & 0x7fffu;
                    if ((r % 100u) < tb_pct)
                        tapback_skip = true;
                }
#endif

                /* F46: Leave-on-read — deliberate non-response as social signal (<2%).
                 * Never in group chats. Skip if we're in active leave-on-read period, or
                 * if classifier says leave-on-read and we store 2–24h timer. */
                bool leave_on_read_skip = false;
#ifndef HU_IS_TEST
                /* Leave-on-read state machine (F46): combine active-period status
                 * (ring buffer scan), group-chat policy, and the classifier's verdict
                 * via hu_leave_on_read_decide. Decision policy is unit-tested in
                 * tests/test_conversation.c::leave_on_read_decide_*; this block
                 * handles only the daemon-local state (ring scan + insert). */
                if (!llm_decides) {
                    time_t now_ts = time(NULL);
                    bool is_group = msgs[batch_start].is_group;
                    size_t lor_slot = SIZE_MAX;
                    bool already_in_period = false;
                    for (size_t lor_i = 0; lor_i < HU_LEAVE_ON_READ_MAX; lor_i++) {
                        if (leave_on_read_entries[lor_i].key[0] == '\0')
                            continue;
                        if (key_len < sizeof(leave_on_read_entries[lor_i].key) &&
                            memcmp(leave_on_read_entries[lor_i].key, batch_key, key_len) == 0 &&
                            leave_on_read_entries[lor_i].key[key_len] == '\0') {
                            if (leave_on_read_entries[lor_i].until > now_ts)
                                already_in_period = true;
                            else
                                lor_slot = lor_i; /* expired, can reuse */
                            break;
                        }
                    }

                    uint32_t lor_seed =
                        (uint32_t)now_ts * 1103515245u + 12345u + (uint32_t)(uintptr_t)combined;
                    bool helper_says = false;
                    bool eligible_to_roll = !is_group && !already_in_period &&
                                            action != HU_RESPONSE_SKIP && !tapback_skip;
                    if (eligible_to_roll) {
                        uint8_t lor_pct = 0;
                        if (agent->persona) {
                            const hu_persona_overlay_t *lor_ov = NULL;
                            if (agent->active_channel)
                                lor_ov =
                                    hu_persona_find_overlay(agent->persona, agent->active_channel,
                                                            agent->active_channel_len);
                            const hu_contact_profile_t *lor_cp =
                                hu_persona_find_contact(agent->persona, batch_key, key_len);
                            lor_pct = hu_leave_on_read_pct_effective(lor_cp, lor_ov);
                        }
                        helper_says = hu_conversation_should_leave_on_read(
                            combined, combined_len, early_history, early_history_count, lor_seed,
                            lor_pct);
                    }

                    hu_leave_on_read_decision_t decision =
                        hu_leave_on_read_decide(is_group, already_in_period, helper_says);
                    if (decision == HU_LOR_ALREADY_IN_PERIOD) {
                        leave_on_read_skip = true;
                        hu_log_info("human", agent ? agent->observer : NULL,
                                    "leave-on-read: still in period for %.*s",
                                    (int)(key_len > 20 ? 20 : key_len), batch_key);
                    } else if (decision == HU_LOR_TRIGGER_NEW) {
                        leave_on_read_skip = true;
                        uint32_t hrs = 7200u + ((lor_seed >> 16u) % (86400u - 7200u + 1u));
                        time_t until = now_ts + (time_t)hrs;
                        if (lor_slot == SIZE_MAX) {
                            for (size_t lor_j = 0; lor_j < HU_LEAVE_ON_READ_MAX; lor_j++) {
                                if (leave_on_read_entries[lor_j].key[0] == '\0' ||
                                    leave_on_read_entries[lor_j].until <= now_ts) {
                                    lor_slot = lor_j;
                                    break;
                                }
                            }
                        }
                        if (lor_slot != SIZE_MAX &&
                            key_len < sizeof(leave_on_read_entries[0].key)) {
                            memcpy(leave_on_read_entries[lor_slot].key, batch_key, key_len);
                            leave_on_read_entries[lor_slot].key[key_len] = '\0';
                            leave_on_read_entries[lor_slot].until = until;
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "leave-on-read: skipping for %.*s until %ld",
                                        (int)(key_len > 20 ? 20 : key_len), batch_key, (long)until);
                        }
                    }
                }
#endif

                if (action == HU_RESPONSE_SKIP || tapback_skip || leave_on_read_skip) {
                    if (tapback_skip)
                        hu_log_info("human", agent ? agent->observer : NULL,
                                    "tapback-skip (no response): %.*s",
                                    (int)(combined_len > 40 ? 40 : combined_len), combined);
                    else
                        hu_log_info("human", agent ? agent->observer : NULL,
                                    "skipping message (no response needed): %.*s",
                                    (int)(combined_len > 40 ? 40 : combined_len), combined);
                    if (agent->session_store && agent->session_store->vtable &&
                        agent->session_store->vtable->save_message) {
                        for (size_t b = batch_start; b <= batch_end; b++) {
                            size_t blen = strlen(msgs[b].content);
                            if (blen > 0)
                                agent->session_store->vtable->save_message(
                                    agent->session_store->ctx, batch_key, key_len, "user", 4,
                                    msgs[b].content, blen);
                        }
                    }
                    if (early_history)
                        alloc->free(alloc->ctx, early_history,
                                    early_history_count * sizeof(hu_channel_history_entry_t));
                    if (agent)
                        agent->lean_prompt = false;
                    continue;
                }

                /* F29: Active listening backchannels — send brief cue instead of LLM when
                 * narrative/venting detected and probability roll passes. */
                {
                    float bc_prob = 0.3f;
                    if (agent && agent->persona)
                        bc_prob = agent->persona->humanization.backchannel_probability;
                    uint32_t bc_seed =
                        (uint32_t)time(NULL) * 1103515245u + 12345u + (uint32_t)(uintptr_t)combined;
                    if (hu_conversation_should_backchannel(combined, combined_len, early_history,
                                                           early_history_count, bc_seed, bc_prob)) {
                        backchannel_len = hu_conversation_pick_backchannel(
                            bc_seed + 1, backchannel_buf, sizeof(backchannel_buf));
                        if (backchannel_len > 0)
                            use_backchannel = true;
                    }
                }
                if (early_history && !llm_decides) {
                    alloc->free(alloc->ctx, early_history,
                                early_history_count * sizeof(hu_channel_history_entry_t));
                    early_history = NULL;
                    early_history_count = 0;
                }

                /* ── BTH: Late-night mode (b1c) ────────────────────────────── */
                int bth_hour = -1;
#ifndef HU_IS_TEST
                {
                    time_t night_t = time(NULL);
                    struct tm night_buf;
                    struct tm *night_lt = localtime_r(&night_t, &night_buf);
                    if (night_lt)
                        bth_hour = night_lt->tm_hour;
                }
                if (bth_hour >= 0) {
                    if (bth_hour >= 2 && bth_hour < 6) {
                        /* 2AM-6AM: very high SKIP chance (sleeping) */
                        uint32_t night_r = (uint32_t)time(NULL) * 1103515245u + 12345u;
                        if (((night_r >> 16u) % 100u) < 85u) {
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "late-night skip (2AM-6AM, hour=%d)", bth_hour);
                            action = HU_RESPONSE_SKIP;
                            if (early_history) {
                                alloc->free(alloc->ctx, early_history,
                                            early_history_count *
                                                sizeof(hu_channel_history_entry_t));
                                early_history = NULL;
                                early_history_count = 0;
                            }
                            if (agent)
                                agent->lean_prompt = false;
                            continue;
                        }
                        /* If not skipped, force very delayed + brief */
                        extra_delay_ms += 30000 + ((night_r >> 8u) % 60000u);
                        action = HU_RESPONSE_BRIEF;
                    } else if (bth_hour >= 7 && bth_hour < 9) {
                        /* 7-9AM: brief "just woke up" energy */
                        if (action == HU_RESPONSE_FULL)
                            action = HU_RESPONSE_BRIEF;
                    }
                }
#endif

                /* ── BTH: Media-type awareness (b3c) ──────────────────────── */
                if (action != HU_RESPONSE_SKIP &&
                    hu_conversation_is_media_message(combined, combined_len, NULL, 0)) {
                    action = HU_RESPONSE_BRIEF;
                    hu_log_info("human", agent ? agent->observer : NULL,
                                "media message detected, forcing brief response");
                }

                /* For BRIEF actions, override max_response_chars to force ultra-short */
                bool brief_mode = (action == HU_RESPONSE_BRIEF) || group_brief ||
                                  msgs[batch_start].is_group; /* always brief in group chats */

#ifndef HU_IS_TEST
                if (agent && agent->bth_metrics)
                    agent->bth_metrics->total_turns++;
#endif

                /* Load per-contact timing model from chat.db */
#if !defined(HU_IS_TEST) && defined(HU_ENABLE_SQLITE) && defined(__APPLE__)
                if (agent && agent->timing_model &&
                    (agent->timing_model->contact_id == NULL ||
                     strncmp(agent->timing_model->contact_id, batch_key, key_len) != 0)) {
                    hu_timing_model_deinit(alloc, agent->timing_model);
                    memset(agent->timing_model, 0, sizeof(*agent->timing_model));
                    hu_timing_model_learn_from_chatdb(agent->timing_model, batch_key, key_len);
                    agent->timing_model->contact_id = hu_strdup(alloc, batch_key);
                    if (agent->timing_model->contact_id)
                        agent->timing_model->contact_id_len = key_len;
                }
#endif

#if defined(HU_IS_TEST) && HU_IS_TEST
                /* Deterministic director (no cloud classify provider in tests). */
                if (llm_decides) {
                    director_result_valid =
                        hu_daemon_director_call(alloc, combined, combined_len, early_history,
                                                early_history_count, &director_result);
                    if (early_history) {
                        alloc->free(alloc->ctx, early_history,
                                    early_history_count * sizeof(hu_channel_history_entry_t));
                        early_history = NULL;
                        early_history_count = 0;
                    }
                    goto llm_decides_skip_delays;
                }
#endif

                /* Seen behavior: model realistic "read then wait" patterns */
#ifndef HU_IS_TEST
                if (llm_decides) {
                    /* Call director early for meta-behavior (delay, tapback, silence) */
                    if (g_classify_provider_ok) {
                        director_result_valid =
                            hu_daemon_director_call(alloc, combined, combined_len, early_history,
                                                    early_history_count, &director_result);
                    }
                    if (early_history) {
                        alloc->free(alloc->ctx, early_history,
                                    early_history_count * sizeof(hu_channel_history_entry_t));
                        early_history = NULL;
                        early_history_count = 0;
                    }
                    if (director_result_valid && director_result.delay_s > 0) {
                        uint32_t delay_ms = director_result.delay_s * 1000;
                        uint32_t delay_seed = (uint32_t)time(NULL) * 1103515245u + 12345u +
                                              (uint32_t)(uintptr_t)batch_key;
                        uint32_t jitter = delay_seed % (delay_ms / 5 + 1);
                        delay_ms += (delay_seed & 1) ? jitter : 0;
                        if (delay_ms > 120000)
                            delay_ms = 120000;
                        /* Seen-then-reply choreography: mark as read early,
                         * then pause before typing — like a real human who
                         * picks up their phone, reads, thinks, then replies. */
                        uint32_t read_wait;
                        if (delay_ms <= 5000) {
                            /* Quick reply: read fast (200-800ms) */
                            read_wait = 200 + (delay_seed >> 16u) % 601u;
                        } else if (delay_ms <= 15000) {
                            /* Normal reply: read in 1-3s */
                            read_wait = 1000 + (delay_seed >> 16u) % 2001u;
                        } else {
                            /* Slow reply (busy): read in 2-8s */
                            read_wait = 2000 + (delay_seed >> 16u) % 6001u;
                        }
                        if (read_wait > delay_ms)
                            read_wait = delay_ms / 2;
                        hu_log_info("human", agent ? agent->observer : NULL,
                                    "director delay: %u ms (read after %ums)", delay_ms, read_wait);
                        usleep(read_wait * 1000u);
                        if (ch->channel->vtable->mark_read)
                            ch->channel->vtable->mark_read(ch->channel->ctx, batch_key, key_len);
                        uint32_t remaining = delay_ms > read_wait ? delay_ms - read_wait : 0;
                        if (remaining > 0)
                            usleep(remaining * 1000u);
                    }
                    goto llm_decides_skip_delays;
                }
                /* Trigger read receipt with human-like delay */
                if (ch->channel->vtable->mark_read) {
                    uint32_t read_seed = (uint32_t)time(NULL) * 1103515245u + 12345u +
                                         (uint32_t)(uintptr_t)batch_key;
                    uint32_t read_delay_ms = 500 + (read_seed >> 16u) % 2001u;
                    usleep(read_delay_ms * 1000u);
                    ch->channel->vtable->mark_read(ch->channel->ctx, batch_key, key_len);
                }
                {
                    uint32_t seen_seed =
                        (uint32_t)time(NULL) * 1103515245u + (uint32_t)(uintptr_t)combined;
                    uint32_t seen_delay_ms = 0;
                    hu_seen_action_t seen_action = hu_conversation_classify_seen_behavior(
                        combined, combined_len, (uint8_t)bth_hour, seen_seed, &seen_delay_ms);
                    if (seen_action == HU_SEEN_DELAY_THEN_RESPOND && seen_delay_ms > 0) {
                        hu_log_info("human", agent ? agent->observer : NULL,
                                    "seen-delay: waiting %u ms before responding", seen_delay_ms);
                        usleep(seen_delay_ms * 1000u);
                    }
                }
#endif

                /* Adaptive timing based on message type. Variable delay with jitter,
                 * time-of-day awareness, and rare "busy" delay.
                 * This sleep also serves as the burst accumulation window. */
#ifndef HU_IS_TEST
                {
                    size_t msg_count = batch_end - batch_start + 1;
                    uint32_t base_delay = 2500 + (uint32_t)(msg_count * 1500);
                    if (base_delay > 8000)
                        base_delay = 8000;
                    base_delay += extra_delay_ms;
                    if (base_delay > 15000)
                        base_delay = 15000;

                    /* F6: Photo viewing delay — 3–8 s when batch has attachment */
                    {
                        uint32_t seed = 0;
#if defined(__APPLE__) || (defined(__linux__) && defined(__GLIBC__))
                        seed = (uint32_t)arc4random_uniform(5001);
#else
                        seed = ((uint32_t)time(NULL) * 1103515245u + 12345u) % 5001u;
#endif
                        base_delay +=
                            hu_daemon_compute_photo_delay(msgs, batch_start, batch_end, seed);
                    }
                    /* F7: Video viewing delay — 2–10 s when batch has video */
                    {
                        uint32_t vseed = 0;
#if defined(__APPLE__) || (defined(__linux__) && defined(__GLIBC__))
                        vseed = (uint32_t)arc4random_uniform(8001);
#else
                        vseed = ((uint32_t)time(NULL) * 2654435769u + 54321u) % 8001u;
#endif
                        base_delay +=
                            hu_daemon_compute_video_delay(msgs, batch_start, batch_end, vseed);
                    }

                    /* Add +/- 30% random jitter */
                    uint32_t jitter_range = base_delay * 30 / 100;
                    uint32_t jitter = 0;
#if defined(__APPLE__) || (defined(__linux__) && defined(__GLIBC__))
                    jitter = (uint32_t)arc4random_uniform(jitter_range * 2 + 1);
#else
                    {
                        uint32_t s = (uint32_t)time(NULL);
                        s = s * 1103515245u + 12345u;
                        jitter = (s >> 16u) & 0x7fffu;
                        jitter = jitter % (jitter_range * 2 + 1);
                    }
#endif
                    int32_t adjusted =
                        (int32_t)base_delay - (int32_t)jitter_range + (int32_t)jitter;
                    if (adjusted < 1000)
                        adjusted = 1000;

                    /* Late-night mode: realistic delays — a 51yo checking his
                     * phone groggily takes 1-3 minutes, not 8 seconds. */
                    if (bth_hour >= 1 && bth_hour < 7) {
                        /* 1-7 AM: probably asleep, phone woke them up */
                        adjusted = adjusted * 3;
                        adjusted += 90000 + (int32_t)(
#if defined(__APPLE__) || (defined(__linux__) && defined(__GLIBC__))
                                                arc4random_uniform(90001)
#else
                                                (((uint32_t)time(NULL) * 2654435769u) >> 16u) %
                                                90001u
#endif
                                            );
                    } else if (bth_hour >= 0 && bth_hour < 1) {
                        /* midnight-1 AM: winding down, slow to respond */
                        adjusted = adjusted * 2;
                        adjusted += 45000 + (int32_t)(
#if defined(__APPLE__) || (defined(__linux__) && defined(__GLIBC__))
                                                arc4random_uniform(75001)
#else
                                                (((uint32_t)time(NULL) * 2654435769u) >> 16u) %
                                                75001u
#endif
                                            );
                    } else if (bth_hour >= 22) {
                        /* 10 PM+: getting sleepy */
                        adjusted = adjusted * 2;
                        adjusted += 20000 + (int32_t)(
#if defined(__APPLE__) || (defined(__linux__) && defined(__GLIBC__))
                                                arc4random_uniform(40001)
#else
                                                (((uint32_t)time(NULL) * 2654435769u) >> 16u) %
                                                40001u
#endif
                                            );
                    } else if (bth_hour >= 21) {
                        /* 9-10 PM: evening, slower than daytime */
                        adjusted = adjusted * 3 / 2;
                        adjusted += 10000;
                    } else if (bth_hour >= 7 && bth_hour < 9) {
                        /* 7-9 AM: just woke up, groggy */
                        adjusted = adjusted * 3 / 2;
                        adjusted += 15000;
                    }

                    /* Rare "busy" delay: 1% chance of 15-45 second delay */
#if defined(__APPLE__) || (defined(__linux__) && defined(__GLIBC__))
                    if (arc4random_uniform(100) < 1)
#else
                    if ((((uint32_t)time(NULL) * 1103515245u + 12345u) >> 16u) % 100u < 1u)
#endif
                        adjusted += (int32_t)(15000 + (uint32_t)(
#if defined(__APPLE__) || (defined(__linux__) && defined(__GLIBC__))
                                                          arc4random_uniform(30001)
#else
                                                          (((uint32_t)time(NULL) * 1103515245u +
                                                            12345u) >>
                                                           16u) %
                                                          30001u
#endif
                                                              ));

                    /* F59: Multiply delay by life sim availability factor */
                    if (agent && agent->persona &&
                        (agent->persona->daily_routine.weekday_count > 0 ||
                         agent->persona->daily_routine.weekend_count > 0)) {
                        time_t now_ts = time(NULL);
                        struct tm tm_buf;
                        struct tm *lt = hu_platform_localtime_r(&now_ts, &tm_buf);
                        int dow = lt ? lt->tm_wday : 0;
                        uint32_t seed = (uint32_t)now_ts * 1103515245u + 12345u;
                        hu_life_sim_state_t ls_state = hu_life_sim_get_current(
                            &agent->persona->daily_routine, (int64_t)now_ts, dow, seed);
                        adjusted = (int32_t)((float)adjusted * ls_state.availability_factor);
                        if (adjusted < 1000)
                            adjusted = 1000;
                    }
                    /* F157 (Pillar 31): Statistical timing model overlay — learned distribution */
                    {
                        uint32_t tm_seed = (uint32_t)((uintptr_t)batch_key ^ (uint32_t)time(NULL));
                        int tm_dow = 0;
                        {
                            time_t tm_now = time(NULL);
                            struct tm tm_buf2;
                            struct tm *lt2 = hu_platform_localtime_r(&tm_now, &tm_buf2);
                            if (lt2)
                                tm_dow = lt2->tm_wday;
                        }
                        uint64_t stm_delay =
                            hu_timing_model_sample(agent->timing_model, (uint8_t)bth_hour,
                                                   (uint8_t)tm_dow, combined_len, tm_seed);
                        if (stm_delay > 0 && stm_delay < 120000) {
                            uint64_t blended = ((uint64_t)adjusted + stm_delay) / 2;
                            adjusted = (int32_t)blended;
                        }
                    }
                    if (adjusted > 60000)
                        adjusted = 60000;
                    /* F157b: Context-aware timing adjustment */
                    {
                        uint8_t conv_depth =
                            (uint8_t)(agent->history_count > 255 ? 255 : agent->history_count);
                        double emo_intensity = 0.0;
                        if (combined_len > 0) {
                            const char *emo_words[] = {"love",    "hate",       "angry", "sad",
                                                       "worried", "scared",     "happy", "excited",
                                                       "miss",    "frustrated", "hurt",  "crying"};
                            for (size_t ew = 0; ew < 12; ew++) {
                                if (strstr(combined, emo_words[ew]))
                                    emo_intensity += 0.15;
                            }
                            if (emo_intensity > 1.0)
                                emo_intensity = 1.0;
                        }
                        adjusted = (int32_t)hu_timing_adjust((uint64_t)adjusted, conv_depth,
                                                             emo_intensity, false, 0);
                        if (adjusted < 1000)
                            adjusted = 1000;
                        if (adjusted > 60000)
                            adjusted = 60000;
                    }
                    /* Humanness: emotional weight adds thoughtful pause for heavy content */
                    {
                        hu_emotional_weight_t ew =
                            hu_emotional_weight_classify(combined, combined_len);
                        adjusted = (int32_t)hu_emotional_pacing_adjust((uint64_t)adjusted, ew);
                        if (adjusted > 60000)
                            adjusted = 60000;
                    }
                    hu_log_info("human", agent ? agent->observer : NULL,
                                "reading delay: %dms (hour=%d)", (int)adjusted, bth_hour);
                    usleep((useconds_t)((unsigned int)adjusted * 1000));
                    hu_log_info("human", agent ? agent->observer : NULL,
                                "delay done, building context...");
                }
#else
                {
                    size_t msg_count = batch_end - batch_start + 1;
                    unsigned int read_ms = 2500 + (unsigned int)(msg_count * 1500);
                    if (read_ms > 8000)
                        read_ms = 8000;
                    read_ms += extra_delay_ms;
                    if (read_ms > 15000)
                        read_ms = 15000;
                    usleep(read_ms * 1000);
                }
#endif
            llm_decides_skip_delays:;

                /* Burst accumulation: re-poll for messages that arrived during
                 * the read delay. Humans finish their thought in 2-3 messages,
                 * so we pick up any follow-ups before responding. */
                {
                    hu_channel_loop_msg_t burst[16];
                    size_t burst_count = 0;
                    ch->poll_fn(ch->channel_ctx, alloc, burst, 16, &burst_count);
                    for (size_t bi = 0; bi < burst_count; bi++) {
                        if (strcmp(burst[bi].session_key, batch_key) != 0)
                            continue;
                        size_t mlen = strlen(burst[bi].content);
                        if (mlen == 0)
                            continue;
                        if (combined_len + mlen + 2 >= sizeof(combined))
                            break;
                        combined[combined_len++] = '\n';
                        memcpy(combined + combined_len, burst[bi].content, mlen);
                        combined_len += mlen;
                        if (agent->session_store && agent->session_store->vtable &&
                            agent->session_store->vtable->save_message) {
                            agent->session_store->vtable->save_message(agent->session_store->ctx,
                                                                       batch_key, key_len, "user",
                                                                       4, burst[bi].content, mlen);
                        }
                    }
                    combined[combined_len] = '\0';
                }

#ifdef HU_ENABLE_SQLITE
                /* F20: Commitment keeper — detect and store commitments from their message */
                if (agent && agent->memory && combined_len > 0) {
                    char desc_buf[512];
                    char who_buf[64];
                    if (hu_conversation_detect_commitment(combined, combined_len, desc_buf,
                                                          sizeof(desc_buf), who_buf,
                                                          sizeof(who_buf), false)) {
                        int64_t deadline = hu_conversation_parse_deadline(combined, combined_len,
                                                                          (int64_t)time(NULL));
                        (void)hu_superhuman_commitment_store(
                            agent->memory, alloc, batch_key, key_len, desc_buf,
                            (size_t)strlen(desc_buf), who_buf, (size_t)strlen(who_buf), deadline);
                    }
                    /* F24: Growth celebration — detect positive outcomes, store for later reference
                     */
                    {
                        char topic_buf[128];
                        char after_buf[64];
                        if (hu_conversation_detect_growth_opportunity(
                                combined, combined_len, topic_buf, sizeof(topic_buf), after_buf,
                                sizeof(after_buf))) {
                            (void)hu_superhuman_growth_store(
                                agent->memory, alloc, batch_key, key_len, topic_buf,
                                (size_t)strlen(topic_buf), "worried/stressed", 15, after_buf,
                                (size_t)strlen(after_buf));
                        }
                    }
                    /* F22: Pattern mirror — record topic + emotional tone for behavioral patterns
                     */
                    {
                        char topic_buf[64];
                        size_t topic_len = hu_conversation_extract_topic(
                            combined, combined_len, topic_buf, sizeof(topic_buf));
                        if (topic_len > 0) {
                            const char *tone =
                                hu_conversation_classify_emotional_tone(combined, combined_len);
                            size_t tone_len = tone ? strlen(tone) : 0;
                            if (tone_len > 0) {
                                time_t now = time(NULL);
                                struct tm tm_buf;
                                struct tm *tm = hu_platform_localtime_r(&now, &tm_buf);
                                int dow = tm ? tm->tm_wday : 0;
                                int hour = tm ? tm->tm_hour : 0;
                                (void)hu_superhuman_pattern_record(agent->memory, batch_key,
                                                                   key_len, topic_buf, topic_len,
                                                                   tone, tone_len, dow, hour);
                            }
                        }
                    }
                }
#endif

                /* Real-user-response window: if the real user already replied to
                 * this contact during our delay, stay silent (channels that implement
                 * human_active_recently). */
                {
                    const char *chn = ch->channel->vtable->name
                                          ? ch->channel->vtable->name(ch->channel->ctx)
                                          : NULL;
                    const hu_channel_daemon_config_t *dcfg_hu =
                        get_active_daemon_config(config, chn);
                    int window = 120;
                    if (dcfg_hu && dcfg_hu->user_response_window_sec > 0)
                        window = dcfg_hu->user_response_window_sec;
                    if (ch->channel->vtable->human_active_recently &&
                        ch->channel->vtable->human_active_recently(ch->channel->ctx, batch_key,
                                                                   key_len, window)) {
                        hu_log_info("human", agent ? agent->observer : NULL,
                                    "real user responded to %.*s within %ds — "
                                    "staying silent",
                                    (int)(key_len > 20 ? 20 : key_len), batch_key, window);
                        /* Reset consecutive counter — real user is active */
                        for (size_t ci = 0; ci < consec_contact_count; ci++) {
                            if (key_len < sizeof(consec_contact_keys[0]) &&
                                memcmp(consec_contact_keys[ci], batch_key, key_len) == 0 &&
                                consec_contact_keys[ci][key_len] == '\0') {
                                consec_response_count[ci] = 0;
                                break;
                            }
                        }
                        if (agent)
                            agent->lean_prompt = false;
                        continue;
                    }
                }
#else
                bool brief_mode = false;
#endif

                hu_agent_clear_history(agent);

                /* Set active channel for per-channel persona overlays */
                if (ch->channel->vtable->name) {
                    agent->active_channel = ch->channel->vtable->name(ch->channel->ctx);
                    agent->active_channel_len =
                        agent->active_channel ? strlen(agent->active_channel) : 0;
                } else {
                    agent->active_channel = NULL;
                    agent->active_channel_len = 0;
                }

                /* Apply persona override: per-contact takes priority, then per-channel */
                if (config) {
                    const char *persona_override = NULL;
                    if (batch_key && key_len > 0)
                        persona_override = hu_config_persona_for_contact(config, batch_key);
                    if (!persona_override && agent->active_channel)
                        persona_override =
                            hu_config_persona_for_channel(config, agent->active_channel);
                    if (persona_override && persona_override[0]) {
                        const char *current = agent->persona_name ? agent->persona_name : "";
                        if (strcmp(persona_override, current) != 0) {
                            hu_error_t perr = hu_agent_set_persona(agent, persona_override,
                                                                   strlen(persona_override));
                            if (perr != HU_OK) {
#ifndef HU_IS_TEST
                                hu_log_error("human", agent ? agent->observer : NULL,
                                             "warning: failed to switch persona to '%s'",
                                             persona_override);
#endif
                            }
                        }
                    }
                }

                /* Restore prior conversation for this sender */
                if (agent->session_store && agent->session_store->vtable &&
                    agent->session_store->vtable->load_messages) {
                    hu_message_entry_t *entries = NULL;
                    size_t entry_count = 0;
                    if (agent->session_store->vtable->load_messages(
                            agent->session_store->ctx, alloc, batch_key, key_len, &entries,
                            &entry_count) == HU_OK &&
                        entries && entry_count > 0) {
                        for (size_t e = 0; e < entry_count; e++) {
                            if (!entries[e].content || entries[e].content_len == 0)
                                continue;
                            hu_role_t role = HU_ROLE_USER;
                            if (entries[e].role) {
                                if (strcmp(entries[e].role, "assistant") == 0)
                                    role = HU_ROLE_ASSISTANT;
                                else if (strcmp(entries[e].role, "system") == 0)
                                    role = HU_ROLE_SYSTEM;
                            }
                            if (agent->history_count >= agent->history_cap) {
                                size_t new_cap;
                                if (!agent->history_cap)
                                    new_cap = 8;
                                else if (agent->history_cap > SIZE_MAX / 2)
                                    break;
                                else
                                    new_cap = agent->history_cap * 2;
                                hu_owned_message_t *arr = (hu_owned_message_t *)alloc->realloc(
                                    alloc->ctx, agent->history,
                                    agent->history_cap * sizeof(hu_owned_message_t),
                                    new_cap * sizeof(hu_owned_message_t));
                                if (!arr)
                                    break;
                                agent->history = arr;
                                agent->history_cap = new_cap;
                            }
                            hu_owned_message_t *hm = &agent->history[agent->history_count];
                            memset(hm, 0, sizeof(*hm));
                            hm->role = role;
                            hm->content = hu_strndup(agent->alloc, entries[e].content,
                                                     entries[e].content_len);
                            hm->content_len = entries[e].content_len;
                            if (hm->content)
                                agent->history_count++;
                        }
                        for (size_t e = 0; e < entry_count; e++) {
                            if (entries[e].role)
                                alloc->free(alloc->ctx, (void *)entries[e].role,
                                            entries[e].role_len + 1);
                            if (entries[e].content)
                                alloc->free(alloc->ctx, (void *)entries[e].content,
                                            entries[e].content_len + 1);
                        }
                        alloc->free(alloc->ctx, entries, entry_count * sizeof(hu_message_entry_t));
                    }
                }

                char *response = NULL;
                size_t response_len = 0;
                hu_error_t err = HU_OK;
                /* hu_agent_turn allocates response via agent->alloc; use agent->alloc for
                 * free/realloc to match. (agent->alloc == alloc at creation time.) */

                /* Build per-turn context via proper architecture:
                 * 1. Contact profile from persona (hu_persona_find_contact)
                 * 2. Conversation history from channel vtable (load_conversation_history)
                 * 3. Awareness from shared analyzer (hu_conversation_build_awareness)
                 * 4. Response constraints from channel vtable (get_response_constraints)
                 */
                char *contact_ctx = NULL;
                size_t contact_ctx_len = 0;
                char *convo_ctx = NULL;
                size_t convo_ctx_len = 0;
                hu_channel_history_entry_t *history_entries = NULL;
                size_t history_count = 0;
#if defined(HU_ENABLE_SQLITE) && !defined(HU_IS_TEST)
                char *cross_channel_ctx = NULL;
                size_t cross_channel_ctx_len = 0;
#endif
                const hu_contact_profile_t *contact_for_tapback = NULL;

#ifndef HU_IS_TEST
                hu_daemon_out_turn_state_t turn_out_state;
                memset(&turn_out_state, 0, sizeof(turn_out_state));
                /* 1. Per-contact profile via persona struct */
                if (agent->persona) {
                    const hu_contact_profile_t *cp =
                        hu_persona_find_contact(agent->persona, batch_key, key_len);
                    if (cp) {
                        contact_for_tapback = cp;
                        if (ch->channel->vtable->name && cp->contact_id) {
                            const char *act_ch = ch->channel->vtable->name(ch->channel->ctx);
                            if (act_ch) {
                                daemon_contact_activity_record(cp->contact_id, act_ch, batch_key);
#ifdef HU_ENABLE_SQLITE
                                /* Link this platform handle to canonical contact ID */
                                if (agent->memory) {
                                    sqlite3 *link_db = hu_sqlite_memory_get_db(agent->memory);
                                    if (link_db) {
                                        hu_contact_graph_link(link_db, cp->contact_id, act_ch,
                                                              batch_key, cp->name ? cp->name : "",
                                                              1.0);
                                    }
                                }
#endif
                            }
                        }
                        hu_contact_profile_build_context(alloc, cp, &contact_ctx, &contact_ctx_len);

                        size_t iw_len = 0;
                        char *iw_ctx = llm_decides ? NULL
                                                   : hu_persona_build_inner_world_context(
                                                         alloc, agent->persona,
                                                         cp->relationship_stage, &iw_len);
                        if (iw_ctx && iw_len > 0 && contact_ctx) {
                            size_t total = contact_ctx_len + iw_len + 1;
                            char *merged = (char *)alloc->alloc(alloc->ctx, total);
                            if (merged) {
                                memcpy(merged, contact_ctx, contact_ctx_len);
                                memcpy(merged + contact_ctx_len, iw_ctx, iw_len);
                                merged[total - 1] = '\0';
                                alloc->free(alloc->ctx, contact_ctx, contact_ctx_len + 1);
                                contact_ctx = merged;
                                contact_ctx_len = total - 1;
                            }
                            alloc->free(alloc->ctx, iw_ctx, iw_len + 1);
                        } else if (iw_ctx) {
                            alloc->free(alloc->ctx, iw_ctx, iw_len + 1);
                        }
                    }
                }

#ifndef HU_IS_TEST
                /* BTH: Ongoing per-contact style learning (b2c) — re-run every 10 convos,
                 * use all overlay fields, LRU eviction at cap.
                 * Skip in llm_decides: auto_profile may use LLM. */
                if (!llm_decides) {
#define HU_STYLE_CACHE_CAP        16
#define HU_STYLE_RELEARN_INTERVAL 10
                    static struct {
                        char key[64];
                        uint32_t convo_count;
                        uint64_t last_used;
                    } style_cache[HU_STYLE_CACHE_CAP];
                    static size_t style_cache_count = 0;
                    static uint64_t style_seq = 0;
                    style_seq++;

                    size_t slot = (size_t)-1;
                    for (size_t hu_i = 0; hu_i < style_cache_count; hu_i++) {
                        if (strncmp(style_cache[hu_i].key, batch_key, key_len) == 0 &&
                            style_cache[hu_i].key[key_len] == '\0') {
                            slot = hu_i;
                            style_cache[hu_i].convo_count++;
                            style_cache[hu_i].last_used = style_seq;
                            break;
                        }
                    }

                    if (slot == (size_t)-1 && key_len < 64) {
                        if (style_cache_count < HU_STYLE_CACHE_CAP) {
                            slot = style_cache_count++;
                        } else {
                            size_t lru = 0;
                            for (size_t hu_i = 1; hu_i < HU_STYLE_CACHE_CAP; hu_i++) {
                                if (style_cache[hu_i].last_used < style_cache[lru].last_used)
                                    lru = hu_i;
                            }
                            slot = lru;
                        }
                        memcpy(style_cache[slot].key, batch_key, key_len);
                        style_cache[slot].key[key_len] = '\0';
                        style_cache[slot].convo_count = 1;
                        style_cache[slot].last_used = style_seq;
                    }

                    bool should_profile = false;
                    if (slot != (size_t)-1) {
                        uint32_t cc = style_cache[slot].convo_count;
                        if (cc == 1 || (cc % HU_STYLE_RELEARN_INTERVAL) == 0)
                            should_profile = true;
                    }

                    if (should_profile) {
                        hu_persona_overlay_t auto_ov;
                        memset(&auto_ov, 0, sizeof(auto_ov));
                        if (hu_persona_auto_profile(alloc, batch_key, key_len, &auto_ov) == HU_OK) {
                            char profile_buf[1024];
                            int pb_n = 0;
                            profile_buf[0] = '\0';

                            if (auto_ov.formality) {
                                int w =
                                    snprintf(profile_buf + pb_n, sizeof(profile_buf) - (size_t)pb_n,
                                             "Contact formality: %s. ", auto_ov.formality);
                                if (w > 0 && (size_t)pb_n + (size_t)w < sizeof(profile_buf))
                                    pb_n += w;
                            }
                            if (auto_ov.avg_length) {
                                int w =
                                    snprintf(profile_buf + pb_n, sizeof(profile_buf) - (size_t)pb_n,
                                             "Avg message length: %s. ", auto_ov.avg_length);
                                if (w > 0 && (size_t)pb_n + (size_t)w < sizeof(profile_buf))
                                    pb_n += w;
                            }
                            if (auto_ov.emoji_usage) {
                                int w =
                                    snprintf(profile_buf + pb_n, sizeof(profile_buf) - (size_t)pb_n,
                                             "Emoji usage: %s. ", auto_ov.emoji_usage);
                                if (w > 0 && (size_t)pb_n + (size_t)w < sizeof(profile_buf))
                                    pb_n += w;
                            }
                            if (auto_ov.style_notes) {
                                for (size_t sn = 0;
                                     sn < auto_ov.style_notes_count && (size_t)pb_n < 900; sn++) {
                                    if (auto_ov.style_notes[sn]) {
                                        int w = snprintf(profile_buf + pb_n,
                                                         sizeof(profile_buf) - (size_t)pb_n, "%s ",
                                                         auto_ov.style_notes[sn]);
                                        if (w > 0 && (size_t)pb_n + (size_t)w < sizeof(profile_buf))
                                            pb_n += w;
                                    }
                                }
                            }

                            if (pb_n > 0) {
                                char *note = hu_strndup(alloc, profile_buf, (size_t)pb_n);
                                if (note) {
                                    if (contact_ctx) {
                                        size_t total = contact_ctx_len + (size_t)pb_n + 1;
                                        char *merged = (char *)alloc->alloc(alloc->ctx, total + 1);
                                        if (merged) {
                                            memcpy(merged, contact_ctx, contact_ctx_len);
                                            merged[contact_ctx_len] = '\n';
                                            memcpy(merged + contact_ctx_len + 1, note,
                                                   (size_t)pb_n);
                                            merged[total] = '\0';
                                            alloc->free(alloc->ctx, contact_ctx,
                                                        contact_ctx_len + 1);
                                            contact_ctx = merged;
                                            contact_ctx_len = total;
                                        }
                                        alloc->free(alloc->ctx, note, (size_t)pb_n + 1);
                                    } else {
                                        contact_ctx = note;
                                        contact_ctx_len = (size_t)pb_n;
                                    }
                                }
                            }

                            if (auto_ov.formality)
                                alloc->free(alloc->ctx, (char *)auto_ov.formality,
                                            strlen(auto_ov.formality) + 1);
                            if (auto_ov.avg_length)
                                alloc->free(alloc->ctx, (char *)auto_ov.avg_length,
                                            strlen(auto_ov.avg_length) + 1);
                            if (auto_ov.emoji_usage)
                                alloc->free(alloc->ctx, (char *)auto_ov.emoji_usage,
                                            strlen(auto_ov.emoji_usage) + 1);
                            if (auto_ov.style_notes) {
                                for (size_t sn = 0; sn < auto_ov.style_notes_count; sn++) {
                                    if (auto_ov.style_notes[sn])
                                        alloc->free(alloc->ctx, auto_ov.style_notes[sn],
                                                    strlen(auto_ov.style_notes[sn]) + 1);
                                }
                                alloc->free(alloc->ctx, auto_ov.style_notes,
                                            auto_ov.style_notes_count * sizeof(char *));
                            }
                            if (auto_ov.typing_quirks) {
                                for (size_t tq = 0; tq < auto_ov.typing_quirks_count; tq++) {
                                    if (auto_ov.typing_quirks[tq])
                                        alloc->free(alloc->ctx, (char *)auto_ov.typing_quirks[tq],
                                                    strlen(auto_ov.typing_quirks[tq]) + 1);
                                }
                                alloc->free(alloc->ctx, auto_ov.typing_quirks,
                                            auto_ov.typing_quirks_count * sizeof(char *));
                            }
                        }
                    }
                }
#endif

                /* 2. Conversation history via channel vtable */
                if (ch->channel->vtable->load_conversation_history) {
                    ch->channel->vtable->load_conversation_history(
                        ch->channel->ctx, alloc, batch_key, key_len, 25, &history_entries,
                        &history_count);
                }

#ifndef HU_IS_TEST
                /* F27: If we have pending comfort record for this contact, their current message
                 * is their reply. Score engagement and record, then clear pending. */
                if (agent->memory) {
                    for (size_t cp_i = 0; cp_i < HU_COMFORT_PENDING_MAX; cp_i++) {
                        if (comfort_pending[cp_i].key[0] == '\0')
                            continue;
                        if (key_len < sizeof(comfort_pending[0].key) &&
                            memcmp(comfort_pending[cp_i].key, batch_key, key_len) == 0 &&
                            comfort_pending[cp_i].key[key_len] == '\0') {
                            float eng = score_comfort_engagement(combined, combined_len);
                            (void)hu_comfort_pattern_record(
                                agent->memory, batch_key, key_len, comfort_pending[cp_i].emotion,
                                strlen(comfort_pending[cp_i].emotion),
                                comfort_pending[cp_i].response_type,
                                strlen(comfort_pending[cp_i].response_type), eng);
                            comfort_pending[cp_i].key[0] = '\0';
                            break;
                        }
                    }
                }
#endif

                /* Primary-channel history only for ToM / awareness (no cross-merge). */
                const hu_channel_history_entry_t *ctx_entries = history_entries;
                size_t ctx_count = history_count;

#if defined(HU_ENABLE_SQLITE) && !defined(HU_IS_TEST)
                /* 2b. Cross-channel awareness: other platforms for the same contact (contact
                 * graph). Formatted lines are prepended to convo_ctx for the LLM, not merged into
                 * history. */
                if (agent->memory && ch->channel->vtable->name && batch_key && key_len > 0 &&
                    key_len < 512 && !llm_decides) {
                    sqlite3 *cg_db = hu_sqlite_memory_get_db(agent->memory);
                    if (cg_db) {
                        const char *cur_plat = ch->channel->vtable->name(ch->channel->ctx);
                        if (cur_plat && cur_plat[0]) {
                            char handle_buf[512];
                            memcpy(handle_buf, batch_key, key_len);
                            handle_buf[key_len] = '\0';
                            char canonical[128];
                            if (hu_contact_graph_resolve(cg_db, cur_plat, handle_buf, canonical,
                                                         sizeof(canonical)) == HU_OK) {
                                hu_contact_identity_t *idents = NULL;
                                size_t id_count = 0;
                                if (hu_contact_graph_list(alloc, cg_db, canonical, &idents,
                                                          &id_count) == HU_OK &&
                                    idents && id_count > 0) {
                                    for (size_t ci = 0; ci < channel_count; ci++) {
                                        if (&channels[ci] == ch)
                                            continue;
                                        hu_channel_t *och = channels[ci].channel;
                                        if (!och || !och->vtable ||
                                            !och->vtable->load_conversation_history ||
                                            !och->vtable->name)
                                            continue;
                                        const char *oname = och->vtable->name(och->ctx);
                                        if (!oname || !oname[0] || strcmp(oname, cur_plat) == 0)
                                            continue;
                                        const char *ohandle = NULL;
                                        for (size_t ij = 0; ij < id_count; ij++) {
                                            if (strcmp(idents[ij].platform, oname) == 0 &&
                                                idents[ij].platform_handle[0]) {
                                                ohandle = idents[ij].platform_handle;
                                                break;
                                            }
                                        }
                                        if (!ohandle)
                                            continue;
                                        hu_channel_history_entry_t *oent = NULL;
                                        size_t onc = 0;
                                        hu_error_t oh = och->vtable->load_conversation_history(
                                            och->ctx, alloc, ohandle, strlen(ohandle), 5, &oent,
                                            &onc);
                                        if (oh == HU_OK && oent && onc > 0) {
                                            size_t start = onc > 5 ? onc - 5 : 0;
                                            char plabel[64];
                                            cross_channel_platform_label(oname, plabel,
                                                                         sizeof(plabel));
                                            for (size_t ei = start; ei < onc; ei++) {
                                                char when[48];
                                                cross_channel_format_when(when, sizeof(when),
                                                                          oent[ei].timestamp);
                                                const char *role = oent[ei].from_me ? " (you)" : "";
                                                char line[768];
                                                int lw = snprintf(line, sizeof(line),
                                                                  "[From %s, %s]%s %s", plabel,
                                                                  when, role, oent[ei].text);
                                                if (lw > 0 && (size_t)lw < sizeof(line)) {
                                                    (void)daemon_cross_ctx_append_line(
                                                        alloc, &cross_channel_ctx,
                                                        &cross_channel_ctx_len, line, (size_t)lw);
                                                }
                                            }
                                            alloc->free(alloc->ctx, oent,
                                                        onc * sizeof(hu_channel_history_entry_t));
                                        }
                                    }
                                    alloc->free(alloc->ctx, idents,
                                                id_count * sizeof(hu_contact_identity_t));
                                }
                            }
                        }
                    }
                }
#endif /* HU_ENABLE_SQLITE && !HU_IS_TEST */

                /* Phase 6 (F59–F69): Build prefix context before awareness.
                 * Order: life sim, mood, ToM, anticipatory, self-awareness, life chapter,
                 * social graph, humor. These are prepended to hu_conversation_build_awareness. */
                char *phase6_prefix = NULL;
                size_t phase6_len = 0;
                if (agent && agent->persona && !llm_decides) {
                    const hu_contact_profile_t *cp_p6 =
                        hu_persona_find_contact(agent->persona, batch_key, key_len);
                    (void)cp_p6;
#define PHASE6_APPEND(str, len)                                                           \
    do {                                                                                  \
        if ((str) && (len) > 0) {                                                         \
            size_t new_len = phase6_len + (len) + (phase6_len ? 2 : 1);                   \
            char *p6m = (char *)alloc->realloc(alloc->ctx, phase6_prefix,                 \
                                               phase6_len ? phase6_len + 1 : 0, new_len); \
            if (p6m) {                                                                    \
                phase6_prefix = p6m;                                                      \
                if (phase6_len) {                                                         \
                    phase6_prefix[phase6_len] = '\n';                                     \
                    phase6_prefix[phase6_len + 1] = '\n';                                 \
                    memcpy(phase6_prefix + phase6_len + 2, (str), (len));                 \
                } else                                                                    \
                    memcpy(phase6_prefix, (str), (len));                                  \
                phase6_prefix[new_len - 1] = '\0';                                        \
                phase6_len = new_len - 1;                                                 \
                alloc->free(alloc->ctx, (str), (len) + 1);                                \
            } else if ((str))                                                             \
                alloc->free(alloc->ctx, (str), (len) + 1);                                \
        }                                                                                 \
    } while (0)

                    /* 1. Life sim context (F59) */
                    if (agent->persona->daily_routine.weekday_count > 0 ||
                        agent->persona->daily_routine.weekend_count > 0) {
                        time_t now_ts = time(NULL);
                        struct tm tm_buf;
                        struct tm *lt = hu_platform_localtime_r(&now_ts, &tm_buf);
                        int dow = lt ? lt->tm_wday : 0;
                        uint32_t seed = (uint32_t)now_ts * 1103515245u + 12345u;
                        hu_life_sim_state_t ls_state = hu_life_sim_get_current(
                            &agent->persona->daily_routine, (int64_t)now_ts, dow, seed);
                        size_t ls_len = 0;
                        char *ls_ctx = hu_life_sim_build_context(alloc, &ls_state, &ls_len);
                        if (ls_ctx && ls_len > 0) {
                            phase6_prefix = ls_ctx;
                            phase6_len = ls_len;
                        } else if (ls_ctx) {
                            alloc->free(alloc->ctx, ls_ctx, ls_len + 1);
                        }
                    }

                    /* 2. Current mood (F60) — persona mood directive */
#if defined(HU_ENABLE_SQLITE)
                    if (agent->memory) {
                        hu_mood_state_t mood_state;
                        memset(&mood_state, 0, sizeof(mood_state));
                        if (hu_mood_get_current(alloc, agent->memory, &mood_state) == HU_OK) {
                            size_t mood_len = 0;
                            char *mood_dir = hu_mood_build_directive(alloc, &mood_state, &mood_len);
                            if (mood_dir && mood_len > 0)
                                PHASE6_APPEND(mood_dir, mood_len);
                            else if (mood_dir)
                                alloc->free(alloc->ctx, mood_dir, mood_len + 1);
                        }
                    }
#endif

                    /* 3. Theory of Mind inference (F58) */
#ifdef HU_ENABLE_SQLITE
                    if (agent->memory && ctx_entries && ctx_count > 0) {
                        hu_contact_baseline_t tom_baseline;
                        memset(&tom_baseline, 0, sizeof(tom_baseline));
                        if (hu_theory_of_mind_get_baseline(agent->memory, alloc, batch_key, key_len,
                                                           &tom_baseline) == HU_OK &&
                            tom_baseline.messages_sampled >= 5) {
                            hu_theory_of_mind_deviation_t dev = hu_theory_of_mind_detect_deviation(
                                &tom_baseline, ctx_entries, ctx_count);
                            if (dev.severity >= 0.3f) {
                                const char *contact_name = NULL;
                                size_t name_len = 0;
                                if (cp_p6 && cp_p6->name) {
                                    contact_name = cp_p6->name;
                                    name_len = strlen(cp_p6->name);
                                } else {
                                    contact_name = batch_key;
                                    name_len = key_len;
                                }
                                size_t tom_len = 0;
                                char *tom_inf = hu_theory_of_mind_build_inference(
                                    alloc, contact_name, name_len, NULL, 0, &dev, &tom_len);
                                if (tom_inf && tom_len > 0)
                                    PHASE6_APPEND(tom_inf, tom_len);
                                else if (tom_inf)
                                    alloc->free(alloc->ctx, tom_inf, tom_len + 1);
                            }
                        }
                    }
#endif

                    /* 4. Anticipatory predictions (F64) */
#ifdef HU_ENABLE_SQLITE
                    if (agent->memory) {
                        hu_emotional_prediction_t *preds = NULL;
                        size_t pred_count = 0;
                        if (hu_anticipatory_predict_with_provider(
                                alloc, agent->memory, &agent->provider, agent->model_name,
                                agent->model_name_len, batch_key, key_len, (int64_t)time(NULL),
                                &preds, &pred_count) == HU_OK &&
                            preds && pred_count > 0) {
                            const char *cname = (cp_p6 && cp_p6->name) ? cp_p6->name : batch_key;
                            size_t cname_len =
                                (cp_p6 && cp_p6->name) ? strlen(cp_p6->name) : key_len;
                            size_t ant_len = 0;
                            char *ant_dir = hu_anticipatory_build_directive(
                                alloc, preds, pred_count, cname, cname_len, &ant_len);
                            hu_anticipatory_predictions_free(alloc, preds, pred_count);
                            if (ant_dir && ant_len > 0)
                                PHASE6_APPEND(ant_dir, ant_len);
                            else if (ant_dir)
                                alloc->free(alloc->ctx, ant_dir, ant_len + 1);
                        }
                    }
#endif

                    /* 5. Self-awareness / reciprocity (F62, F63) */
#ifdef HU_ENABLE_SQLITE
                    if (agent->memory) {
                        char *sa_dir = NULL;
                        size_t sa_len = 0;
                        if (hu_self_awareness_build_directive_from_memory(
                                alloc, agent->memory, batch_key, key_len, (int64_t)time(NULL),
                                &sa_dir, &sa_len) == HU_OK &&
                            sa_dir && sa_len > 0) {
                            PHASE6_APPEND(sa_dir, sa_len);
                        }
                        char *rec_dir = NULL;
                        size_t rec_len = 0;
                        if (hu_self_awareness_build_reciprocity_directive(
                                alloc, agent->memory, batch_key, key_len, &rec_dir, &rec_len) ==
                                HU_OK &&
                            rec_dir && rec_len > 0) {
                            PHASE6_APPEND(rec_dir, rec_len);
                        }
                    }
#endif

                    /* 6. Life chapter (F66) */
#ifdef HU_ENABLE_SQLITE
                    if (agent->memory) {
                        hu_life_chapter_t lc = {0};
                        if (hu_life_chapter_get_active(alloc, agent->memory, &lc) == HU_OK &&
                            lc.theme[0]) {
                            size_t lc_len = 0;
                            char *lc_dir = hu_life_chapter_build_directive(alloc, &lc, &lc_len);
                            if (lc_dir && lc_len > 0)
                                PHASE6_APPEND(lc_dir, lc_len);
                            else if (lc_dir)
                                alloc->free(alloc->ctx, lc_dir, lc_len + 1);
                        } else if (agent->persona && agent->persona->current_chapter.theme[0]) {
                            size_t lc_len = 0;
                            char *lc_dir = hu_life_chapter_build_directive(
                                alloc, &agent->persona->current_chapter, &lc_len);
                            if (lc_dir && lc_len > 0)
                                PHASE6_APPEND(lc_dir, lc_len);
                            else if (lc_dir)
                                alloc->free(alloc->ctx, lc_dir, lc_len + 1);
                        }
                    }
#endif

                    /* 7. Social network (F67) */
#ifdef HU_ENABLE_SQLITE
                    if (agent->memory) {
                        hu_relationship_t *rels = NULL;
                        size_t rel_count = 0;
                        if (hu_social_graph_get(alloc, agent->memory, batch_key, key_len, &rels,
                                                &rel_count) == HU_OK &&
                            rels && rel_count > 0) {
                            const char *cname = (cp_p6 && cp_p6->name) ? cp_p6->name : batch_key;
                            size_t cname_len =
                                (cp_p6 && cp_p6->name) ? strlen(cp_p6->name) : key_len;
                            size_t sg_len = 0;
                            char *sg_dir = hu_social_graph_build_directive(
                                alloc, cname, cname_len, rels, rel_count, &sg_len);
                            hu_social_graph_free(alloc, rels, rel_count);
                            if (sg_dir && sg_len > 0)
                                PHASE6_APPEND(sg_dir, sg_len);
                            else if (sg_dir)
                                alloc->free(alloc->ctx, sg_dir, sg_len + 1);
                        }
                    }
#endif

                    /* 8. Timezone awareness (F54) — inject local-time context */
                    if (cp_p6 && agent->persona && agent->persona->timezone[0]) {
                        const char *tzs = agent->persona->timezone;
                        int tz_offset = 0;
                        if (strncasecmp(tzs, "UTC", 3) == 0 || strncasecmp(tzs, "GMT", 3) == 0)
                            tz_offset = atoi(tzs + 3);
                        else if (tzs[0] == '+' || tzs[0] == '-')
                            tz_offset = atoi(tzs);
                        if (tz_offset != 0) {
                            uint64_t utc_now_ms = (uint64_t)time(NULL) * 1000ULL;
                            hu_timezone_info_t tz = hu_timezone_compute(tz_offset, utc_now_ms);
                            const char *cname_tz = cp_p6->name ? cp_p6->name : batch_key;
                            size_t cname_tz_len = cp_p6->name ? strlen(cp_p6->name) : key_len;
                            size_t tz_len = 0;
                            char *tz_dir = NULL;
                            if (hu_timezone_build_directive(alloc, &tz, cname_tz, cname_tz_len,
                                                            &tz_dir, &tz_len) == HU_OK &&
                                tz_dir && tz_len > 0)
                                PHASE6_APPEND(tz_dir, tz_len);
                            else if (tz_dir)
                                alloc->free(alloc->ctx, tz_dir, tz_len + 1);
                        }
                    }

                    /* 9. Humor (F69) — persona directive + strategy (Phase 3) */
                    if (agent->persona && agent->persona->humor.type) {
                        hu_emotional_state_t emo_humor =
                            history_entries && history_count > 0
                                ? hu_daemon_detect_emotion(alloc, agent, history_entries,
                                                           history_count)
                                : (hu_emotional_state_t){0};
                        bool playful = (combined_len > 0) &&
                                       (strstr(combined, "lol") || strstr(combined, "haha") ||
                                        strstr(combined, "😂") || strstr(combined, "😄"));
                        if (playful && !emo_humor.concerning) {
                            /* Check timing appropriateness (Phase 3) */
                            time_t hum_now = time(NULL);
                            struct tm hum_tm;
                            int hum_hour = 12;
                            if (hu_platform_localtime_r(&hum_now, &hum_tm))
                                hum_hour = hum_tm.tm_hour;
                            float hum_valence = emo_humor.valence;
                            const char *rel_stage = (cp_p6 && cp_p6->relationship_stage)
                                                        ? cp_p6->relationship_stage
                                                        : "friend";
                            hu_humor_timing_result_t timing =
                                hu_humor_check_timing(hum_hour, hum_valence, false, rel_stage);

                            if (timing.allowed) {
                                const char *dom = emo_humor.dominant_emotion
                                                      ? emo_humor.dominant_emotion
                                                      : "neutral";
                                size_t hum_len = 0;
                                char *hum_dir = hu_humor_build_persona_directive(
                                    alloc, &agent->persona->humor, dom, strlen(dom), true,
                                    &hum_len);
                                if (hum_dir && hum_len > 0)
                                    PHASE6_APPEND(hum_dir, hum_len);
                                else if (hum_dir)
                                    alloc->free(alloc->ctx, hum_dir, hum_len + 1);

                                /* Generate humor strategy from audience model (Phase 3) */
#ifdef HU_ENABLE_SQLITE
                                if (agent->memory) {
                                    sqlite3 *hum_db = hu_sqlite_memory_get_db(agent->memory);
                                    if (hum_db) {
                                        hu_humor_audience_t audience = {0};
                                        (void)hu_humor_audience_load(hum_db, batch_key, &audience);
                                        char topic_hint[80] = {0};
                                        size_t th_len = combined_len < sizeof(topic_hint) - 1
                                                            ? combined_len
                                                            : sizeof(topic_hint) - 1;
                                        memcpy(topic_hint, combined, th_len);
                                        topic_hint[th_len] = '\0';
                                        size_t strat_len = 0;
                                        char *strat_dir = hu_humor_generate_strategy(
                                            alloc, &audience, topic_hint, hum_valence, rel_stage,
                                            &agent->persona->humor, &strat_len);
                                        if (strat_dir && strat_len > 0)
                                            PHASE6_APPEND(strat_dir, strat_len);
                                        else if (strat_dir)
                                            alloc->free(alloc->ctx, strat_dir, strat_len + 1);
                                    }
                                }
#endif
                            }
                        }
                    }

                    /* 10. Weather awareness (F51) — inject notable weather when location available
                     */
                    if (agent->persona && agent->persona->location[0]) {
                        hu_weather_context_t wx = {0};
                        (void)hu_weather_fetch(alloc, agent->persona->location,
                                               strlen(agent->persona->location), NULL, &wx);
                        time_t wx_now = time(NULL);
                        struct tm wx_tm;
                        uint8_t wx_hour = 12;
                        if (hu_platform_localtime_r(&wx_now, &wx_tm))
                            wx_hour = (uint8_t)wx_tm.tm_hour;
                        if (hu_weather_awareness_should_mention(&wx, wx_hour)) {
                            char *wx_dir = NULL;
                            size_t wx_len = 0;
                            if (hu_weather_awareness_build_directive(alloc, &wx, wx_hour, &wx_dir,
                                                                     &wx_len) == HU_OK &&
                                wx_dir && wx_len > 0)
                                PHASE6_APPEND(wx_dir, wx_len);
                            else if (wx_dir)
                                alloc->free(alloc->ctx, wx_dir, wx_len + 1);
                        }
                    }

                    /* 11. Inner thought surfacing (Phase 3) — inject accumulated thoughts
                     * that are relevant to the current conversation topic */
                    if (inner_thought_store_ok && combined_len > 0) {
                        hu_inner_thought_t *surfaced_thoughts[HU_INNER_THOUGHT_MAX_SURFACE];
                        char topic_hint[128] = {0};
                        size_t hint_len = combined_len < sizeof(topic_hint) - 1
                                              ? combined_len
                                              : sizeof(topic_hint) - 1;
                        memcpy(topic_hint, combined, hint_len);
                        topic_hint[hint_len] = '\0';
                        uint64_t now_ms = (uint64_t)time(NULL) * 1000ULL;
                        size_t surfaced_count = hu_inner_thought_surface(
                            &inner_thought_store, batch_key, key_len, topic_hint, hint_len, now_ms,
                            surfaced_thoughts, HU_INNER_THOUGHT_MAX_SURFACE);
                        if (surfaced_count > 0) {
                            /* Build a directive from surfaced thoughts */
                            char thought_dir[512];
                            int tdir_len = 0;
                            for (size_t si = 0; si < surfaced_count && si < 3; si++) {
                                const hu_inner_thought_t *th = surfaced_thoughts[si];
                                if (th->thought_text && th->thought_text_len > 0) {
                                    int wrote =
                                        snprintf(thought_dir + tdir_len,
                                                 sizeof(thought_dir) - (size_t)tdir_len,
                                                 "%s[Inner thought: %.*s]", tdir_len > 0 ? " " : "",
                                                 (int)th->thought_text_len, th->thought_text);
                                    if (wrote > 0 &&
                                        (size_t)wrote < sizeof(thought_dir) - (size_t)tdir_len)
                                        tdir_len += wrote;
                                }
                            }
                            if (tdir_len > 0) {
                                size_t td_alloc_len = (size_t)tdir_len;
                                char *td_copy =
                                    (char *)alloc->realloc(alloc->ctx, NULL, 0, td_alloc_len + 1);
                                if (td_copy) {
                                    memcpy(td_copy, thought_dir, td_alloc_len);
                                    td_copy[td_alloc_len] = '\0';
                                    PHASE6_APPEND(td_copy, td_alloc_len);
                                }
                            }
                        }
                    }

                    /* 12. Temporal reasoning (Phase 3) — seasonal awareness, anniversaries,
                     * life transitions */
                    {
                        time_t temp_now = time(NULL);
                        struct tm temp_tm;
                        if (hu_platform_localtime_r(&temp_now, &temp_tm)) {
                            int cur_month = temp_tm.tm_mon + 1;
                            int cur_day = temp_tm.tm_mday;
                            int cur_year = temp_tm.tm_year + 1900;

                            /* Check anniversaries from persona important_dates */
                            hu_anniversary_t ann_buf[8];
                            size_t ann_count = 0;
                            if (agent->persona && agent->persona->important_dates &&
                                agent->persona->important_dates_count > 0) {
                                hu_date_entry_t date_entries[16];
                                size_t de_count = 0;
                                for (size_t di = 0;
                                     di < agent->persona->important_dates_count && de_count < 16;
                                     di++) {
                                    const hu_important_date_t *id =
                                        &agent->persona->important_dates[di];
                                    if (id->date[0] && id->date[2] == '-') {
                                        date_entries[de_count].label = id->type;
                                        date_entries[de_count].label_len = strlen(id->type);
                                        date_entries[de_count].month =
                                            (id->date[0] - '0') * 10 + (id->date[1] - '0');
                                        date_entries[de_count].day =
                                            (id->date[3] - '0') * 10 + (id->date[4] - '0');
                                        de_count++;
                                    }
                                }
                                if (de_count > 0)
                                    ann_count = hu_temporal_check_anniversaries(
                                        date_entries, de_count, cur_year, cur_month, cur_day, 7,
                                        ann_buf, 8);
                            }

                            /* Detect life transitions from recent messages */
                            hu_life_transition_t transition = HU_TRANSITION_NONE;
                            if (combined_len > 0) {
                                hu_temporal_message_t tmsg = {.text = combined,
                                                              .text_len = combined_len};
                                transition = hu_temporal_detect_life_transition(&tmsg, 1);
                            }

                            /* Build temporal context directive */
                            char *temp_dir = NULL;
                            size_t temp_dir_len = 0;
                            if (hu_temporal_build_context(alloc, cur_month, cur_day, ann_buf,
                                                          ann_count, transition, &temp_dir,
                                                          &temp_dir_len) == HU_OK &&
                                temp_dir && temp_dir_len > 0)
                                PHASE6_APPEND(temp_dir, temp_dir_len);
                            else if (temp_dir)
                                alloc->free(alloc->ctx, temp_dir, temp_dir_len + 1);

                            /* ann_buf[].label is borrowed from persona important_dates — do NOT
                             * free */
                        }
                    }

                    /* 13. Anti-sycophancy (Phase 3) — check existing opinions before agreeing,
                     * and inject contrarian prompt on ~15% budget */
#ifdef HU_ENABLE_SQLITE
                    if (agent->memory && combined_len > 0) {
                        sqlite3 *syc_db = hu_sqlite_memory_get_db(agent->memory);
                        if (syc_db) {
                            /* Extract rough topic from user message for opinion lookup */
                            char syc_topic[128];
                            size_t syc_topic_len = combined_len < sizeof(syc_topic) - 1
                                                       ? combined_len
                                                       : sizeof(syc_topic) - 1;
                            memcpy(syc_topic, combined, syc_topic_len);
                            syc_topic[syc_topic_len] = '\0';

                            /* Check if user's message touches an existing opinion */
                            size_t cba_len = 0;
                            char *cba_dir = hu_opinion_check_before_agree(alloc, syc_db, syc_topic,
                                                                          syc_topic_len, &cba_len);
                            if (cba_dir && cba_len > 0)
                                PHASE6_APPEND(cba_dir, cba_len);
                            else if (cba_dir)
                                alloc->free(alloc->ctx, cba_dir, cba_len + 1);

                            /* ~15% random contrarian prompt */
                            size_t cp_len = 0;
                            char *cp_dir = hu_opinion_contrarian_prompt(
                                alloc, syc_topic, syc_topic_len, daemon_turn_counter, &cp_len);
                            if (cp_dir && cp_len > 0)
                                PHASE6_APPEND(cp_dir, cp_len);
                            else if (cp_dir)
                                alloc->free(alloc->ctx, cp_dir, cp_len + 1);
                        }
                    }
#endif /* HU_ENABLE_SQLITE anti-sycophancy */

                    /* ── Phase 4 pre-turn: style drift correction ─────── */
#if defined(HU_ENABLE_SQLITE)
                    if (++drift_check_counter % 5 == 0 && agent->memory) {
                        hu_style_drift_result_t drift_res = {0};
                        hu_style_fingerprint_t drift_bl = {0};
                        if (agent->persona) {
                            drift_bl.avg_message_length = (int)agent->persona->avg_message_length;
                            if (agent->persona->signature_phrases_count > 0 &&
                                agent->persona->signature_phrases)
                                snprintf(drift_bl.common_phrases, sizeof(drift_bl.common_phrases),
                                         "%s", agent->persona->signature_phrases[0]);
                        }
                        if (hu_style_drift_check(agent->memory, alloc, &drift_bl, &drift_res) ==
                                HU_OK &&
                            drift_res.corrective && drift_res.directive[0] != '\0') {
                            size_t dlen = strlen(drift_res.directive);
                            char *dc = (char *)alloc->alloc(alloc->ctx, dlen + 1);
                            if (dc) {
                                memcpy(dc, drift_res.directive, dlen + 1);
                                PHASE6_APPEND(dc, dlen);
                            }
                        }
                    }
#endif

                    /* ── Phase 4 pre-turn: conversation repair acknowledgment ── */
                    if (repair_signal.should_acknowledge && repair_signal.directive[0] != '\0') {
                        size_t rlen = strlen(repair_signal.directive);
                        char *rc = (char *)alloc->alloc(alloc->ctx, rlen + 1);
                        if (rc) {
                            memcpy(rc, repair_signal.directive, rlen + 1);
                            PHASE6_APPEND(rc, rlen);
                        }
                        memset(&repair_signal, 0, sizeof(repair_signal));
                    }

                    /* Phase 7 (F72–F76): Prospective memory, emotional residue, episodic context */
#ifdef HU_ENABLE_SQLITE
                    if (agent->memory && batch_key && key_len > 0) {
                        sqlite3 *db = hu_sqlite_memory_get_db(agent->memory);
                        if (db) {
                            int64_t now_ts = (int64_t)time(NULL);

                            /* 9. Prospective memory — check triggers from current message */
                            if (combined_len > 0) {
                                hu_prospective_entry_t *prosp_entries = NULL;
                                size_t prosp_count = 0;
                                if (hu_prospective_check_triggers(
                                        alloc, db, "keyword", combined, combined_len, batch_key,
                                        key_len, now_ts, &prosp_entries, &prosp_count) == HU_OK &&
                                    prosp_entries && prosp_count > 0) {
                                    char prosp_buf[1024];
                                    size_t prosp_pos = 0;
                                    int n = snprintf(prosp_buf, sizeof(prosp_buf),
                                                     "[PROSPECTIVE MEMORY: Remember to: ");
                                    if (n > 0 && (size_t)n < sizeof(prosp_buf))
                                        prosp_pos = (size_t)n;
                                    for (size_t pi = 0; pi < prosp_count && pi < 3 &&
                                                        prosp_pos < sizeof(prosp_buf) - 64;
                                         pi++) {
                                        if (pi > 0) {
                                            prosp_buf[prosp_pos++] = ' ';
                                            prosp_buf[prosp_pos++] = '|';
                                            prosp_buf[prosp_pos++] = ' ';
                                        }
                                        int w = snprintf(
                                            prosp_buf + prosp_pos, sizeof(prosp_buf) - prosp_pos,
                                            "%s (triggered by: %s)", prosp_entries[pi].action,
                                            prosp_entries[pi].trigger_value);
                                        if (w > 0 && prosp_pos + (size_t)w < sizeof(prosp_buf))
                                            prosp_pos += (size_t)w;
                                    }
                                    if (prosp_pos + 2 < sizeof(prosp_buf)) {
                                        prosp_buf[prosp_pos++] = ']';
                                        prosp_buf[prosp_pos] = '\0';
                                        char *prosp_str =
                                            (char *)alloc->alloc(alloc->ctx, prosp_pos + 1);
                                        if (prosp_str) {
                                            memcpy(prosp_str, prosp_buf, prosp_pos + 1);
                                            PHASE6_APPEND(prosp_str, prosp_pos);
                                        }
                                    }
                                    alloc->free(alloc->ctx, prosp_entries,
                                                prosp_count * sizeof(hu_prospective_entry_t));
                                }
                            }

                            /* 10. Emotional residue — active valence/intensity for this contact */
                            {
                                hu_emotional_residue_t *residues = NULL;
                                size_t res_count = 0;
                                if (hu_emotional_residue_get_active(alloc, db, batch_key, key_len,
                                                                    now_ts, &residues,
                                                                    &res_count) == HU_OK &&
                                    residues && res_count > 0) {
                                    size_t dir_len = 0;
                                    char *dir = hu_emotional_residue_build_directive(
                                        alloc, residues, res_count, &dir_len);
                                    if (dir && dir_len > 0)
                                        PHASE6_APPEND(dir, dir_len);
                                    else if (dir)
                                        alloc->free(alloc->ctx, dir, dir_len + 1);
                                    alloc->free(alloc->ctx, residues,
                                                res_count * sizeof(hu_emotional_residue_t));
                                }
                            }

                            /* 10b. Emotional residue carryover — conversation-opening tone shift
                             * when starting a new conversation after a heavy prior exchange */
                            if (agent->history_count == 0) {
                                hu_emotional_residue_t *carry_res = NULL;
                                size_t carry_count = 0;
                                if (hu_emotional_residue_get_active(alloc, db, batch_key, key_len,
                                                                    now_ts, &carry_res,
                                                                    &carry_count) == HU_OK &&
                                    carry_res && carry_count > 0) {
                                    double *valences = (double *)alloc->alloc(
                                        alloc->ctx, carry_count * sizeof(double));
                                    double *intensities = (double *)alloc->alloc(
                                        alloc->ctx, carry_count * sizeof(double));
                                    int64_t *timestamps = (int64_t *)alloc->alloc(
                                        alloc->ctx, carry_count * sizeof(int64_t));
                                    if (valences && intensities && timestamps) {
                                        for (size_t cr = 0; cr < carry_count; cr++) {
                                            valences[cr] = carry_res[cr].valence;
                                            intensities[cr] = carry_res[cr].intensity;
                                            timestamps[cr] = carry_res[cr].created_at;
                                        }
                                        hu_residue_carryover_t carryover = {0};
                                        if (hu_residue_carryover_compute(
                                                valences, intensities, timestamps, carry_count,
                                                now_ts, &carryover) == HU_OK) {
                                            size_t carry_dir_len = 0;
                                            char *carry_dir = hu_residue_carryover_build_directive(
                                                alloc, &carryover, &carry_dir_len);
                                            if (carry_dir && carry_dir_len > 0)
                                                PHASE6_APPEND(carry_dir, carry_dir_len);
                                            else if (carry_dir)
                                                alloc->free(alloc->ctx, carry_dir,
                                                            carry_dir_len + 1);
                                        }
                                    }
                                    if (valences)
                                        alloc->free(alloc->ctx, valences,
                                                    carry_count * sizeof(double));
                                    if (intensities)
                                        alloc->free(alloc->ctx, intensities,
                                                    carry_count * sizeof(double));
                                    if (timestamps)
                                        alloc->free(alloc->ctx, timestamps,
                                                    carry_count * sizeof(int64_t));
                                    alloc->free(alloc->ctx, carry_res,
                                                carry_count * sizeof(hu_emotional_residue_t));
                                }
                            }

                            /* 10c. Evolving opinions — inject developed perspectives */
                            {
                                hu_evolved_opinions_ensure_table(db);
                                hu_evolved_opinion_t *evo_opinions = NULL;
                                size_t evo_count = 0;
                                if (hu_evolved_opinions_get(alloc, db, 0.4, 5, &evo_opinions,
                                                            &evo_count) == HU_OK &&
                                    evo_opinions && evo_count > 0) {
                                    size_t op_dir_len = 0;
                                    char *op_dir = hu_evolved_opinion_build_directive(
                                        alloc, evo_opinions, evo_count, 0.4, &op_dir_len);
                                    if (op_dir && op_dir_len > 0)
                                        PHASE6_APPEND(op_dir, op_dir_len);
                                    else if (op_dir)
                                        alloc->free(alloc->ctx, op_dir, op_dir_len + 1);
                                    hu_evolved_opinions_free(alloc, evo_opinions, evo_count);
                                }
                            }

                            /* 11. Episodic context — last 5 episodes for this contact */
                            {
                                hu_episode_sqlite_t *episodes = NULL;
                                size_t ep_count = 0;
                                if (hu_episode_get_by_contact(alloc, db, batch_key, key_len, 5, 0,
                                                              &episodes, &ep_count) == HU_OK &&
                                    episodes && ep_count > 0) {
                                    char ep_buf[4096];
                                    size_t ep_pos = 0;
                                    static const char ep_hdr[] =
                                        "[SHARED HISTORY with this person — reference specific "
                                        "details when relevant, not generic empathy: ";
                                    int n = snprintf(ep_buf, sizeof(ep_buf), "%s", ep_hdr);
                                    if (n > 0 && (size_t)n < sizeof(ep_buf))
                                        ep_pos = (size_t)n;
                                    for (size_t ei = 0;
                                         ei < ep_count && ep_pos < sizeof(ep_buf) - 64; ei++) {
                                        if (ei > 0) {
                                            ep_buf[ep_pos++] = ' ';
                                            ep_buf[ep_pos++] = '|';
                                            ep_buf[ep_pos++] = ' ';
                                        }
                                        size_t add = episodes[ei].summary_len;
                                        if (add > 250)
                                            add = 250;
                                        if (ep_pos + add + 2 < sizeof(ep_buf)) {
                                            memcpy(ep_buf + ep_pos, episodes[ei].summary, add);
                                            ep_pos += add;
                                        }
                                    }
                                    if (ep_pos + 2 < sizeof(ep_buf)) {
                                        ep_buf[ep_pos++] = ']';
                                        ep_buf[ep_pos] = '\0';
                                        char *ep_str = (char *)alloc->alloc(alloc->ctx, ep_pos + 1);
                                        if (ep_str) {
                                            memcpy(ep_str, ep_buf, ep_pos + 1);
                                            PHASE6_APPEND(ep_str, ep_pos);
                                        }
                                    }
                                    /* P7: Reinforce referenced episodes */
                                    for (size_t ei = 0; ei < ep_count; ei++)
                                        (void)hu_episode_reinforce(db, episodes[ei].id,
                                                                   (int64_t)time(NULL));
                                    hu_episode_free(alloc, episodes, ep_count);
                                }
                            }
                        }
                    }
#endif

                    /* F148 (Pillar 29): On-device message classification */
                    if (batch_key && key_len > 0) {
                        double cls_conf = 0.0;
                        hu_classify_result_t cls =
                            hu_classify_message(batch_key, key_len, &cls_conf);
                        if (cls_conf > 0.5) {
                            char *cls_dir = NULL;
                            size_t cls_len = 0;
                            if (hu_classifier_build_prompt(alloc, cls, cls_conf, &cls_dir,
                                                           &cls_len) == HU_OK &&
                                cls_dir && cls_len > 0)
                                PHASE6_APPEND(cls_dir, cls_len);
                            else if (cls_dir)
                                alloc->free(alloc->ctx, cls_dir, cls_len + 1);
                        }
                    }

                    /* F125-F127 (Pillar 20): Contact knowledge state */
#ifdef HU_ENABLE_SQLITE
                    if (agent->memory && batch_key && key_len > 0) {
                        sqlite3 *kdb = hu_sqlite_memory_get_db(agent->memory);
                        if (kdb) {
                            char ksql[512];
                            size_t ksql_len = 0;
                            if (hu_knowledge_query_sql(batch_key, key_len, 0.5, ksql, sizeof(ksql),
                                                       &ksql_len) == HU_OK) {
                                sqlite3_stmt *kstmt = NULL;
                                if (sqlite3_prepare_v2(kdb, ksql, (int)ksql_len, &kstmt, NULL) ==
                                    SQLITE_OK) {
                                    hu_knowledge_entry_t kentries[8];
                                    size_t kcount = 0;
                                    while (sqlite3_step(kstmt) == SQLITE_ROW && kcount < 8) {
                                        hu_knowledge_entry_t *ke = &kentries[kcount];
                                        memset(ke, 0, sizeof(*ke));
                                        const char *kt =
                                            (const char *)sqlite3_column_text(kstmt, 0);
                                        if (kt) {
                                            ke->topic_len = (size_t)sqlite3_column_bytes(kstmt, 0);
                                            ke->topic = hu_strndup(alloc, kt, ke->topic_len);
                                        }
                                        ke->confidence = sqlite3_column_double(kstmt, 1);
                                        kcount++;
                                    }
                                    sqlite3_finalize(kstmt);
                                    if (kcount > 0) {
                                        hu_knowledge_summary_t ksummary = {0};
                                        if (hu_knowledge_build_summary(alloc, kentries, kcount,
                                                                       batch_key, key_len, NULL, 0,
                                                                       &ksummary) == HU_OK) {
                                            char *kprompt = NULL;
                                            size_t kprompt_len = 0;
                                            if (hu_knowledge_summary_to_prompt(
                                                    alloc, &ksummary, &kprompt, &kprompt_len) ==
                                                    HU_OK &&
                                                kprompt && kprompt_len > 0)
                                                PHASE6_APPEND(kprompt, kprompt_len);
                                            else if (kprompt)
                                                alloc->free(alloc->ctx, kprompt, kprompt_len + 1);
                                            hu_knowledge_summary_deinit(alloc, &ksummary);
                                        }
                                        for (size_t ki = 0; ki < kcount; ki++)
                                            hu_knowledge_entry_deinit(alloc, &kentries[ki]);
                                    }
                                }
                            }
                        }
                    }
#endif

                    /* F135-F137 (Pillar 24): Shared experience compression */
#ifdef HU_ENABLE_SQLITE
                    if (agent->memory && batch_key && key_len > 0) {
                        sqlite3 *cdb = hu_sqlite_memory_get_db(agent->memory);
                        if (cdb) {
                            char csql[512];
                            size_t csql_len = 0;
                            if (hu_compression_query_sql(batch_key, key_len, csql, sizeof(csql),
                                                         &csql_len) == HU_OK) {
                                sqlite3_stmt *cstmt = NULL;
                                if (sqlite3_prepare_v2(cdb, csql, (int)csql_len, &cstmt, NULL) ==
                                    SQLITE_OK) {
                                    hu_shared_ref_t crefs[8];
                                    size_t ccount = 0;
                                    while (sqlite3_step(cstmt) == SQLITE_ROW && ccount < 8) {
                                        hu_shared_ref_t *cr = &crefs[ccount];
                                        memset(cr, 0, sizeof(*cr));
                                        const char *ck =
                                            (const char *)sqlite3_column_text(cstmt, 0);
                                        if (ck) {
                                            cr->compressed_form_len =
                                                (size_t)sqlite3_column_bytes(cstmt, 0);
                                            cr->compressed_form =
                                                hu_strndup(alloc, ck, cr->compressed_form_len);
                                        }
                                        cr->strength = sqlite3_column_double(cstmt, 1);
                                        ccount++;
                                    }
                                    sqlite3_finalize(cstmt);
                                    if (ccount > 0) {
                                        char *cprompt = NULL;
                                        size_t cprompt_len = 0;
                                        if (hu_compression_build_prompt(alloc, crefs, ccount,
                                                                        &cprompt,
                                                                        &cprompt_len) == HU_OK &&
                                            cprompt && cprompt_len > 0)
                                            PHASE6_APPEND(cprompt, cprompt_len);
                                        else if (cprompt)
                                            alloc->free(alloc->ctx, cprompt, cprompt_len + 1);
                                        for (size_t ci = 0; ci < ccount; ci++)
                                            hu_shared_ref_deinit(alloc, &crefs[ci]);
                                    }
                                }
                            }
                        }
                    }
#endif

                    /* Phase 8 (F96): Skill trigger matching */
#ifdef HU_HAS_SKILLS
                    if (agent->memory && batch_key && key_len > 0) {
                        sqlite3 *skill_db = hu_sqlite_memory_get_db(agent->memory);
                        if (skill_db) {
                            hu_skill_t *matched = NULL;
                            size_t matched_count = 0;
                            hu_error_t skill_err =
                                hu_skill_match_triggers(alloc, skill_db, batch_key, key_len, NULL,
                                                        0, NULL, 0, 0.5, &matched, &matched_count);
                            if (skill_err == HU_OK && matched && matched_count > 0) {
                                size_t max_skills = matched_count > 3 ? 3 : matched_count;
                                for (size_t si = 0; si < max_skills; si++) {
                                    /* Inject skill strategy as directive */
                                    size_t strat_len = matched[si].strategy_len > 0
                                                           ? matched[si].strategy_len
                                                           : strlen(matched[si].strategy);
                                    if (strat_len > 0 && strat_len < 500) {
                                        hu_log_info("human", agent ? agent->observer : NULL,
                                                    "skill: %s", matched[si].name);
                                        char skill_buf[512];
                                        int sb = snprintf(skill_buf, sizeof(skill_buf),
                                                          "[SKILL %s]: %.*s", matched[si].name,
                                                          (int)strat_len, matched[si].strategy);
                                        if (sb > 0 && (size_t)sb < sizeof(skill_buf)) {
                                            char *skill_str =
                                                (char *)alloc->alloc(alloc->ctx, (size_t)sb + 1);
                                            if (skill_str) {
                                                memcpy(skill_str, skill_buf, (size_t)sb + 1);
                                                PHASE6_APPEND(skill_str, (size_t)sb);
                                            }
                                        }
                                    }
                                    /* Record attempt and update success rate */
                                    int64_t attempt_id = 0;
                                    hu_skill_record_attempt(skill_db, matched[si].id, batch_key,
                                                            key_len, (int64_t)time(NULL), NULL, 0,
                                                            NULL, 0, NULL, 0, &attempt_id);
                                    (void)hu_skill_update_success_rate(skill_db, matched[si].id, 1,
                                                                       0);
                                    if (agent->bth_metrics)
                                        agent->bth_metrics->skills_applied++;
                                }
                                hu_skill_free(alloc, matched, matched_count);
                            }
                        }
                    }
#endif

                    /* Phase 9 (F102-F115): Authentic existence context injection */
                    {
                        time_t t_p9 = time(NULL);
#ifdef HU_ENABLE_AUTHENTIC
                        /* F102: Cognitive load */
                        hu_cognitive_load_config_t cog_cfg = {.peak_hour_start = 9,
                                                              .peak_hour_end = 12,
                                                              .low_hour_start = 22,
                                                              .low_hour_end = 6,
                                                              .fatigue_threshold = 12,
                                                              .monday_penalty = 0.15f,
                                                              .friday_bonus = 0.1f};
                        hu_cognitive_load_state_t cog =
                            hu_cognitive_load_calculate(&cog_cfg, 0, t_p9);
                        const char *cog_hint = hu_cognitive_load_prompt_hint(&cog);
#else
                        const char *cog_hint = NULL;
#endif

                        /* F104: Physical state */
                        hu_physical_config_t phys_cfg = {.exercises = true,
                                                         .exercise_days = {1, 3, 5},
                                                         .exercise_day_count = 3,
                                                         .coffee_drinker = true,
                                                         .mentions_frequency = 0.3f};
                        hu_physical_state_t phys = hu_physical_state_from_schedule(&phys_cfg, t_p9);
                        const char *phys_hint = hu_physical_state_prompt_hint(phys);

                        if (cog_hint) {
#ifdef HU_ENABLE_AUTHENTIC
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "Phase 9: cognitive hint: capacity=%.2f", cog.capacity);
#endif
                            size_t ch_len = strlen(cog_hint);
                            char *ch_copy = (char *)alloc->alloc(alloc->ctx, ch_len + 1);
                            if (ch_copy) {
                                memcpy(ch_copy, cog_hint, ch_len + 1);
                                PHASE6_APPEND(ch_copy, ch_len);
                            }
                        }
                        if (phys_hint) {
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "Phase 9: physical state: %s",
                                        hu_physical_state_name(phys));
                            size_t ph_len = strlen(phys_hint);
                            char *ph_copy = (char *)alloc->alloc(alloc->ctx, ph_len + 1);
                            if (ph_copy) {
                                memcpy(ph_copy, phys_hint, ph_len + 1);
                                PHASE6_APPEND(ph_copy, ph_len);
                            }
                        }

                        /* F105: Error injection (3% chance) */
                        static uint32_t error_seed = 0;
                        if (hu_should_inject_error(0.03f, error_seed++)) {
                            const char *err_p = hu_error_injection_prompt();
                            if (err_p) {
                                size_t el = strlen(err_p);
                                char *ec = (char *)alloc->alloc(alloc->ctx, el + 1);
                                if (ec) {
                                    memcpy(ec, err_p, el + 1);
                                    PHASE6_APPEND(ec, el);
                                }
                            }
                            hu_log_error("human", agent ? agent->observer : NULL,
                                         "Phase 9: error injection active");
                        }

                        /* F106: Mundane complaint */
                        struct tm tm_p9;
                        struct tm *lt_p9 = hu_platform_localtime_r(&t_p9, &tm_p9);
                        if (lt_p9) {
                            const char *complaint = hu_mundane_complaint_prompt(
                                lt_p9->tm_hour, lt_p9->tm_wday, phys, NULL);
                            if (complaint) {
                                size_t cl = strlen(complaint);
                                char *cc = (char *)alloc->alloc(alloc->ctx, cl + 1);
                                if (cc) {
                                    memcpy(cc, complaint, cl + 1);
                                    PHASE6_APPEND(cc, cl);
                                }
                            }
                        }
                    }

                    /* 9. Authentic existence (F103-F115) */
                    {
                        uint32_t auth_seed =
                            (uint32_t)((uint64_t)time(NULL) ^ (uintptr_t)batch_key);
                        hu_authentic_config_t auth_cfg = {
                            .narration_probability = 0.10,
                            .embodiment_probability = 0.08,
                            .imperfection_probability = 0.05,
                            .complaining_probability = 0.07,
                            .gossip_probability = 0.04,
                            .random_thought_probability = 0.06,
                            .medium_awareness_probability = 0.05,
                            .resistance_probability = 0.03,
                            .existential_probability = 0.02,
                            .contradiction_probability = 0.03,
                            .guilt_probability = 0.04,
                            .life_thread_probability = 0.05,
                            .bad_day_active = false,
                            .bad_day_duration_hours = 8,
                        };
                        hu_authentic_behavior_t behavior =
                            hu_authentic_select(&auth_cfg, 0.5, false, auth_seed);
                        if (behavior != HU_AUTH_NONE) {
                            size_t auth_len = 0;
                            char *auth_dir = NULL;
                            hu_authentic_build_directive(alloc, behavior, NULL, 0, &auth_dir,
                                                         &auth_len);
                            if (auth_dir && auth_len > 0)
                                PHASE6_APPEND(auth_dir, auth_len);
                            else if (auth_dir)
                                alloc->free(alloc->ctx, auth_dir, auth_len + 1);
                        }
                    }

                    /* 10. Cognitive load (F102) */
                    {
                        double load = hu_cognitive_compute_load(
                            ctx_count > 0 ? (uint32_t)ctx_count : 1, 1, false);
                        if (load > 0.5) {
                            hu_cognitive_state_t cog_state = {
                                .active_conversations = ctx_count > 0 ? (uint32_t)ctx_count : 1,
                                .messages_this_hour = 1,
                                .complex_topic_active = false,
                                .load_score = load,
                            };
                            size_t cog_len = 0;
                            char *cog_dir = NULL;
                            hu_cognitive_build_directive(alloc, &cog_state, &cog_dir, &cog_len);
                            if (cog_dir && cog_len > 0)
                                PHASE6_APPEND(cog_dir, cog_len);
                            else if (cog_dir)
                                alloc->free(alloc->ctx, cog_dir, cog_len + 1);
                        }
                    }

                    /* 11. Relationship dynamics velocity (F138-F140) */
                    {
                        hu_rel_velocity_t rel_vel = {0};
                        rel_vel.contact_id = batch_key;
                        rel_vel.contact_id_len = key_len;
                        hu_rel_velocity_compute(&rel_vel);
                        if (rel_vel.velocity > 0.0 || rel_vel.velocity < 0.0) {
                            hu_drift_signal_t drift = {0};
                            drift.contact_id = batch_key;
                            drift.contact_id_len = key_len;
                            drift.current_velocity = rel_vel.velocity;
                            hu_repair_state_t repair = {0};
                            char *rel_dir = NULL;
                            size_t rel_len = 0;
                            hu_rel_dynamics_build_prompt(alloc, &rel_vel, &drift, &repair, &rel_dir,
                                                         &rel_len);
                            if (rel_dir && rel_len > 0)
                                PHASE6_APPEND(rel_dir, rel_len);
                            else if (rel_dir)
                                alloc->free(alloc->ctx, rel_dir, rel_len + 1);
                        }
                    }

                    /* F65: Opinion evolution — inject current opinions for topic relevance */
#ifdef HU_ENABLE_SQLITE
                    if (agent->memory && batch_key && key_len > 0) {
                        sqlite3 *op_db = hu_sqlite_memory_get_db(agent->memory);
                        if (op_db) {
                            char op_sql[512];
                            size_t op_sql_len = 0;
                            if (hu_opinions_query_current_sql(batch_key, key_len, op_sql,
                                                              sizeof(op_sql),
                                                              &op_sql_len) == HU_OK) {
                                sqlite3_stmt *op_stmt = NULL;
                                if (sqlite3_prepare_v2(op_db, op_sql, (int)op_sql_len, &op_stmt,
                                                       NULL) == SQLITE_OK) {
                                    hu_opinion_t ops[8];
                                    size_t op_count = 0;
                                    while (sqlite3_step(op_stmt) == SQLITE_ROW && op_count < 8) {
                                        hu_opinion_t *o = &ops[op_count];
                                        memset(o, 0, sizeof(*o));
                                        const char *t =
                                            (const char *)sqlite3_column_text(op_stmt, 1);
                                        const char *p =
                                            (const char *)sqlite3_column_text(op_stmt, 2);
                                        if (t) {
                                            o->topic_len = (size_t)sqlite3_column_bytes(op_stmt, 1);
                                            o->topic = hu_strndup(alloc, t, o->topic_len);
                                        }
                                        if (p) {
                                            o->position_len =
                                                (size_t)sqlite3_column_bytes(op_stmt, 2);
                                            o->position = hu_strndup(alloc, p, o->position_len);
                                        }
                                        o->confidence = sqlite3_column_double(op_stmt, 3);
                                        op_count++;
                                    }
                                    sqlite3_finalize(op_stmt);
                                    if (op_count > 0) {
                                        char *op_prompt = NULL;
                                        size_t op_prompt_len = 0;
                                        if (hu_opinions_build_prompt(alloc, ops, op_count,
                                                                     &op_prompt,
                                                                     &op_prompt_len) == HU_OK &&
                                            op_prompt && op_prompt_len > 0)
                                            PHASE6_APPEND(op_prompt, op_prompt_len);
                                        else if (op_prompt)
                                            alloc->free(alloc->ctx, op_prompt, op_prompt_len + 1);
                                        for (size_t oi = 0; oi < op_count; oi++)
                                            hu_opinion_deinit(alloc, &ops[oi]);
                                    }
                                }
                            }
                        }
                    }
#endif

                    /* F83-F93: Feed context — inject recent relevant feed items */
#ifdef HU_ENABLE_SQLITE
                    if (agent->memory && batch_key && key_len > 0) {
                        hu_feed_item_stored_t *fitems = NULL;
                        size_t fcount = 0;
                        sqlite3 *fdb = hu_sqlite_memory_get_db(agent->memory);
                        if (fdb &&
                            hu_feed_processor_get_for_contact(alloc, fdb, batch_key, key_len, 5,
                                                              &fitems, &fcount) == HU_OK &&
                            fitems && fcount > 0) {
                            hu_feed_item_t *conv_items = (hu_feed_item_t *)alloc->alloc(
                                alloc->ctx, fcount * sizeof(hu_feed_item_t));
                            if (conv_items) {
                                memset(conv_items, 0, fcount * sizeof(hu_feed_item_t));
                                for (size_t fi = 0; fi < fcount; fi++) {
                                    conv_items[fi].content = fitems[fi].content;
                                    conv_items[fi].content_len = fitems[fi].content_len;
                                    conv_items[fi].source = fitems[fi].source;
                                    conv_items[fi].source_len = strlen(fitems[fi].source);
                                }
                                char *feed_prompt = NULL;
                                size_t feed_prompt_len = 0;
                                if (hu_feeds_build_prompt(alloc, conv_items, fcount, &feed_prompt,
                                                          &feed_prompt_len) == HU_OK &&
                                    feed_prompt && feed_prompt_len > 0)
                                    PHASE6_APPEND(feed_prompt, feed_prompt_len);
                                else if (feed_prompt)
                                    alloc->free(alloc->ctx, feed_prompt, feed_prompt_len + 1);
                                alloc->free(alloc->ctx, conv_items,
                                            fcount * sizeof(hu_feed_item_t));
                            }
                            hu_feed_items_free(alloc, fitems, fcount);
                        }
                    }
#endif

                    /* F116-F120: Visual content pipeline — check for shareable visual content */
#ifdef HU_ENABLE_SQLITE
                    if (agent->memory && batch_key && key_len > 0) {
                        sqlite3 *vdb = hu_sqlite_memory_get_db(agent->memory);
                        if (vdb) {
                            hu_visual_entry_t *ventries = NULL;
                            size_t vcount = 0;
                            if (hu_visual_match_for_contact(alloc, vdb, batch_key, key_len,
                                                            combined, combined_len, &ventries,
                                                            &vcount) == HU_OK &&
                                ventries && vcount > 0) {
                                bool should_share = false;
                                double vconf = 0.0;
                                hu_visual_should_share(&ventries[0], combined, combined_len,
                                                       &should_share, &vconf);
                                if (should_share) {
                                    hu_visual_candidate_t vc = {0};
                                    vc.path = ventries[0].path;
                                    vc.path_len = strlen(ventries[0].path);
                                    vc.description = ventries[0].description;
                                    vc.description_len = strlen(ventries[0].description);
                                    vc.relevance_score = vconf;
                                    char *vprompt = NULL;
                                    size_t vprompt_len = 0;
                                    if (hu_visual_build_prompt(alloc, &vc, 1, &vprompt,
                                                               &vprompt_len) == HU_OK &&
                                        vprompt && vprompt_len > 0)
                                        PHASE6_APPEND(vprompt, vprompt_len);
                                    else if (vprompt)
                                        alloc->free(alloc->ctx, vprompt, vprompt_len + 1);
                                }
                                hu_visual_entries_free(alloc, ventries, vcount);
                            }
                        }
                    }
#endif

                    /* F47: Content forwarding — check for shareable content from other sources */
#ifdef HU_ENABLE_SQLITE
                    if (agent->memory && batch_key && key_len > 0) {
                        sqlite3 *fwd_db = hu_sqlite_memory_get_db(agent->memory);
                        if (fwd_db) {
                            char fwd_sql[512];
                            size_t fwd_sql_len = 0;
                            if (hu_forwarding_query_for_contact_sql(batch_key, key_len, fwd_sql,
                                                                    sizeof(fwd_sql),
                                                                    &fwd_sql_len) == HU_OK) {
                                sqlite3_stmt *fwd_stmt = NULL;
                                if (sqlite3_prepare_v2(fwd_db, fwd_sql, (int)fwd_sql_len, &fwd_stmt,
                                                       NULL) == SQLITE_OK) {
                                    if (sqlite3_step(fwd_stmt) == SQLITE_ROW) {
                                        const char *fc =
                                            (const char *)sqlite3_column_text(fwd_stmt, 1);
                                        const char *fs =
                                            (const char *)sqlite3_column_text(fwd_stmt, 2);
                                        if (fc && fs) {
                                            size_t fc_len =
                                                (size_t)sqlite3_column_bytes(fwd_stmt, 1);
                                            char fwd_buf[512];
                                            int fw = snprintf(
                                                fwd_buf, sizeof(fwd_buf),
                                                "[SHAREABLE CONTENT from %s]: %.*s — "
                                                "Share this naturally if relevant.",
                                                fs, (int)(fc_len > 300 ? 300 : fc_len), fc);
                                            if (fw > 0 && (size_t)fw < sizeof(fwd_buf)) {
                                                char *fwd_str = (char *)alloc->alloc(
                                                    alloc->ctx, (size_t)fw + 1);
                                                if (fwd_str) {
                                                    memcpy(fwd_str, fwd_buf, (size_t)fw + 1);
                                                    PHASE6_APPEND(fwd_str, (size_t)fw);
                                                }
                                            }
                                        }
                                    }
                                    sqlite3_finalize(fwd_stmt);
                                }
                            }
                        }
                    }
#endif

                    /* F52: Sports/current events — inject relevant events matching persona
                     * interests */
#if defined(HU_ENABLE_SQLITE)
                    if (agent->memory && agent->persona) {
                        sqlite3 *ev_db = hu_sqlite_memory_get_db(agent->memory);
                        if (ev_db && agent->persona->context_awareness.news_topics_count > 0) {
                            for (size_t ni = 0;
                                 ni < agent->persona->context_awareness.news_topics_count && ni < 3;
                                 ni++) {
                                const char *topic =
                                    agent->persona->context_awareness.news_topics[ni];
                                if (!topic || !topic[0])
                                    continue;
                                size_t tlen = strlen(topic);
                                char ev_sql[512];
                                size_t ev_sql_len = 0;
                                if (hu_events_create_table_sql(ev_sql, sizeof(ev_sql),
                                                               &ev_sql_len) != HU_OK)
                                    break;
                                /* Query events by topic */
                                char eq_sql[512];
                                size_t eq_len = 0;
                                int qn =
                                    snprintf(eq_sql, sizeof(eq_sql),
                                             "SELECT topic, summary, source FROM current_events "
                                             "WHERE topic LIKE '%%%.*s%%' ORDER BY published_at "
                                             "DESC LIMIT 3",
                                             (int)tlen, topic);
                                if (qn <= 0 || (size_t)qn >= sizeof(eq_sql))
                                    continue;
                                eq_len = (size_t)qn;
                                sqlite3_stmt *ev_stmt = NULL;
                                if (sqlite3_prepare_v2(ev_db, eq_sql, (int)eq_len, &ev_stmt,
                                                       NULL) == SQLITE_OK) {
                                    if (sqlite3_step(ev_stmt) == SQLITE_ROW) {
                                        const char *summary =
                                            (const char *)sqlite3_column_text(ev_stmt, 1);
                                        if (summary) {
                                            size_t slen = (size_t)sqlite3_column_bytes(ev_stmt, 1);
                                            char ev_buf[384];
                                            int ew = snprintf(
                                                ev_buf, sizeof(ev_buf),
                                                "[CURRENT EVENT — %.*s]: %.*s", (int)tlen, topic,
                                                (int)(slen > 250 ? 250 : slen), summary);
                                            if (ew > 0 && (size_t)ew < sizeof(ev_buf)) {
                                                char *ev_str = (char *)alloc->alloc(alloc->ctx,
                                                                                    (size_t)ew + 1);
                                                if (ev_str) {
                                                    memcpy(ev_str, ev_buf, (size_t)ew + 1);
                                                    PHASE6_APPEND(ev_str, (size_t)ew);
                                                }
                                            }
                                        }
                                    }
                                    sqlite3_finalize(ev_stmt);
                                }
                            }
                        }
                    }
#endif

                    /* F134-F137: Context arbitration — trim phase6 prefix to token budget */
                    if (phase6_prefix && phase6_len > 0) {
                        size_t est_tokens = hu_directive_estimate_tokens(phase6_prefix, phase6_len);
                        const size_t max_tokens = 1500;
                        if (est_tokens > max_tokens) {
                            size_t target_chars = max_tokens * 4;
                            if (target_chars < phase6_len) {
                                phase6_prefix[target_chars] = '\0';
                                phase6_len = target_chars;
                                hu_log_info("human", agent ? agent->observer : NULL,
                                            "arbitrator: trimmed phase6 directives "
                                            "from %zu to %zu tokens",
                                            est_tokens, max_tokens);
                            }
                        }
                    }

#undef PHASE6_APPEND
                }

                /* 3. Build awareness context from history via shared analyzer.
                 * Skip in llm_decides: director + persona are sufficient. */
                if (ctx_entries && ctx_count > 0 && !llm_decides) {
                    char *awareness_ctx = hu_conversation_build_awareness(
                        alloc, ctx_entries, ctx_count,
                        (agent && agent->persona) ? agent->persona : NULL, &convo_ctx_len);

                    /* Prepend Phase 6 prefix (1–8) before awareness (9) */
                    if (phase6_prefix && phase6_len > 0) {
                        if (awareness_ctx && convo_ctx_len > 0) {
                            size_t total = phase6_len + convo_ctx_len + 2;
                            char *merged = (char *)alloc->alloc(alloc->ctx, total + 1);
                            if (merged) {
                                memcpy(merged, phase6_prefix, phase6_len);
                                merged[phase6_len] = '\n';
                                merged[phase6_len + 1] = '\n';
                                memcpy(merged + phase6_len + 2, awareness_ctx, convo_ctx_len);
                                merged[total] = '\0';
                                alloc->free(alloc->ctx, awareness_ctx, convo_ctx_len + 1);
                                convo_ctx = merged;
                                convo_ctx_len = total;
                            } else {
                                convo_ctx = awareness_ctx;
                            }
                        } else {
                            convo_ctx = phase6_prefix;
                            convo_ctx_len = phase6_len;
                            awareness_ctx = NULL;
                            phase6_prefix = NULL; /* ownership transferred to convo_ctx */
                        }
                        if (phase6_prefix) {
                            alloc->free(alloc->ctx, phase6_prefix, phase6_len + 1);
                            phase6_prefix = NULL;
                        }
                    } else {
                        convo_ctx = awareness_ctx;
                    }
                    if (phase6_prefix)
                        alloc->free(alloc->ctx, phase6_prefix, phase6_len + 1);

                    /* F21: Avoidance pattern detection — topic change within same session */
                    static hu_consolidation_debounce_t topic_consolidation_debounce;
                    static bool topic_debounce_initialized = false;
                    if (!topic_debounce_initialized) {
                        hu_consolidation_debounce_init(&topic_consolidation_debounce);
                        topic_debounce_initialized = true;
                    }
                    hu_consolidation_debounce_tick(&topic_consolidation_debounce);
                    if (agent->memory && ctx_count >= 2) {
                        char topic_before[64], topic_after[64];
                        if (hu_conversation_detect_topic_change(ctx_entries, ctx_count,
                                                                topic_before, sizeof(topic_before),
                                                                topic_after, sizeof(topic_after))) {
                            size_t tb_len = strlen(topic_before);
                            if (tb_len > 0)
                                (void)hu_superhuman_avoidance_record(
                                    agent->memory, batch_key, key_len, topic_before, tb_len, true);

                            hu_consolidation_set_topic_switch(true);

                            /* Topic-switch consolidation (EdgeClaw-inspired):
                             * trigger memory consolidation on topic change
                             * with debounce to avoid excessive consolidation. */
                            {
                                int64_t tc_now = (int64_t)time(NULL);
                                if (hu_consolidation_should_run(&topic_consolidation_debounce,
                                                                tc_now)) {
                                    hu_consolidation_config_t tc_cfg = {
                                        .decay_days = config ? config->behavior.decay_days : 30,
                                        .decay_factor = 0.5,
                                        .dedup_threshold =
                                            config ? config->behavior.dedup_threshold : 0,
                                        .max_entries = 5000,
                                        .provider = &agent->provider,
                                        .model = agent->model_name,
                                        .model_len = agent->model_name_len,
                                    };
                                    if (hu_memory_consolidate(alloc, agent->memory, &tc_cfg) ==
                                        HU_OK) {
                                        hu_consolidation_debounce_reset(
                                            &topic_consolidation_debounce, tc_now);
                                        hu_log_info("human", agent ? agent->observer : NULL,
                                                    "topic-switch consolidation: '%s' -> '%s'",
                                                    topic_before, topic_after);
                                    }
                                }
                            }
                        }
                    }
                }

                /* 2c. Length calibration fallback for channels without history.
                 * When history exists, calibration runs inside build_awareness.
                 * When it doesn't, we still want message-type guidance. */
                if (!convo_ctx && combined_len > 0) {
                    char cal_buf[1024];
                    const hu_contact_profile_t *cp_cal =
                        (agent->persona && batch_key && key_len > 0)
                            ? hu_persona_find_contact(agent->persona, batch_key, key_len)
                            : NULL;
                    size_t cal_len = hu_conversation_calibrate_length_for_contact(
                        combined, combined_len, NULL, 0, msgs[batch_start].is_group, cp_cal,
                        agent->relationship.stage, cal_buf, sizeof(cal_buf));
                    if (cal_len > 0) {
                        convo_ctx = (char *)alloc->alloc(alloc->ctx, cal_len + 1);
                        if (convo_ctx) {
                            memcpy(convo_ctx, cal_buf, cal_len);
                            convo_ctx[cal_len] = '\0';
                            convo_ctx_len = cal_len;
                        }
                    }
                }

#if defined(HU_ENABLE_SQLITE) && !defined(HU_IS_TEST)
                /* Prepend cross-channel snippets before other conversation context for the LLM. */
                if (cross_channel_ctx && cross_channel_ctx_len > 0) {
                    if (convo_ctx && convo_ctx_len > 0) {
                        size_t total = cross_channel_ctx_len + convo_ctx_len + 2;
                        char *merged = (char *)alloc->alloc(alloc->ctx, total + 1);
                        if (merged) {
                            memcpy(merged, cross_channel_ctx, cross_channel_ctx_len);
                            merged[cross_channel_ctx_len] = '\n';
                            merged[cross_channel_ctx_len + 1] = '\n';
                            memcpy(merged + cross_channel_ctx_len + 2, convo_ctx, convo_ctx_len);
                            merged[total] = '\0';
                            alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                            alloc->free(alloc->ctx, cross_channel_ctx, cross_channel_ctx_len + 1);
                            convo_ctx = merged;
                            convo_ctx_len = total;
                            cross_channel_ctx = NULL;
                            cross_channel_ctx_len = 0;
                        }
                    } else {
                        convo_ctx = cross_channel_ctx;
                        convo_ctx_len = cross_channel_ctx_len;
                        cross_channel_ctx = NULL;
                        cross_channel_ctx_len = 0;
                    }
                }
#endif

                /* 2d. Style analysis: analyze their texting patterns for mirroring */
                char *style_ctx = NULL;
                size_t style_ctx_len = 0;
                if (history_entries && history_count > 0 && !llm_decides) {
                    style_ctx = hu_conversation_analyze_style(
                        alloc, history_entries, history_count,
                        (agent && agent->persona) ? agent->persona : NULL, &style_ctx_len);
                }

                /* F32: Style fingerprint — our texting style with this contact (haha vs lol, etc.)
                 * Skip in llm_decides: persona already covers voice. */
                if (agent->memory && batch_key && key_len > 0 && !llm_decides) {
                    hu_style_fingerprint_t fp;
                    memset(&fp, 0, sizeof(fp));
                    if (hu_style_fingerprint_get(agent->memory, alloc, batch_key, key_len, &fp) ==
                        HU_OK) {
                        char fp_buf[256];
                        size_t fp_len =
                            hu_style_fingerprint_build_directive(&fp, fp_buf, sizeof(fp_buf));
                        if (fp_len > 0) {
                            if (style_ctx && style_ctx_len > 0) {
                                size_t merged_len = style_ctx_len + fp_len + 2;
                                char *merged = (char *)alloc->alloc(alloc->ctx, merged_len + 1);
                                if (merged) {
                                    memcpy(merged, style_ctx, style_ctx_len);
                                    merged[style_ctx_len] = '\n';
                                    memcpy(merged + style_ctx_len + 1, fp_buf, fp_len);
                                    merged[merged_len - 1] = '\n';
                                    merged[merged_len] = '\0';
                                    alloc->free(alloc->ctx, style_ctx, style_ctx_len + 1);
                                    style_ctx = merged;
                                    style_ctx_len = merged_len;
                                }
                            } else {
                                style_ctx = (char *)alloc->alloc(alloc->ctx, fp_len + 2);
                                if (style_ctx) {
                                    memcpy(style_ctx, fp_buf, fp_len);
                                    style_ctx[fp_len] = '\n';
                                    style_ctx[fp_len + 1] = '\0';
                                    style_ctx_len = fp_len + 1;
                                }
                            }
                        }
                    }
                }

                /* F32b: Rich style clone — detailed texting patterns from chat history */
                if (history_entries && history_count > 10 && !llm_decides) {
                    const char *own_msgs[512];
                    size_t own_count = 0;
                    for (size_t hi = 0; hi < history_count && own_count < 512; hi++) {
                        if (history_entries[hi].from_me && history_entries[hi].text[0] != '\0')
                            own_msgs[own_count++] = history_entries[hi].text;
                    }
                    if (own_count >= 10) {
                        char *clone_prompt = NULL;
                        size_t clone_len = 0;
                        if (hu_style_clone_from_history(alloc, own_msgs, own_count, &clone_prompt,
                                                        &clone_len) == HU_OK &&
                            clone_prompt && clone_len > 0) {
                            if (style_ctx && style_ctx_len > 0) {
                                size_t merged_len = style_ctx_len + clone_len + 2;
                                char *merged = (char *)alloc->alloc(alloc->ctx, merged_len + 1);
                                if (merged) {
                                    memcpy(merged, style_ctx, style_ctx_len);
                                    merged[style_ctx_len] = '\n';
                                    memcpy(merged + style_ctx_len + 1, clone_prompt, clone_len);
                                    merged[merged_len - 1] = '\n';
                                    merged[merged_len] = '\0';
                                    alloc->free(alloc->ctx, style_ctx, style_ctx_len + 1);
                                    style_ctx = merged;
                                    style_ctx_len = merged_len;
                                }
                            } else {
                                style_ctx = clone_prompt;
                                style_ctx_len = clone_len;
                                clone_prompt = NULL;
                            }
                            if (clone_prompt)
                                alloc->free(alloc->ctx, clone_prompt, clone_len + 1);
                        }
                    }
                }

                /* Merge style context into conversation context */
                if (style_ctx && convo_ctx) {
                    size_t merged_len = convo_ctx_len + style_ctx_len + 2;
                    char *merged = (char *)alloc->alloc(alloc->ctx, merged_len + 1);
                    if (merged) {
                        memcpy(merged, convo_ctx, convo_ctx_len);
                        merged[convo_ctx_len] = '\n';
                        memcpy(merged + convo_ctx_len + 1, style_ctx, style_ctx_len);
                        merged[merged_len - 1] = '\n';
                        merged[merged_len] = '\0';
                        alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                        convo_ctx = merged;
                        convo_ctx_len = merged_len;
                    }
                    alloc->free(alloc->ctx, style_ctx, style_ctx_len + 1);
                    style_ctx = NULL;
                } else if (style_ctx && !convo_ctx) {
                    convo_ctx = style_ctx;
                    convo_ctx_len = style_ctx_len;
                    style_ctx = NULL;
                }
                if (style_ctx)
                    alloc->free(alloc->ctx, style_ctx, style_ctx_len + 1);

                /* Tapback / reaction awareness via channel vtable.
                 * For groups, batch_key is still the sender handle, so
                 * per-handle SQL returns that person's reactions (partial but valid). */
#ifndef HU_IS_TEST
                {
                    if (ch->channel->vtable->build_reaction_context) {
                        char *tapback_ctx = NULL;
                        size_t tapback_len = 0;
                        if (ch->channel->vtable->build_reaction_context(
                                ch->channel->ctx, alloc, batch_key, key_len, &tapback_ctx,
                                &tapback_len) == HU_OK &&
                            tapback_ctx && tapback_len > 0) {
                            if (convo_ctx) {
                                size_t merged_len = convo_ctx_len + tapback_len + 2;
                                char *merged = (char *)alloc->alloc(alloc->ctx, merged_len + 1);
                                if (merged) {
                                    memcpy(merged, convo_ctx, convo_ctx_len);
                                    merged[convo_ctx_len] = '\n';
                                    memcpy(merged + convo_ctx_len + 1, tapback_ctx, tapback_len);
                                    merged[merged_len - 1] = '\n';
                                    merged[merged_len] = '\0';
                                    alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                    convo_ctx = merged;
                                    convo_ctx_len = merged_len;
                                }
                            } else {
                                convo_ctx = tapback_ctx;
                                convo_ctx_len = tapback_len;
                                tapback_ctx = NULL;
                            }
                            if (tapback_ctx)
                                alloc->free(alloc->ctx, tapback_ctx, tapback_len + 1);
                        }
                    }

                    /* Read receipt awareness via channel vtable. */
                    if (ch->channel->vtable->build_read_receipt_context) {
                        char *rr_ctx = NULL;
                        size_t rr_len = 0;
                        if (ch->channel->vtable->build_read_receipt_context(
                                ch->channel->ctx, alloc, batch_key, key_len, &rr_ctx, &rr_len) ==
                                HU_OK &&
                            rr_ctx && rr_len > 0) {
                            if (convo_ctx) {
                                size_t merged_len = convo_ctx_len + rr_len + 2;
                                char *merged = (char *)alloc->alloc(alloc->ctx, merged_len + 1);
                                if (merged) {
                                    memcpy(merged, convo_ctx, convo_ctx_len);
                                    merged[convo_ctx_len] = '\n';
                                    memcpy(merged + convo_ctx_len + 1, rr_ctx, rr_len);
                                    merged[merged_len - 1] = '\n';
                                    merged[merged_len] = '\0';
                                    alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                    convo_ctx = merged;
                                    convo_ctx_len = merged_len;
                                }
                            } else {
                                convo_ctx = rr_ctx;
                                convo_ctx_len = rr_len;
                                rr_ctx = NULL;
                            }
                            if (rr_ctx)
                                alloc->free(alloc->ctx, rr_ctx, rr_len + 1);
                        }
                    }
                }
#endif

                /* Narrative, engagement, emotion: inject when meaningful.
                 * Skip in llm_decides: avoids expensive emotion LLM calls. */
                if (history_entries && history_count > 0 && !llm_decides) {
                    hu_narrative_phase_t narr =
                        hu_conversation_detect_narrative(history_entries, history_count);
                    hu_engagement_level_t eng =
                        hu_conversation_detect_engagement(history_entries, history_count);
                    hu_emotional_state_t emo =
                        hu_daemon_detect_emotion(alloc, agent, history_entries, history_count);

                    bool narr_meaningful =
                        (narr != HU_NARRATIVE_OPENING && narr != HU_NARRATIVE_BUILDING);
                    bool eng_meaningful = (eng != HU_ENGAGEMENT_MODERATE);
                    bool emo_meaningful =
                        (emo.dominant_emotion && strcmp(emo.dominant_emotion, "neutral") != 0) ||
                        emo.concerning;

                    if (narr_meaningful || eng_meaningful || emo_meaningful) {
                        char insights_buf[512];
                        size_t insights_pos = 0;
                        int w = snprintf(insights_buf, sizeof(insights_buf),
                                         "\n--- Narrative/engagement/emotion ---\n");
                        if (w > 0 && (size_t)w < sizeof(insights_buf))
                            insights_pos = (size_t)w;

                        if (narr_meaningful) {
                            const char *phase_name = (narr == HU_NARRATIVE_CLOSING)   ? "closing"
                                                     : (narr == HU_NARRATIVE_PEAK)    ? "peak"
                                                     : (narr == HU_NARRATIVE_RELEASE) ? "release"
                                                     : (narr == HU_NARRATIVE_APPROACHING_CLIMAX)
                                                         ? "approaching climax"
                                                         : "building";
                            w = snprintf(insights_buf + insights_pos,
                                         sizeof(insights_buf) - insights_pos,
                                         "- Narrative phase: %s\n", phase_name);
                            if (w > 0 && (size_t)w < sizeof(insights_buf) - insights_pos)
                                insights_pos += (size_t)w;
                        }
                        if (eng_meaningful) {
                            const char *eng_name = (eng == HU_ENGAGEMENT_HIGH)  ? "high"
                                                   : (eng == HU_ENGAGEMENT_LOW) ? "low"
                                                   : (eng == HU_ENGAGEMENT_DISTRACTED)
                                                       ? "distracted"
                                                       : "moderate";
                            w = snprintf(insights_buf + insights_pos,
                                         sizeof(insights_buf) - insights_pos, "- Engagement: %s\n",
                                         eng_name);
                            if (w > 0 && (size_t)w < sizeof(insights_buf) - insights_pos)
                                insights_pos += (size_t)w;
                        }
                        if (emo_meaningful) {
                            if (emo.dominant_emotion &&
                                strcmp(emo.dominant_emotion, "neutral") != 0) {
                                w = snprintf(insights_buf + insights_pos,
                                             sizeof(insights_buf) - insights_pos,
                                             "- Emotional tone: %s%s\n", emo.dominant_emotion,
                                             emo.concerning ? " (concerning)" : "");
                            } else {
                                w = snprintf(insights_buf + insights_pos,
                                             sizeof(insights_buf) - insights_pos,
                                             "- Emotional tone: concerning\n");
                            }
                            if (w > 0 && (size_t)w < sizeof(insights_buf) - insights_pos)
                                insights_pos += (size_t)w;
                        }

                        w = snprintf(insights_buf + insights_pos,
                                     sizeof(insights_buf) - insights_pos,
                                     "--- End narrative/engagement/emotion ---\n");
                        if (w > 0 && (size_t)w < sizeof(insights_buf) - insights_pos)
                            insights_pos += (size_t)w;

                        if (convo_ctx) {
                            size_t total = convo_ctx_len + insights_pos + 1;
                            char *merged = (char *)alloc->alloc(alloc->ctx, total);
                            if (merged) {
                                memcpy(merged, convo_ctx, convo_ctx_len);
                                memcpy(merged + convo_ctx_len, insights_buf, insights_pos);
                                merged[total - 1] = '\0';
                                alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                convo_ctx = merged;
                                convo_ctx_len = total - 1;
                            }
                        } else {
                            convo_ctx = (char *)alloc->alloc(alloc->ctx, insights_pos + 1);
                            if (convo_ctx) {
                                memcpy(convo_ctx, insights_buf, insights_pos);
                                convo_ctx[insights_pos] = '\0';
                                convo_ctx_len = insights_pos;
                            }
                        }
                    }
                }

                /* F14: Escalation detection — if 3+ consecutive negative messages,
                 * use de-escalation directive (overrides energy).
                 * Skip in llm_decides mode — reduces prompt building latency. */
                bool use_escalation = false;
                if (history_entries && history_count > 0 && !llm_decides) {
                    hu_escalation_state_t escalation =
                        hu_conversation_detect_escalation(history_entries, history_count);
                    if (escalation.escalating) {
                        char deesc_buf[256];
                        size_t deesc_len = hu_conversation_build_deescalation_directive(
                            deesc_buf, sizeof(deesc_buf));
                        if (deesc_len > 0) {
                            use_escalation = true;
                            if (convo_ctx) {
                                size_t total = convo_ctx_len + deesc_len + 3;
                                char *merged = (char *)alloc->alloc(alloc->ctx, total);
                                if (merged) {
                                    memcpy(merged, convo_ctx, convo_ctx_len);
                                    merged[convo_ctx_len] = '\n';
                                    memcpy(merged + convo_ctx_len + 1, deesc_buf, deesc_len);
                                    merged[convo_ctx_len + 1 + deesc_len] = '\n';
                                    merged[total - 1] = '\0';
                                    alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                    convo_ctx = merged;
                                    convo_ctx_len = total - 1;
                                }
                            } else {
                                convo_ctx = (char *)alloc->alloc(alloc->ctx, deesc_len + 2);
                                if (convo_ctx) {
                                    memcpy(convo_ctx, deesc_buf, deesc_len);
                                    convo_ctx[deesc_len] = '\n';
                                    convo_ctx[deesc_len + 1] = '\0';
                                    convo_ctx_len = deesc_len + 1;
                                }
                            }
                            if (agent && agent->bth_metrics)
                                agent->bth_metrics->emotions_surfaced++;
                        }
                    }
                }

                /* F13: Energy matching — detect emotional energy of incoming message,
                 * inject [ENERGY: ...] directive when not neutral. De-escalation overrides. */
                /* F57: Multi-thread energy tracking — record per-conversation energy */
                static hu_thread_energy_tracker_t g_energy_tracker;
                static bool g_energy_tracker_inited = false;
                if (!g_energy_tracker_inited) {
                    hu_thread_energy_init(&g_energy_tracker);
                    g_energy_tracker_inited = true;
                }
                if (!use_escalation && combined_len > 0 && !llm_decides) {
                    hu_energy_level_t energy = hu_conversation_detect_energy(
                        combined, combined_len, history_entries, history_count);
                    hu_thread_energy_update(&g_energy_tracker, batch_key, key_len, energy,
                                            (uint64_t)time(NULL) * 1000ULL);
                    if (energy != HU_ENERGY_NEUTRAL) {
                        char energy_buf[128];
                        size_t energy_len = hu_conversation_build_energy_directive(
                            energy, energy_buf, sizeof(energy_buf));
                        if (energy_len > 0) {
                            if (convo_ctx) {
                                size_t total = convo_ctx_len + energy_len + 3;
                                char *merged = (char *)alloc->alloc(alloc->ctx, total);
                                if (merged) {
                                    memcpy(merged, convo_ctx, convo_ctx_len);
                                    merged[convo_ctx_len] = '\n';
                                    memcpy(merged + convo_ctx_len + 1, energy_buf, energy_len);
                                    merged[convo_ctx_len + 1 + energy_len] = '\n';
                                    merged[total - 1] = '\0';
                                    alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                    convo_ctx = merged;
                                    convo_ctx_len = total - 1;
                                }
                            } else {
                                convo_ctx = (char *)alloc->alloc(alloc->ctx, energy_len + 2);
                                if (convo_ctx) {
                                    memcpy(convo_ctx, energy_buf, energy_len);
                                    convo_ctx[energy_len] = '\n';
                                    convo_ctx[energy_len + 1] = '\0';
                                    convo_ctx_len = energy_len + 1;
                                }
                            }
                            if (agent && agent->bth_metrics)
                                agent->bth_metrics->emotions_surfaced++;
                        }
                    }
                }

                /* F57: Multi-thread energy isolation hint */
                {
                    char iso_buf[256];
                    size_t iso_len = hu_thread_energy_build_isolation_hint(
                        &g_energy_tracker, batch_key, key_len, iso_buf, sizeof(iso_buf));
                    if (iso_len > 0 && convo_ctx) {
                        size_t total = convo_ctx_len + iso_len + 2;
                        char *merged = (char *)alloc->alloc(alloc->ctx, total);
                        if (merged) {
                            memcpy(merged, convo_ctx, convo_ctx_len);
                            merged[convo_ctx_len] = '\n';
                            memcpy(merged + convo_ctx_len + 1, iso_buf, iso_len);
                            merged[total - 1] = '\0';
                            alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                            convo_ctx = merged;
                            convo_ctx_len = total - 1;
                        }
                    }
                }

                /* TRUST-001: Trap question detection — inject anti-fabrication directive.
                 * Skip in llm_decides mode. */
#ifdef HU_ENABLE_SQLITE
                if (!llm_decides) {
                    hu_challenge_result_t adv_r;
                    if (hu_adversarial_detect(combined, combined_len, &adv_r) == HU_OK &&
                        adv_r.type == HU_CHALLENGE_TRAP_QUESTION && adv_r.confidence >= 0.5) {
                        /* Check if we have a matching episode for this contact */
                        sqlite3 *trap_db = hu_sqlite_memory_get_db(agent->memory);
                        bool has_episode = false;
                        if (trap_db) {
                            hu_claim_result_t trap_cr;
                            if (hu_memory_verify_claim(alloc, trap_db, batch_key, key_len, combined,
                                                       combined_len, &trap_cr) == HU_OK &&
                                trap_cr.confidence >= 0.15) {
                                has_episode = true;
                            }
                        }
                        if (!has_episode) {
                            const char *trap_dir =
                                "[MEMORY GUARD] You have no memory of this. "
                                "Do NOT fabricate or guess. If asked about a shared "
                                "experience you cannot verify, say so honestly.";
                            size_t trap_dir_len = strlen(trap_dir);
                            if (convo_ctx) {
                                size_t total = convo_ctx_len + trap_dir_len + 2;
                                char *merged = (char *)alloc->alloc(alloc->ctx, total);
                                if (merged) {
                                    memcpy(merged, convo_ctx, convo_ctx_len);
                                    merged[convo_ctx_len] = '\n';
                                    memcpy(merged + convo_ctx_len + 1, trap_dir, trap_dir_len);
                                    merged[total - 1] = '\0';
                                    alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                    convo_ctx = merged;
                                    convo_ctx_len = total - 1;
                                }
                            } else {
                                convo_ctx = (char *)alloc->alloc(alloc->ctx, trap_dir_len + 2);
                                if (convo_ctx) {
                                    memcpy(convo_ctx, trap_dir, trap_dir_len);
                                    convo_ctx[trap_dir_len] = '\n';
                                    convo_ctx[trap_dir_len + 1] = '\0';
                                    convo_ctx_len = trap_dir_len + 1;
                                }
                            }
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "TRUST-001: trap question detected, no matching "
                                        "episode for %.*s",
                                        (int)(key_len > 20 ? 20 : key_len), batch_key);
                        }
                    }
                }
#endif

                /* TRUST-006: Inject trust-level directive when trust is below neutral */
                {
                    hu_trust_state_t ts;
                    if (hu_daemon_get_trust_state(batch_key, key_len, &ts) == HU_OK) {
                        char *trust_dir = NULL;
                        size_t trust_dir_len = 0;
                        if (hu_trust_state_build_directive(alloc, &ts, &trust_dir,
                                                           &trust_dir_len) == HU_OK &&
                            trust_dir && trust_dir_len > 0) {
                            if (convo_ctx) {
                                size_t total = convo_ctx_len + trust_dir_len + 2;
                                char *merged = (char *)alloc->alloc(alloc->ctx, total);
                                if (merged) {
                                    memcpy(merged, convo_ctx, convo_ctx_len);
                                    merged[convo_ctx_len] = '\n';
                                    memcpy(merged + convo_ctx_len + 1, trust_dir, trust_dir_len);
                                    merged[total - 1] = '\0';
                                    alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                    convo_ctx = merged;
                                    convo_ctx_len = total - 1;
                                }
                            } else {
                                convo_ctx = (char *)alloc->alloc(alloc->ctx, trust_dir_len + 2);
                                if (convo_ctx) {
                                    memcpy(convo_ctx, trust_dir, trust_dir_len);
                                    convo_ctx[trust_dir_len] = '\n';
                                    convo_ctx[trust_dir_len + 1] = '\0';
                                    convo_ctx_len = trust_dir_len + 1;
                                }
                            }
                            alloc->free(alloc->ctx, trust_dir, trust_dir_len + 1);
                        }
                    }
                }

                /* F27: Comfort pattern — when emotion is negative, inject learned preference.
                 * Skip in llm_decides: avoids expensive emotion LLM calls. */
                if (history_entries && history_count > 0 && agent->memory && !llm_decides) {
                    hu_emotional_state_t emo_f27 =
                        hu_daemon_detect_emotion(alloc, agent, history_entries, history_count);
                    bool emo_negative =
                        emo_f27.concerning || (emo_f27.dominant_emotion &&
                                               (strcmp(emo_f27.dominant_emotion, "sad") == 0 ||
                                                strcmp(emo_f27.dominant_emotion, "stressed") == 0 ||
                                                strcmp(emo_f27.dominant_emotion, "anxious") == 0 ||
                                                strcmp(emo_f27.dominant_emotion, "worried") == 0));
                    if (emo_negative) {
                        const char *emotion_str =
                            emo_f27.dominant_emotion && emo_f27.dominant_emotion[0]
                                ? emo_f27.dominant_emotion
                                : "concerning";
                        char pref_type[32];
                        size_t pref_len = 0;
                        if (hu_comfort_pattern_get_preferred(
                                alloc, agent->memory, batch_key, key_len, emotion_str,
                                strlen(emotion_str), pref_type, sizeof(pref_type),
                                &pref_len) == HU_OK &&
                            pref_len > 0) {
                            char comfort_buf[256];
                            size_t comfort_len = hu_conversation_build_comfort_directive(
                                pref_type, pref_len, emotion_str, strlen(emotion_str), comfort_buf,
                                sizeof(comfort_buf));
                            if (comfort_len > 0) {
                                if (convo_ctx) {
                                    size_t total = convo_ctx_len + comfort_len + 3;
                                    char *merged = (char *)alloc->alloc(alloc->ctx, total);
                                    if (merged) {
                                        memcpy(merged, convo_ctx, convo_ctx_len);
                                        merged[convo_ctx_len] = '\n';
                                        memcpy(merged + convo_ctx_len + 1, comfort_buf,
                                               comfort_len);
                                        merged[convo_ctx_len + 1 + comfort_len] = '\n';
                                        merged[total - 1] = '\0';
                                        alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                        convo_ctx = merged;
                                        convo_ctx_len = total - 1;
                                    }
                                } else {
                                    convo_ctx = (char *)alloc->alloc(alloc->ctx, comfort_len + 2);
                                    if (convo_ctx) {
                                        memcpy(convo_ctx, comfort_buf, comfort_len);
                                        convo_ctx[comfort_len] = '\n';
                                        convo_ctx[comfort_len + 1] = '\0';
                                        convo_ctx_len = comfort_len + 1;
                                    }
                                }
                            }
                        }
                    }
                }

                /* F16: Context modifiers — heavy topics, personal sharing, high emotion, early
                 * turn. Skip in llm_decides: avoids expensive emotion LLM calls. */
                if (history_entries && history_count > 0 && !llm_decides) {
                    hu_emotional_state_t emo_ctx =
                        hu_daemon_detect_emotion(alloc, agent, history_entries, history_count);
                    const hu_context_modifiers_t *mods =
                        (agent && agent->persona) ? &agent->persona->context_modifiers : NULL;
                    char mod_buf[512];
                    size_t mod_len = hu_conversation_build_context_modifiers(
                        history_entries, history_count, &emo_ctx, mods, mod_buf, sizeof(mod_buf));
                    if (mod_len > 0) {
                        if (convo_ctx) {
                            size_t total = convo_ctx_len + mod_len + 3;
                            char *merged = (char *)alloc->alloc(alloc->ctx, total);
                            if (merged) {
                                memcpy(merged, convo_ctx, convo_ctx_len);
                                merged[convo_ctx_len] = '\n';
                                memcpy(merged + convo_ctx_len + 1, mod_buf, mod_len);
                                merged[convo_ctx_len + 1 + mod_len] = '\n';
                                merged[total - 1] = '\0';
                                alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                convo_ctx = merged;
                                convo_ctx_len = total - 1;
                            }
                        } else {
                            convo_ctx = (char *)alloc->alloc(alloc->ctx, mod_len + 2);
                            if (convo_ctx) {
                                memcpy(convo_ctx, mod_buf, mod_len);
                                convo_ctx[mod_len] = '\n';
                                convo_ctx[mod_len + 1] = '\0';
                                convo_ctx_len = mod_len + 1;
                            }
                        }
                    }
                }

                /* F17: First-time vulnerability detection — extra care when they share
                 * something personal for the first time. Skip in llm_decides mode. */
                if (!llm_decides) {
                    hu_vulnerability_state_t vuln = hu_conversation_detect_first_time_vulnerability(
                        combined, combined_len, agent ? agent->memory : NULL, batch_key, key_len);
                    if (vuln.first_time && vuln.topic_category) {
                        char vuln_buf[512];
                        size_t vuln_len = hu_conversation_build_vulnerability_directive(
                            &vuln, vuln_buf, sizeof(vuln_buf));
                        if (vuln_len > 0) {
                            if (convo_ctx) {
                                size_t total = convo_ctx_len + vuln_len + 3;
                                char *merged = (char *)alloc->alloc(alloc->ctx, total);
                                if (merged) {
                                    memcpy(merged, convo_ctx, convo_ctx_len);
                                    merged[convo_ctx_len] = '\n';
                                    memcpy(merged + convo_ctx_len + 1, vuln_buf, vuln_len);
                                    merged[convo_ctx_len + 1 + vuln_len] = '\n';
                                    merged[total - 1] = '\0';
                                    alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                    convo_ctx = merged;
                                    convo_ctx_len = total - 1;
                                }
                            } else {
                                convo_ctx = (char *)alloc->alloc(alloc->ctx, vuln_len + 2);
                                if (convo_ctx) {
                                    memcpy(convo_ctx, vuln_buf, vuln_len);
                                    convo_ctx[vuln_len] = '\n';
                                    convo_ctx[vuln_len + 1] = '\0';
                                    convo_ctx_len = vuln_len + 1;
                                }
                            }
#ifndef HU_IS_TEST
                            /* Record so future calls return first_time=false */
                            if (agent && agent->memory) {
                                size_t tl = strlen(vuln.topic_category);
                                (void)hu_emotional_moment_record(alloc, agent->memory, batch_key,
                                                                 key_len, vuln.topic_category, tl,
                                                                 "vulnerable", 9, vuln.intensity);
                            }
#endif
                        }
                    }
                }

                /* F49: Call escalation — when text isn't enough, suggest a call.
                 * Skip in llm_decides mode. */
                if (combined_len > 0 && !llm_decides) {
                    hu_call_escalation_t call_esc = hu_conversation_should_escalate_to_call(
                        combined, combined_len, history_entries, history_count);
                    if (call_esc.should_suggest) {
                        char call_buf[512];
                        size_t call_len = hu_conversation_build_call_directive(
                            combined, combined_len, call_buf, sizeof(call_buf));
                        if (call_len > 0) {
                            if (convo_ctx) {
                                size_t total = convo_ctx_len + call_len + 3;
                                char *merged = (char *)alloc->alloc(alloc->ctx, total);
                                if (merged) {
                                    memcpy(merged, convo_ctx, convo_ctx_len);
                                    merged[convo_ctx_len] = '\n';
                                    memcpy(merged + convo_ctx_len + 1, call_buf, call_len);
                                    merged[convo_ctx_len + 1 + call_len] = '\n';
                                    merged[total - 1] = '\0';
                                    alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                    convo_ctx = merged;
                                    convo_ctx_len = total - 1;
                                }
                            } else {
                                convo_ctx = (char *)alloc->alloc(alloc->ctx, call_len + 2);
                                if (convo_ctx) {
                                    memcpy(convo_ctx, call_buf, call_len);
                                    convo_ctx[call_len] = '\n';
                                    convo_ctx[call_len + 1] = '\0';
                                    convo_ctx_len = call_len + 1;
                                }
                            }
                        }
                    }
                }

                /* W9: inject unified world-model context (entities, relations,
                 * goals, negatives, theory-of-mind, recent topics) via the
                 * W7 facade bridge.  Falls back to legacy GraphRAG when the
                 * facade is unavailable.  Skip in llm_decides mode — prompt
                 * inflation. */
#ifdef HU_ENABLE_SQLITE
                if (!llm_decides && batch_key && key_len > 0) {
                    char *graph_ctx = NULL;
                    size_t graph_ctx_len = 0;
                    hu_error_t gerr = HU_ERR_NOT_SUPPORTED;
                    if (agent && agent->w7_facade) {
                        /* P1.1-P1.3: imessage batch path uses agent's persona
                         * + the iMessage channel name. */
                        hu_persona_context_t batch_pctx;
                        batch_pctx.persona = agent->persona;
                        batch_pctx.channel = "imessage";
                        batch_pctx.channel_len = 8;
                        batch_pctx.delta_limit = 8;
                        gerr = hu_w7_render_world_model(agent->w7_facade, alloc, batch_key, key_len,
                                                        0, &graph_ctx, &graph_ctx_len, NULL, 0,
                                                        NULL, 0, NULL, 0, &agent->personal_model,
                                                        agent->persona ? &batch_pctx : NULL);
                    }
                    if (gerr != HU_OK && graph) {
                        gerr = hu_graph_build_contact_context(graph, alloc, combined, combined_len,
                                                              batch_key, key_len, 2, 1024,
                                                              &graph_ctx, &graph_ctx_len);
                    }
                    if (gerr == HU_OK && graph_ctx && graph_ctx_len > 0) {
                        if (convo_ctx) {
                            size_t total = convo_ctx_len + graph_ctx_len + 2;
                            char *merged = (char *)alloc->alloc(alloc->ctx, total);
                            if (merged) {
                                memcpy(merged, convo_ctx, convo_ctx_len);
                                merged[convo_ctx_len] = '\n';
                                memcpy(merged + convo_ctx_len + 1, graph_ctx, graph_ctx_len);
                                merged[total - 1] = '\0';
                                alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                convo_ctx = merged;
                                convo_ctx_len = total - 1;
                            }
                        } else {
                            convo_ctx = graph_ctx;
                            convo_ctx_len = graph_ctx_len;
                            graph_ctx = NULL;
                        }
                        if (graph_ctx)
                            alloc->free(alloc->ctx, graph_ctx, graph_ctx_len + 1);
                    }
                }
#endif

                /* 6. Attachment context: guidance when attachments detected in history.
                 * When provider supports vision and channel exposes attachments, try to
                 * get image path and describe it for richer context.
                 * Skip in llm_decides: vision description uses expensive LLM call. */
                if (history_entries && history_count > 0 && !llm_decides) {
                    size_t attach_ctx_len = 0;
                    char *attach_ctx = hu_conversation_attachment_context(
                        alloc, history_entries, history_count, &attach_ctx_len);
#ifndef HU_IS_TEST
                    /* Vision: provider supports vision and channel exposes latest attachment path.
                     */
                    if (attach_ctx && attach_ctx_len > 0 && agent->provider.vtable &&
                        agent->provider.vtable->supports_vision &&
                        agent->provider.vtable->supports_vision(agent->provider.ctx) &&
                        ch->channel->vtable->get_latest_attachment_path && batch_key &&
                        key_len > 0) {
                        char *img_path = ch->channel->vtable->get_latest_attachment_path(
                            ch->channel->ctx, alloc, batch_key, key_len);
                        if (img_path) {
                            char *desc = NULL;
                            size_t desc_len = 0;
                            const char *model = agent->model_name ? agent->model_name : "gpt-4o";
                            size_t model_len =
                                agent->model_name_len > 0 ? agent->model_name_len : strlen(model);
                            hu_error_t verr = hu_vision_describe_image(
                                alloc, &agent->provider, img_path, strlen(img_path), model,
                                model_len, &desc, &desc_len);
                            alloc->free(alloc->ctx, img_path, strlen(img_path) + 1);
                            if (verr == HU_OK && desc && desc_len > 0) {
                                size_t vision_ctx_len = 0;
                                char *vision_ctx =
                                    hu_vision_build_context(alloc, desc, desc_len, &vision_ctx_len);
                                alloc->free(alloc->ctx, desc, desc_len + 1);
                                if (vision_ctx && vision_ctx_len > 0) {
                                    alloc->free(alloc->ctx, attach_ctx, attach_ctx_len + 1);
                                    attach_ctx = vision_ctx;
                                    attach_ctx_len = vision_ctx_len;
#ifndef HU_IS_TEST
                                    if (agent && agent->bth_metrics)
                                        agent->bth_metrics->vision_descriptions++;
#endif
                                }
                            } else if (desc) {
                                alloc->free(alloc->ctx, desc, desc_len + 1);
                            }
                        }
                    }
#endif
                    if (attach_ctx && attach_ctx_len > 0) {
#ifndef HU_IS_TEST
                        if (agent && agent->bth_metrics)
                            agent->bth_metrics->attachment_contexts++;
#endif
                        if (convo_ctx) {
                            size_t total = convo_ctx_len + attach_ctx_len + 2;
                            char *merged = (char *)alloc->alloc(alloc->ctx, total);
                            if (merged) {
                                memcpy(merged, convo_ctx, convo_ctx_len);
                                merged[convo_ctx_len] = '\n';
                                memcpy(merged + convo_ctx_len + 1, attach_ctx, attach_ctx_len);
                                merged[total - 1] = '\0';
                                alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                convo_ctx = merged;
                                convo_ctx_len = total - 1;
                            }
                        } else {
                            convo_ctx = attach_ctx;
                            convo_ctx_len = attach_ctx_len;
                            attach_ctx = NULL;
                        }
                        if (attach_ctx)
                            alloc->free(alloc->ctx, attach_ctx, attach_ctx_len + 1);
                    } else if (attach_ctx) {
                        alloc->free(alloc->ctx, attach_ctx, attach_ctx_len + 1);
                    }
                }

                /* 3b. Conversation callbacks: thread callback (history) + memory-based.
                 * Skip in llm_decides mode — reduces prompt bloat. */
                if (!llm_decides) {
                    char *thread_cb = NULL;
                    size_t thread_cb_len = 0;
                    char *mem_cb = NULL;
                    size_t mem_cb_len = 0;
                    if (history_entries && history_count > 0) {
                        thread_cb = hu_conversation_build_callback(alloc, history_entries,
                                                                   history_count, &thread_cb_len);
                    }
                    if (agent->memory && agent->memory->vtable && agent->memory->vtable->recall) {
                        mem_cb = build_callback_context(alloc, agent->memory, batch_key, key_len,
                                                        combined, combined_len, &mem_cb_len, agent);
                    }
                    char *cb_ctx = NULL;
                    size_t cb_len = 0;
                    if (thread_cb && mem_cb) {
                        size_t merged_len = thread_cb_len + mem_cb_len + 2;
                        cb_ctx = (char *)alloc->alloc(alloc->ctx, merged_len + 1);
                        if (cb_ctx) {
                            memcpy(cb_ctx, thread_cb, thread_cb_len);
                            cb_ctx[thread_cb_len] = '\n';
                            memcpy(cb_ctx + thread_cb_len + 1, mem_cb, mem_cb_len);
                            cb_ctx[merged_len] = '\0';
                            cb_len = merged_len;
                        }
                        alloc->free(alloc->ctx, thread_cb, thread_cb_len + 1);
                        alloc->free(alloc->ctx, mem_cb, mem_cb_len + 1);
                    } else if (thread_cb) {
                        cb_ctx = thread_cb;
                        cb_len = thread_cb_len;
                    } else if (mem_cb) {
                        cb_ctx = mem_cb;
                        cb_len = mem_cb_len;
                    }
                    if (cb_ctx && convo_ctx) {
                        size_t merged_len = convo_ctx_len + cb_len + 1;
                        char *merged = (char *)alloc->alloc(alloc->ctx, merged_len + 1);
                        if (merged) {
                            memcpy(merged, convo_ctx, convo_ctx_len);
                            merged[convo_ctx_len] = '\n';
                            memcpy(merged + convo_ctx_len + 1, cb_ctx, cb_len);
                            merged[merged_len] = '\0';
                            alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                            convo_ctx = merged;
                            convo_ctx_len = merged_len;
                        }
                        alloc->free(alloc->ctx, cb_ctx, cb_len + 1);
                    } else if (cb_ctx && !convo_ctx) {
                        convo_ctx = cb_ctx;
                        convo_ctx_len = cb_len;
                    }
#ifndef HU_IS_TEST
                    if (cb_len > 0 && agent && agent->bth_metrics)
                        agent->bth_metrics->callbacks_triggered++;
#endif
                }

#ifndef HU_IS_TEST
                /* Episodic: load recent sessions for context.
                 * Skip in llm_decides: reduces prompt bloat. */
                char *episodic_ctx = NULL;
                size_t episodic_ctx_len = 0;
                char *avoidance_json = NULL;
                size_t avoidance_len = 0;
                if (agent->memory && !llm_decides) {
                    hu_episodic_load(agent->memory, alloc, &episodic_ctx, &episodic_ctx_len);
                }
                /* F19: Inside jokes — inject for natural callback opportunities.
                 * Skip in llm_decides mode — prompt inflation. */
                hu_inside_joke_t *jokes_ctx = NULL;
                size_t jokes_count = 0;
                if (agent->memory && !llm_decides &&
                    hu_superhuman_inside_joke_list(agent->memory, alloc, batch_key, key_len, 5,
                                                   &jokes_ctx, &jokes_count) == HU_OK &&
                    jokes_ctx && jokes_count > 0) {
                    char jokes_buf[768];
                    const size_t jokes_cap = sizeof(jokes_buf);
                    size_t jokes_pos =
                        hu_buf_appendf(jokes_buf, jokes_cap, 0, "Inside jokes with this contact: ");
                    if (jokes_pos >= jokes_cap)
                        jokes_pos = jokes_cap - 1;
                    for (size_t j = 0; j < jokes_count && jokes_pos < 700; j++) {
                        if (j > 0) {
                            jokes_pos = hu_buf_appendf(jokes_buf, jokes_cap, jokes_pos, "; ");
                            if (jokes_pos >= jokes_cap)
                                jokes_pos = jokes_cap - 1;
                        }
                        size_t ctx_len = strnlen(jokes_ctx[j].context, 80);
                        size_t pl_len = strnlen(jokes_ctx[j].punchline, 60);
                        jokes_pos = hu_buf_appendf(jokes_buf, jokes_cap, jokes_pos, "[%.*s] %.*s",
                                                   (int)ctx_len, jokes_ctx[j].context, (int)pl_len,
                                                   jokes_ctx[j].punchline);
                        if (jokes_pos >= jokes_cap)
                            jokes_pos = jokes_cap - 1;
                    }
                    if (jokes_pos < jokes_cap - 32) {
                        jokes_pos = hu_buf_appendf(jokes_buf, jokes_cap, jokes_pos,
                                                   ". Use naturally when relevant.");
                        if (jokes_pos >= jokes_cap)
                            jokes_pos = jokes_cap - 1;
                    }
                    if (jokes_pos > 0 && jokes_pos < sizeof(jokes_buf)) {
                        char *jokes_str = (char *)alloc->alloc(alloc->ctx, jokes_pos + 1);
                        if (jokes_str) {
                            memcpy(jokes_str, jokes_buf, jokes_pos);
                            jokes_str[jokes_pos] = '\0';
                            if (convo_ctx) {
                                size_t total = convo_ctx_len + jokes_pos + 3;
                                char *merged = (char *)alloc->alloc(alloc->ctx, total);
                                if (merged) {
                                    memcpy(merged, convo_ctx, convo_ctx_len);
                                    merged[convo_ctx_len] = '\n';
                                    memcpy(merged + convo_ctx_len + 1, jokes_str, jokes_pos);
                                    merged[convo_ctx_len + 1 + jokes_pos] = '\n';
                                    merged[total - 1] = '\0';
                                    alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                    convo_ctx = merged;
                                    convo_ctx_len = total - 1;
                                }
                                alloc->free(alloc->ctx, jokes_str, jokes_pos + 1);
                            } else {
                                convo_ctx = jokes_str;
                                convo_ctx_len = jokes_pos;
                            }
                        }
                    }
                    hu_superhuman_inside_joke_free(alloc, jokes_ctx, jokes_count);
                }
                /* F18: Micro-moments — inject notable details for natural reference.
                 * Skip in llm_decides mode. */
                char *mm_json = NULL;
                size_t mm_len = 0;
                if (agent->memory && !llm_decides &&
                    hu_superhuman_micro_moment_list(agent->memory, alloc, batch_key, key_len, 10,
                                                    &mm_json, &mm_len) == HU_OK &&
                    mm_json && mm_len > 0) {
                    char mm_buf[1024];
                    int mb = snprintf(mm_buf, sizeof(mm_buf),
                                      "Notable details about this contact: %.*s "
                                      "Reference naturally when relevant.",
                                      (int)(mm_len < 900 ? mm_len : 900), mm_json);
                    if (mb > 0 && (size_t)mb < sizeof(mm_buf)) {
                        char *mm_str = (char *)alloc->alloc(alloc->ctx, (size_t)mb + 1);
                        if (mm_str) {
                            memcpy(mm_str, mm_buf, (size_t)mb);
                            mm_str[mb] = '\0';
                            if (convo_ctx) {
                                size_t total = convo_ctx_len + (size_t)mb + 3;
                                char *merged = (char *)alloc->alloc(alloc->ctx, total);
                                if (merged) {
                                    memcpy(merged, convo_ctx, convo_ctx_len);
                                    merged[convo_ctx_len] = '\n';
                                    memcpy(merged + convo_ctx_len + 1, mm_str, (size_t)mb);
                                    merged[convo_ctx_len + 1 + (size_t)mb] = '\n';
                                    merged[total - 1] = '\0';
                                    alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                    convo_ctx = merged;
                                    convo_ctx_len = total - 1;
                                }
                                alloc->free(alloc->ctx, mm_str, (size_t)mb + 1);
                            } else {
                                convo_ctx = mm_str;
                                convo_ctx_len = (size_t)mb;
                            }
                        }
                    }
                    alloc->free(alloc->ctx, mm_json, mm_len + 1);
                }
                /* F21: Avoidance patterns — inject for context, don't push.
                 * Skip in llm_decides mode. */
                if (agent->memory && !llm_decides &&
                    hu_superhuman_avoidance_list(agent->memory, alloc, batch_key, key_len,
                                                 &avoidance_json, &avoidance_len) == HU_OK &&
                    avoidance_json && avoidance_len > 0 && strstr(avoidance_json, "- ") != NULL) {
                    char avoid_buf[512];
                    int ab =
                        snprintf(avoid_buf, sizeof(avoid_buf),
                                 "Topics they may avoid: %.*s Don't push; use for context.",
                                 (int)(avoidance_len < 400 ? avoidance_len : 400), avoidance_json);
                    if (ab > 0 && (size_t)ab < sizeof(avoid_buf)) {
                        char *avoid_str = (char *)alloc->alloc(alloc->ctx, (size_t)ab + 1);
                        if (avoid_str) {
                            memcpy(avoid_str, avoid_buf, (size_t)ab);
                            avoid_str[ab] = '\0';
                            if (convo_ctx) {
                                size_t total = convo_ctx_len + (size_t)ab + 3;
                                char *merged = (char *)alloc->alloc(alloc->ctx, total);
                                if (merged) {
                                    memcpy(merged, convo_ctx, convo_ctx_len);
                                    merged[convo_ctx_len] = '\n';
                                    memcpy(merged + convo_ctx_len + 1, avoid_str, (size_t)ab);
                                    merged[convo_ctx_len + 1 + (size_t)ab] = '\n';
                                    merged[total - 1] = '\0';
                                    alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                    convo_ctx = merged;
                                    convo_ctx_len = total - 1;
                                }
                                alloc->free(alloc->ctx, avoid_str, (size_t)ab + 1);
                            } else {
                                convo_ctx = avoid_str;
                                convo_ctx_len = (size_t)ab;
                            }
                        }
                    }
                    alloc->free(alloc->ctx, avoidance_json, avoidance_len + 1);
                    avoidance_json = NULL;
                    avoidance_len = 0;
                }
                /* F22: Pattern mirror — inject behavioral patterns for friend+ surfacing.
                 * Skip in llm_decides mode. */
                if (!llm_decides) {
                    char *pattern_json = NULL;
                    size_t pattern_len = 0;
                    if (agent->memory &&
                        hu_superhuman_pattern_list(agent->memory, alloc, batch_key, key_len, 5,
                                                   &pattern_json, &pattern_len) == HU_OK &&
                        pattern_json && pattern_len > 0 && strstr(pattern_json, "(none)") == NULL) {
                        char pattern_buf[1024];
                        int pb =
                            snprintf(pattern_buf, sizeof(pattern_buf),
                                     "Behavioral patterns observed: %.*s "
                                     "Surface naturally for friend+ contacts.",
                                     (int)(pattern_len < 900 ? pattern_len : 900), pattern_json);
                        if (pb > 0 && (size_t)pb < sizeof(pattern_buf)) {
                            char *pattern_str = (char *)alloc->alloc(alloc->ctx, (size_t)pb + 1);
                            if (pattern_str) {
                                memcpy(pattern_str, pattern_buf, (size_t)pb);
                                pattern_str[pb] = '\0';
                                if (convo_ctx) {
                                    size_t total = convo_ctx_len + (size_t)pb + 3;
                                    char *merged = (char *)alloc->alloc(alloc->ctx, total);
                                    if (merged) {
                                        memcpy(merged, convo_ctx, convo_ctx_len);
                                        merged[convo_ctx_len] = '\n';
                                        memcpy(merged + convo_ctx_len + 1, pattern_str, (size_t)pb);
                                        merged[convo_ctx_len + 1 + (size_t)pb] = '\n';
                                        merged[total - 1] = '\0';
                                        alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                        convo_ctx = merged;
                                        convo_ctx_len = total - 1;
                                    }
                                    alloc->free(alloc->ctx, pattern_str, (size_t)pb + 1);
                                } else {
                                    convo_ctx = pattern_str;
                                    convo_ctx_len = (size_t)pb;
                                }
                            }
                        }
                        alloc->free(alloc->ctx, pattern_json, pattern_len + 1);
                    }
                }
                /* F24: Growth celebration — inject recent milestones for natural celebration.
                 * Skip in llm_decides mode. */
                if (!llm_decides) {
                    char *growth_json = NULL;
                    size_t growth_len = 0;
                    if (agent->memory &&
                        hu_superhuman_growth_list_recent(agent->memory, alloc, batch_key, key_len,
                                                         3, &growth_json, &growth_len) == HU_OK &&
                        growth_json && growth_len > 0 && strstr(growth_json, "(none)") == NULL) {
                        char growth_buf[1024];
                        int gb = snprintf(growth_buf, sizeof(growth_buf),
                                          "Recent growth to celebrate: %.*s "
                                          "Reference naturally when relevant.",
                                          (int)(growth_len < 900 ? growth_len : 900), growth_json);
                        if (gb > 0 && (size_t)gb < sizeof(growth_buf)) {
                            char *growth_str = (char *)alloc->alloc(alloc->ctx, (size_t)gb + 1);
                            if (growth_str) {
                                memcpy(growth_str, growth_buf, (size_t)gb);
                                growth_str[gb] = '\0';
                                if (convo_ctx) {
                                    size_t total = convo_ctx_len + (size_t)gb + 3;
                                    char *merged = (char *)alloc->alloc(alloc->ctx, total);
                                    if (merged) {
                                        memcpy(merged, convo_ctx, convo_ctx_len);
                                        merged[convo_ctx_len] = '\n';
                                        memcpy(merged + convo_ctx_len + 1, growth_str, (size_t)gb);
                                        merged[convo_ctx_len + 1 + (size_t)gb] = '\n';
                                        merged[total - 1] = '\0';
                                        alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                        convo_ctx = merged;
                                        convo_ctx_len = total - 1;
                                    }
                                    alloc->free(alloc->ctx, growth_str, (size_t)gb + 1);
                                } else {
                                    convo_ctx = growth_str;
                                    convo_ctx_len = (size_t)gb;
                                }
                            }
                        }
                        alloc->free(alloc->ctx, growth_json, growth_len + 1);
                    }
                }
                if (episodic_ctx && episodic_ctx_len > 0) {
                    if (convo_ctx) {
                        size_t merged_len = convo_ctx_len + episodic_ctx_len + 2;
                        char *merged = (char *)alloc->alloc(alloc->ctx, merged_len + 1);
                        if (merged) {
                            memcpy(merged, convo_ctx, convo_ctx_len);
                            merged[convo_ctx_len] = '\n';
                            memcpy(merged + convo_ctx_len + 1, episodic_ctx, episodic_ctx_len);
                            merged[merged_len - 1] = '\n';
                            merged[merged_len] = '\0';
                            alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                            convo_ctx = merged;
                            convo_ctx_len = merged_len;
                        }
                        alloc->free(alloc->ctx, episodic_ctx, episodic_ctx_len + 1);
                    } else {
                        convo_ctx = episodic_ctx;
                        convo_ctx_len = episodic_ctx_len;
                    }
                }
#endif

#ifndef HU_IS_TEST
                /* Replay insights: per-contact lookup from memory store.
                 *
                 * 2026-05-16 incident: the previous version read under the
                 * GLOBAL key "replay:latest" and, on miss, fell back to the
                 * process-global `replay_insights` static buffer.  Both paths
                 * leaked one contact's replay-analysis blob into every other
                 * contact's prompt — confirmed source of the "Replay MCP"
                 * variants that shipped to all three relatives.  Static-buffer
                 * fallback removed entirely; the key is now per-contact.
                 */
                {
                    const char *ri_src = NULL;
                    size_t ri_len = 0;
                    char *ri_heap = NULL;
                    if (agent->memory && agent->memory->vtable && agent->memory->vtable->get &&
                        batch_key && key_len > 0) {
                        char ri_key[320];
                        size_t ri_key_len = 0;
                        if (hu_proactive_build_replay_key(batch_key, key_len, ri_key,
                                                          sizeof(ri_key), &ri_key_len)) {
                            hu_memory_entry_t ri_entry;
                            memset(&ri_entry, 0, sizeof(ri_entry));
                            bool ri_found = false;
                            if (agent->memory->vtable->get(agent->memory->ctx, alloc, ri_key,
                                                           ri_key_len, &ri_entry,
                                                           &ri_found) == HU_OK &&
                                ri_found && ri_entry.content && ri_entry.content_len > 0) {
                                ri_src = ri_entry.content;
                                ri_len = ri_entry.content_len;
                                ri_heap = (char *)ri_entry.content;
                            }
                        }
                    }
                    if (ri_src && ri_len > 0) {
                        if (convo_ctx) {
                            size_t merged_len = convo_ctx_len + ri_len + 2;
                            char *merged = (char *)alloc->alloc(alloc->ctx, merged_len + 1);
                            if (merged) {
                                memcpy(merged, convo_ctx, convo_ctx_len);
                                merged[convo_ctx_len] = '\n';
                                memcpy(merged + convo_ctx_len + 1, ri_src, ri_len);
                                merged[merged_len - 1] = '\n';
                                merged[merged_len] = '\0';
                                alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                convo_ctx = merged;
                                convo_ctx_len = merged_len;
                            }
                        } else {
                            convo_ctx = (char *)alloc->alloc(alloc->ctx, ri_len + 1);
                            if (convo_ctx) {
                                memcpy(convo_ctx, ri_src, ri_len);
                                convo_ctx[ri_len] = '\0';
                                convo_ctx_len = ri_len;
                            }
                        }
                    }
                    if (ri_heap)
                        alloc->free(alloc->ctx, ri_heap, ri_len + 1);
                }
                /* GraphRAG community insights: inject topic clusters from weekly detection */
                if (community_insights_len > 0) {
                    if (convo_ctx) {
                        size_t merged_len = convo_ctx_len + community_insights_len + 2;
                        char *merged = (char *)alloc->alloc(alloc->ctx, merged_len + 1);
                        if (merged) {
                            memcpy(merged, convo_ctx, convo_ctx_len);
                            merged[convo_ctx_len] = '\n';
                            memcpy(merged + convo_ctx_len + 1, community_insights,
                                   community_insights_len);
                            merged[merged_len - 1] = '\n';
                            merged[merged_len] = '\0';
                            alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                            convo_ctx = merged;
                            convo_ctx_len = merged_len;
                        }
                    } else {
                        convo_ctx = (char *)alloc->alloc(alloc->ctx, community_insights_len + 1);
                        if (convo_ctx) {
                            memcpy(convo_ctx, community_insights, community_insights_len);
                            convo_ctx[community_insights_len] = '\0';
                            convo_ctx_len = community_insights_len;
                        }
                    }
                }
#endif

                /* 4. Response constraints via channel vtable */
                uint32_t max_chars = 0;
                if (ch->channel->vtable->get_response_constraints) {
                    hu_channel_response_constraints_t constraints = {0};
                    if (ch->channel->vtable->get_response_constraints(ch->channel->ctx,
                                                                      &constraints) == HU_OK) {
                        max_chars = constraints.max_chars;
                    }
                }

                /* F15: Apply ratio-based length calibration */
                {
                    const hu_contact_profile_t *cp_lim =
                        (!msgs[batch_start].is_group && agent->persona && batch_key && key_len > 0)
                            ? hu_persona_find_contact(agent->persona, batch_key, key_len)
                            : NULL;
                    int calibrated = msgs[batch_start].is_group
                                         ? hu_conversation_max_response_chars(combined_len)
                                         : hu_conversation_max_response_chars_relational(
                                               combined_len, cp_lim, agent->relationship.stage);
                    if (calibrated > 0 && (max_chars == 0 || (uint32_t)calibrated < max_chars))
                        max_chars = (uint32_t)calibrated;
                }

                /* Brief mode: cap length (tight in groups; headroom for trusted 1:1). */
                if (brief_mode) {
                    const hu_contact_profile_t *cp_brief =
                        (!msgs[batch_start].is_group && agent->persona && batch_key && key_len > 0)
                            ? hu_persona_find_contact(agent->persona, batch_key, key_len)
                            : NULL;
                    uint32_t brief_cap = hu_conversation_brief_char_cap(
                        msgs[batch_start].is_group, cp_brief, agent->relationship.stage);
                    if (max_chars > brief_cap)
                        max_chars = brief_cap;
                }

                /* Honesty guardrail: inject if they asked "did you do X?" */
                {
                    char *honesty = hu_conversation_honesty_check(alloc, combined, combined_len);
                    if (honesty && convo_ctx) {
                        size_t h_len = strlen(honesty);
                        size_t total = convo_ctx_len + h_len + 2;
                        char *merged = (char *)alloc->alloc(alloc->ctx, total);
                        if (merged) {
                            memcpy(merged, convo_ctx, convo_ctx_len);
                            merged[convo_ctx_len] = '\n';
                            memcpy(merged + convo_ctx_len + 1, honesty, h_len);
                            merged[total - 1] = '\0';
                            alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                            convo_ctx = merged;
                            convo_ctx_len = total - 1;
                        }
                        alloc->free(alloc->ctx, honesty, h_len + 1);
                    } else if (honesty && !convo_ctx) {
                        convo_ctx = honesty;
                        convo_ctx_len = strlen(honesty);
                    } else if (honesty) {
                        alloc->free(alloc->ctx, honesty, strlen(honesty) + 1);
                    }
                }

                /* 5. Anti-repetition: detect patterns in our recent messages */
                if (history_entries && history_count >= 4) {
                    char rep_buf[1024];
                    size_t rep_len = hu_conversation_detect_repetition(
                        history_entries, history_count, rep_buf, sizeof(rep_buf));
                    if (rep_len > 0 && convo_ctx) {
                        size_t total = convo_ctx_len + rep_len + 1;
                        char *merged = (char *)alloc->alloc(alloc->ctx, total + 1);
                        if (merged) {
                            memcpy(merged, convo_ctx, convo_ctx_len);
                            merged[convo_ctx_len] = '\n';
                            memcpy(merged + convo_ctx_len + 1, rep_buf, rep_len);
                            merged[total] = '\0';
                            alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                            convo_ctx = merged;
                            convo_ctx_len = total;
                        }
                    } else if (rep_len > 0 && !convo_ctx) {
                        convo_ctx = (char *)alloc->alloc(alloc->ctx, rep_len + 1);
                        if (convo_ctx) {
                            memcpy(convo_ctx, rep_buf, rep_len);
                            convo_ctx[rep_len] = '\0';
                            convo_ctx_len = rep_len;
                        }
                    }
                }

                /* 6. Relationship-tier calibration from contact profile */
                if (agent->persona) {
                    const hu_contact_profile_t *cp_rel =
                        hu_persona_find_contact(agent->persona, batch_key, key_len);
                    if (cp_rel && (cp_rel->relationship_stage || cp_rel->warmth_level ||
                                   cp_rel->vulnerability_level)) {
                        char rel_buf[512];
                        size_t rel_len = hu_conversation_calibrate_relationship(
                            cp_rel->relationship_stage, cp_rel->warmth_level,
                            cp_rel->vulnerability_level, rel_buf, sizeof(rel_buf));
                        if (rel_len > 0 && convo_ctx) {
                            size_t total = convo_ctx_len + rel_len + 1;
                            char *merged = (char *)alloc->alloc(alloc->ctx, total + 1);
                            if (merged) {
                                memcpy(merged, convo_ctx, convo_ctx_len);
                                merged[convo_ctx_len] = '\n';
                                memcpy(merged + convo_ctx_len + 1, rel_buf, rel_len);
                                merged[total] = '\0';
                                alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                convo_ctx = merged;
                                convo_ctx_len = total;
                            }
                        } else if (rel_len > 0 && !convo_ctx) {
                            convo_ctx = (char *)alloc->alloc(alloc->ctx, rel_len + 1);
                            if (convo_ctx) {
                                memcpy(convo_ctx, rel_buf, rel_len);
                                convo_ctx[rel_len] = '\0';
                                convo_ctx_len = rel_len;
                            }
                        }
                    }
                }

                /* 6b. Link sharing context: when conversation calls for sharing a link */
                if (hu_conversation_should_share_link(combined, combined_len, history_entries,
                                                      history_count)) {
                    static const char LINK_CTX[] =
                        "\n### Link Sharing\nThe conversation naturally calls for sharing a link "
                        "or recommendation. If you have a relevant URL, include it in your "
                        "response.\n";
                    size_t link_len = sizeof(LINK_CTX) - 1;
                    if (convo_ctx) {
                        size_t total = convo_ctx_len + link_len + 1;
                        char *merged = (char *)alloc->alloc(alloc->ctx, total + 1);
                        if (merged) {
                            memcpy(merged, convo_ctx, convo_ctx_len);
                            memcpy(merged + convo_ctx_len, LINK_CTX, link_len + 1);
                            alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                            convo_ctx = merged;
                            convo_ctx_len = total;
                        }
                    } else {
                        convo_ctx = (char *)alloc->alloc(alloc->ctx, link_len + 1);
                        if (convo_ctx) {
                            memcpy(convo_ctx, LINK_CTX, link_len + 1);
                            convo_ctx_len = link_len;
                        }
                    }
#ifndef HU_IS_TEST
                    if (agent && agent->bth_metrics)
                        agent->bth_metrics->link_contexts++;
#endif
                }

                /* Pre-populate STM from current message for egraph.
                 * Skip in llm_decides mode — STM + egraph add latency. */
                if (!llm_decides) {
                    hu_fc_result_t fc_pre;
                    memset(&fc_pre, 0, sizeof(fc_pre));
                    (void)hu_fast_capture(alloc, combined, combined_len, &fc_pre);
                    uint64_t ts_ms = (uint64_t)time(NULL) * 1000;
                    hu_error_t fc_err =
                        hu_stm_record_turn(&agent->stm, "user", 4, combined, combined_len, ts_ms);
                    if (fc_err == HU_OK) {
                        size_t last_idx = hu_stm_count(&agent->stm) - 1;
                        if (fc_pre.primary_topic && fc_pre.primary_topic[0]) {
                            (void)hu_stm_turn_set_primary_topic(&agent->stm, last_idx,
                                                                fc_pre.primary_topic,
                                                                strlen(fc_pre.primary_topic));
                        }
                        for (size_t ei = 0; ei < fc_pre.emotion_count; ei++) {
                            (void)hu_stm_turn_add_emotion(&agent->stm, last_idx,
                                                          fc_pre.emotions[ei].tag,
                                                          fc_pre.emotions[ei].intensity);
                        }
                    }
#ifndef HU_IS_TEST
                    /* F25: Record emotional moments for 1–3 day check-ins.
                     * Skip in llm_decides: avoids post-response emotion LLM call. */
                    if (agent->memory && history_entries && history_count > 0 && !llm_decides) {
                        hu_emotional_state_t emo_rec =
                            hu_daemon_detect_emotion(alloc, agent, history_entries, history_count);
                        hu_escalation_state_t esc_rec =
                            hu_conversation_detect_escalation(history_entries, history_count);
                        hu_energy_level_t energy_rec = hu_conversation_detect_energy(
                            combined, combined_len, history_entries, history_count);
                        bool should_record = emo_rec.concerning || esc_rec.escalating ||
                                             energy_rec == HU_ENERGY_SAD ||
                                             energy_rec == HU_ENERGY_ANXIOUS ||
                                             (emo_rec.dominant_emotion &&
                                              (strcmp(emo_rec.dominant_emotion, "sad") == 0 ||
                                               strcmp(emo_rec.dominant_emotion, "stressed") == 0 ||
                                               strcmp(emo_rec.dominant_emotion, "anxious") == 0 ||
                                               strcmp(emo_rec.dominant_emotion, "worried") == 0));
                        if (should_record) {
                            /* P2-1 (2026-05-16): use safe predicate. NEVER fall
                             * back to raw user message — that shipped a
                             * first-person confession to family contacts. */
                            const char *emotion_str =
                                emo_rec.dominant_emotion && emo_rec.dominant_emotion[0]
                                    ? emo_rec.dominant_emotion
                                    : (esc_rec.escalating ? "escalating" : "concerning");
                            char topic_buf[128] = {0};
                            size_t topic_len = hu_emotional_moment_select_topic(
                                fc_pre.primary_topic, emotion_str, topic_buf, sizeof(topic_buf));
                            if (topic_len > 0) {
                                (void)hu_emotional_moment_record(
                                    alloc, agent->memory, batch_key, key_len, topic_buf, topic_len,
                                    emotion_str, strlen(emotion_str), emo_rec.intensity);
                            }
                            /* else: extraction yielded nothing AND no emotion
                             * keyword → SKIP the record. Better no data than
                             * leaked data. */
                        }
                    }
#endif
                    hu_fc_result_deinit(&fc_pre, alloc);
                }

                /* 7. Emotional topic map: topics → dominant emotions from STM.
                 * Skip in llm_decides mode. */
                if (!llm_decides) {
                    hu_emotional_graph_t egraph;
                    size_t egraph_len = 0;
                    hu_egraph_init(&egraph, *alloc);
                    (void)hu_egraph_populate_from_stm(&egraph, &agent->stm);
                    char *egraph_ctx = hu_egraph_build_context(alloc, &egraph, &egraph_len);
                    hu_egraph_deinit(&egraph);
                    if (egraph_ctx && egraph_len > 0) {
#ifndef HU_IS_TEST
                        if (agent && agent->bth_metrics)
                            agent->bth_metrics->egraph_contexts++;
#endif
                        if (convo_ctx) {
                            size_t total = convo_ctx_len + egraph_len + 2;
                            char *merged = (char *)alloc->alloc(alloc->ctx, total);
                            if (merged) {
                                memcpy(merged, convo_ctx, convo_ctx_len);
                                merged[convo_ctx_len] = '\n';
                                memcpy(merged + convo_ctx_len + 1, egraph_ctx, egraph_len + 1);
                                alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                convo_ctx = merged;
                                convo_ctx_len = total - 1;
                            }
                        } else {
                            convo_ctx = egraph_ctx;
                            convo_ctx_len = egraph_len;
                        }
                        if (convo_ctx != egraph_ctx)
                            alloc->free(alloc->ctx, egraph_ctx, egraph_len + 1);
                    } else if (egraph_ctx) {
                        alloc->free(alloc->ctx, egraph_ctx, egraph_len + 1);
                    }
                }

                /* 7b. Mood context: recent emotional state from memory.
                 * Skip in llm_decides mode — prompt inflation. */
                {
                    char *mood_ctx = NULL;
                    size_t mood_ctx_len = 0;
                    if (agent->memory && !llm_decides &&
                        hu_mood_build_context(alloc, agent->memory, batch_key, key_len, &mood_ctx,
                                              &mood_ctx_len) == HU_OK &&
                        mood_ctx && mood_ctx_len > 0) {
#ifndef HU_IS_TEST
                        if (agent->bth_metrics)
                            agent->bth_metrics->mood_contexts_built++;
#endif
                        if (convo_ctx) {
                            size_t total = convo_ctx_len + mood_ctx_len + 2;
                            char *merged = (char *)alloc->alloc(alloc->ctx, total);
                            if (merged) {
                                memcpy(merged, convo_ctx, convo_ctx_len);
                                merged[convo_ctx_len] = '\n';
                                memcpy(merged + convo_ctx_len + 1, mood_ctx, mood_ctx_len);
                                merged[total - 1] = '\0';
                                alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                convo_ctx = merged;
                                convo_ctx_len = total - 1;
                            }
                        } else {
                            convo_ctx = mood_ctx;
                            convo_ctx_len = mood_ctx_len;
                            mood_ctx = NULL;
                        }
                        if (mood_ctx)
                            alloc->free(alloc->ctx, mood_ctx, mood_ctx_len + 1);
                    }
                }

                /* ── BTH Tier 1: Wire conversation planning (t1a) ──────────── */
#ifndef HU_IS_TEST
                if (!llm_decides) {
                    hu_conversation_plan_t plan;
                    memset(&plan, 0, sizeof(plan));
                    const char *emotional_ctx = convo_ctx ? convo_ctx : "";
                    size_t emotional_len = convo_ctx ? convo_ctx_len : 0;
                    const char *hist_text = "";
                    size_t hist_len = 0;
                    char hist_buf[2048];
                    if (history_entries && history_count > 0) {
                        size_t hpos = 0;
                        for (size_t hi = 0; hi < history_count && hpos < sizeof(hist_buf) - 1;
                             hi++) {
                            int hw = snprintf(hist_buf + hpos, sizeof(hist_buf) - hpos, "%s: %s\n",
                                              history_entries[hi].from_me ? "me" : "them",
                                              history_entries[hi].text);
                            if (hw > 0 && hpos + (size_t)hw < sizeof(hist_buf))
                                hpos += (size_t)hw;
                        }
                        hist_buf[hpos] = '\0';
                        hist_text = hist_buf;
                        hist_len = hpos;
                    }
                    if (hu_plan_conversation(alloc, combined, combined_len, hist_text, hist_len,
                                             emotional_ctx, emotional_len, &plan) == HU_OK) {
                        char *plan_ctx = NULL;
                        size_t plan_ctx_len = 0;
                        if (hu_plan_build_prompt(&plan, alloc, &plan_ctx, &plan_ctx_len) == HU_OK &&
                            plan_ctx && plan_ctx_len > 0) {
                            if (convo_ctx) {
                                size_t total = convo_ctx_len + plan_ctx_len + 2;
                                char *merged = (char *)alloc->alloc(alloc->ctx, total);
                                if (merged) {
                                    memcpy(merged, convo_ctx, convo_ctx_len);
                                    merged[convo_ctx_len] = '\n';
                                    memcpy(merged + convo_ctx_len + 1, plan_ctx, plan_ctx_len);
                                    merged[total - 1] = '\0';
                                    alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                    convo_ctx = merged;
                                    convo_ctx_len = total - 1;
                                }
                            } else {
                                convo_ctx = plan_ctx;
                                convo_ctx_len = plan_ctx_len;
                                plan_ctx = NULL;
                            }
                            if (plan_ctx)
                                alloc->free(alloc->ctx, plan_ctx, plan_ctx_len + 1);
                        }
                        hu_conversation_plan_deinit(&plan, alloc);
                    }
                }

                /* ── BTH Tier 1: Theory of Mind context (t1b-pre) ──────────── */
                if (!llm_decides) {
                    size_t tom_idx = (size_t)-1;
                    for (size_t ti = 0; ti < tom_contact_count; ti++) {
                        if (strncmp(tom_contact_keys[ti], batch_key, key_len) == 0 &&
                            tom_contact_keys[ti][key_len] == '\0') {
                            tom_idx = ti;
                            break;
                        }
                    }
                    if (tom_idx == (size_t)-1 && tom_contact_count < HU_TOM_MAX_CONTACTS &&
                        key_len < 64) {
                        tom_idx = tom_contact_count;
                        memset(&tom_states[tom_idx], 0, sizeof(hu_tom_belief_state_t));
                        hu_tom_init(&tom_states[tom_idx], alloc, batch_key, key_len);
                        memcpy(tom_contact_keys[tom_idx], batch_key, key_len);
                        tom_contact_keys[tom_idx][key_len] = '\0';
                        tom_contact_count++;
                    }
                    if (tom_idx != (size_t)-1 && tom_states[tom_idx].belief_count > 0) {
                        char *tom_ctx = NULL;
                        size_t tom_ctx_len = 0;
                        if (hu_tom_build_context(&tom_states[tom_idx], alloc, &tom_ctx,
                                                 &tom_ctx_len) == HU_OK &&
                            tom_ctx && tom_ctx_len > 0) {
                            if (convo_ctx) {
                                size_t total = convo_ctx_len + tom_ctx_len + 2;
                                char *merged = (char *)alloc->alloc(alloc->ctx, total);
                                if (merged) {
                                    memcpy(merged, convo_ctx, convo_ctx_len);
                                    merged[convo_ctx_len] = '\n';
                                    memcpy(merged + convo_ctx_len + 1, tom_ctx, tom_ctx_len);
                                    merged[total - 1] = '\0';
                                    alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                    convo_ctx = merged;
                                    convo_ctx_len = total - 1;
                                }
                            } else {
                                convo_ctx = tom_ctx;
                                convo_ctx_len = tom_ctx_len;
                                tom_ctx = NULL;
                            }
                            if (tom_ctx)
                                alloc->free(alloc->ctx, tom_ctx, tom_ctx_len + 1);
                        }

                        /* ── Phase 4: MToM gap detection ─────────── */
                        hu_tom_gap_t *tom_gaps = NULL;
                        size_t tom_gap_count = 0;
                        if (hu_tom_detect_gaps(&tom_states[tom_idx], alloc, &tom_gaps,
                                               &tom_gap_count) == HU_OK &&
                            tom_gaps && tom_gap_count > 0) {
                            size_t gd_len = 0;
                            char *gd =
                                hu_tom_build_gap_directive(alloc, tom_gaps, tom_gap_count, &gd_len);
                            if (gd && gd_len > 0) {
                                if (convo_ctx) {
                                    size_t gt = convo_ctx_len + gd_len + 2;
                                    char *gm = (char *)alloc->alloc(alloc->ctx, gt);
                                    if (gm) {
                                        memcpy(gm, convo_ctx, convo_ctx_len);
                                        gm[convo_ctx_len] = '\n';
                                        memcpy(gm + convo_ctx_len + 1, gd, gd_len);
                                        gm[gt - 1] = '\0';
                                        alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                        convo_ctx = gm;
                                        convo_ctx_len = gt - 1;
                                    }
                                    alloc->free(alloc->ctx, gd, gd_len + 1);
                                } else {
                                    convo_ctx = gd;
                                    convo_ctx_len = gd_len;
                                }
                            } else if (gd) {
                                alloc->free(alloc->ctx, gd, gd_len + 1);
                            }
                            hu_tom_gaps_free(alloc, tom_gaps, tom_gap_count);
                        }
                    }
                }

                /* ── BTH Tier 1: Information asymmetry guidance (t1c) ──────── */
                if (!llm_decides) {
                    hu_info_asymmetry_t asym;
                    memset(&asym, 0, sizeof(asym));
                    const char *agent_ctx = convo_ctx ? convo_ctx : "";
                    size_t agent_ctx_len = convo_ctx ? convo_ctx_len : 0;
                    const char *ct_ctx = contact_ctx ? contact_ctx : "";
                    size_t ct_ctx_len = contact_ctx ? contact_ctx_len : 0;
                    if (hu_info_asymmetry_analyze(&asym, alloc, agent_ctx, agent_ctx_len, ct_ctx,
                                                  ct_ctx_len) == HU_OK &&
                        asym.gap_count > 0) {
                        char *ia_ctx = NULL;
                        size_t ia_ctx_len = 0;
                        if (hu_info_asymmetry_build_guidance(&asym, alloc, &ia_ctx, &ia_ctx_len) ==
                                HU_OK &&
                            ia_ctx && ia_ctx_len > 0) {
                            if (convo_ctx) {
                                size_t total = convo_ctx_len + ia_ctx_len + 2;
                                char *merged = (char *)alloc->alloc(alloc->ctx, total);
                                if (merged) {
                                    memcpy(merged, convo_ctx, convo_ctx_len);
                                    merged[convo_ctx_len] = '\n';
                                    memcpy(merged + convo_ctx_len + 1, ia_ctx, ia_ctx_len);
                                    merged[total - 1] = '\0';
                                    alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                    convo_ctx = merged;
                                    convo_ctx_len = total - 1;
                                }
                            } else {
                                convo_ctx = ia_ctx;
                                convo_ctx_len = ia_ctx_len;
                                ia_ctx = NULL;
                            }
                            if (ia_ctx)
                                alloc->free(alloc->ctx, ia_ctx, ia_ctx_len + 1);
                        }
                        hu_info_asymmetry_deinit(&asym, alloc);
                    }
                }

                /* ── BTH Tier 1: Anticipatory actions from GraphRAG (t1d) ──── */
                if (graph && !llm_decides) {
                    hu_anticipatory_result_t antic;
                    memset(&antic, 0, sizeof(antic));
                    int64_t now_ts = (int64_t)time(NULL);
                    hu_error_t antic_err;
                    if (agent && agent->w7_facade) {
                        hu_memory_facade_t *mf = hu_w7_facade_memory_handle(agent->w7_facade);
                        antic_err = hu_anticipatory_analyze_memory(mf, alloc, batch_key, key_len,
                                                                   now_ts, &antic);
                    } else {
                        antic_err = hu_anticipatory_analyze(graph, alloc, batch_key, key_len,
                                                            now_ts, &antic);
                    }
                    if (antic_err == HU_OK && antic.action_count > 0) {
                        char *antic_ctx = NULL;
                        size_t antic_ctx_len = 0;
                        if (hu_anticipatory_build_context(&antic, alloc, &antic_ctx,
                                                          &antic_ctx_len) == HU_OK &&
                            antic_ctx && antic_ctx_len > 0) {
                            if (convo_ctx) {
                                size_t total = convo_ctx_len + antic_ctx_len + 2;
                                char *merged = (char *)alloc->alloc(alloc->ctx, total);
                                if (merged) {
                                    memcpy(merged, convo_ctx, convo_ctx_len);
                                    merged[convo_ctx_len] = '\n';
                                    memcpy(merged + convo_ctx_len + 1, antic_ctx, antic_ctx_len);
                                    merged[total - 1] = '\0';
                                    alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                    convo_ctx = merged;
                                    convo_ctx_len = total - 1;
                                }
                            } else {
                                convo_ctx = antic_ctx;
                                convo_ctx_len = antic_ctx_len;
                                antic_ctx = NULL;
                            }
                            if (antic_ctx)
                                alloc->free(alloc->ctx, antic_ctx, antic_ctx_len + 1);
                        }
                        hu_anticipatory_result_deinit(&antic, alloc);
                    }
                }

                /* ── BTH Tier 1: Voice maturity guidance (t1e) ─────────────── */
                if (!llm_decides) {
                    size_t vm_idx = (size_t)-1;
                    for (size_t vi = 0; vi < voice_contact_count; vi++) {
                        if (strncmp(voice_contact_keys[vi], batch_key, key_len) == 0 &&
                            voice_contact_keys[vi][key_len] == '\0') {
                            vm_idx = vi;
                            break;
                        }
                    }
                    if (vm_idx == (size_t)-1 && voice_contact_count < HU_TOM_MAX_CONTACTS &&
                        key_len < 64) {
                        vm_idx = voice_contact_count;
                        hu_voice_profile_init(&voice_profiles[vm_idx]);
                        memcpy(voice_contact_keys[vm_idx], batch_key, key_len);
                        voice_contact_keys[vm_idx][key_len] = '\0';
                        voice_contact_count++;
                    }
                    if (vm_idx != (size_t)-1) {
                        char *vm_ctx = NULL;
                        size_t vm_ctx_len = 0;
                        if (hu_voice_build_guidance(&voice_profiles[vm_idx], alloc, &vm_ctx,
                                                    &vm_ctx_len) == HU_OK &&
                            vm_ctx && vm_ctx_len > 0) {
                            if (convo_ctx) {
                                size_t total = convo_ctx_len + vm_ctx_len + 2;
                                char *merged = (char *)alloc->alloc(alloc->ctx, total);
                                if (merged) {
                                    memcpy(merged, convo_ctx, convo_ctx_len);
                                    merged[convo_ctx_len] = '\n';
                                    memcpy(merged + convo_ctx_len + 1, vm_ctx, vm_ctx_len);
                                    merged[total - 1] = '\0';
                                    alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                    convo_ctx = merged;
                                    convo_ctx_len = total - 1;
                                }
                            } else {
                                convo_ctx = vm_ctx;
                                convo_ctx_len = vm_ctx_len;
                                vm_ctx = NULL;
                            }
                            if (vm_ctx)
                                alloc->free(alloc->ctx, vm_ctx, vm_ctx_len + 1);
                        }
                    }
                }

                /* ── BTH: Time-of-day persona overlay (b1c late-night + b3b vulnerability) */
                {
                    const char *tod_overlay = NULL;
                    size_t tod_len = 0;
                    static const char HU_DEFAULT_TIME_OV_LATE_NIGHT[] =
                        "\nIt's late at night. You can be more relaxed, introspective, "
                        "slightly more open than during the day. If it feels natural, share "
                        "something personal or vulnerable. Late-night texts are more intimate.";
                    static const char HU_DEFAULT_TIME_OV_EARLY_MORNING[] =
                        "\nIt's early morning — you just woke up. Keep responses brief, "
                        "practical, slightly groggy. Short sentences. No deep philosophical "
                        "conversations yet.";
                    if (bth_hour >= 22 || (bth_hour >= 0 && bth_hour < 1)) {
                        tod_overlay =
                            (agent && agent->persona && agent->persona->time_overlay_late_night)
                                ? agent->persona->time_overlay_late_night
                                : HU_DEFAULT_TIME_OV_LATE_NIGHT;
                        tod_len =
                            (agent && agent->persona && agent->persona->time_overlay_late_night)
                                ? strlen(tod_overlay)
                                : sizeof(HU_DEFAULT_TIME_OV_LATE_NIGHT) - 1;
                    } else if (bth_hour >= 7 && bth_hour < 9) {
                        tod_overlay =
                            (agent && agent->persona && agent->persona->time_overlay_early_morning)
                                ? agent->persona->time_overlay_early_morning
                                : HU_DEFAULT_TIME_OV_EARLY_MORNING;
                        tod_len =
                            (agent && agent->persona && agent->persona->time_overlay_early_morning)
                                ? strlen(tod_overlay)
                                : sizeof(HU_DEFAULT_TIME_OV_EARLY_MORNING) - 1;
                    }
                    if (tod_overlay && tod_len > 0) {
                        if (convo_ctx) {
                            size_t total = convo_ctx_len + tod_len + 2;
                            char *merged = (char *)alloc->alloc(alloc->ctx, total);
                            if (merged) {
                                memcpy(merged, convo_ctx, convo_ctx_len);
                                merged[convo_ctx_len] = '\n';
                                memcpy(merged + convo_ctx_len + 1, tod_overlay, tod_len);
                                merged[total - 1] = '\0';
                                alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                convo_ctx = merged;
                                convo_ctx_len = total - 1;
                            }
                        } else {
                            convo_ctx = (char *)alloc->alloc(alloc->ctx, tod_len + 1);
                            if (convo_ctx) {
                                memcpy(convo_ctx, tod_overlay, tod_len);
                                convo_ctx[tod_len] = '\0';
                                convo_ctx_len = tod_len;
                            }
                        }
                    }
                }

                /* ── BTH Tier 2: Sentiment momentum (t2b) ─────────────────── */
                if (history_entries && history_count >= 3 && !llm_decides) {
                    char *sent_ctx = NULL;
                    size_t sent_ctx_len = 0;
                    sent_ctx = hu_conversation_build_sentiment_momentum(
                        alloc, history_entries, history_count, &sent_ctx_len);
                    if (sent_ctx && sent_ctx_len > 0) {
                        if (convo_ctx) {
                            size_t total = convo_ctx_len + sent_ctx_len + 2;
                            char *merged = (char *)alloc->alloc(alloc->ctx, total);
                            if (merged) {
                                memcpy(merged, convo_ctx, convo_ctx_len);
                                merged[convo_ctx_len] = '\n';
                                memcpy(merged + convo_ctx_len + 1, sent_ctx, sent_ctx_len);
                                merged[total - 1] = '\0';
                                alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                convo_ctx = merged;
                                convo_ctx_len = total - 1;
                            }
                        } else {
                            convo_ctx = sent_ctx;
                            convo_ctx_len = sent_ctx_len;
                            sent_ctx = NULL;
                        }
                        if (sent_ctx)
                            alloc->free(alloc->ctx, sent_ctx, sent_ctx_len + 1);
                    }
                }

                /* ── BTH Tier 2: Topic tangent callback (t2e) ─────────────── */
                if (history_entries && history_count >= 6 && !llm_decides) {
                    char *tang_ctx = NULL;
                    size_t tang_ctx_len = 0;
                    tang_ctx = hu_conversation_build_tangent_callback(
                        alloc, history_entries, history_count, (uint32_t)time(NULL), &tang_ctx_len);
                    if (tang_ctx && tang_ctx_len > 0) {
                        if (convo_ctx) {
                            size_t total = convo_ctx_len + tang_ctx_len + 2;
                            char *merged = (char *)alloc->alloc(alloc->ctx, total);
                            if (merged) {
                                memcpy(merged, convo_ctx, convo_ctx_len);
                                merged[convo_ctx_len] = '\n';
                                memcpy(merged + convo_ctx_len + 1, tang_ctx, tang_ctx_len);
                                merged[total - 1] = '\0';
                                alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                convo_ctx = merged;
                                convo_ctx_len = total - 1;
                            }
                        } else {
                            convo_ctx = tang_ctx;
                            convo_ctx_len = tang_ctx_len;
                            tang_ctx = NULL;
                        }
                        if (tang_ctx)
                            alloc->free(alloc->ctx, tang_ctx, tang_ctx_len + 1);
                    }
                }

                /* ── BTH Tier 3: Conversation depth signal (t3b) ──────────── */
                if (history_entries && history_count >= 5 && !llm_decides) {
                    char *depth_ctx = NULL;
                    size_t depth_ctx_len = 0;
                    depth_ctx = hu_conversation_build_depth_signal(alloc, history_entries,
                                                                   history_count, &depth_ctx_len);
                    if (depth_ctx && depth_ctx_len > 0) {
                        if (convo_ctx) {
                            size_t total = convo_ctx_len + depth_ctx_len + 2;
                            char *merged = (char *)alloc->alloc(alloc->ctx, total);
                            if (merged) {
                                memcpy(merged, convo_ctx, convo_ctx_len);
                                merged[convo_ctx_len] = '\n';
                                memcpy(merged + convo_ctx_len + 1, depth_ctx, depth_ctx_len);
                                merged[total - 1] = '\0';
                                alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                convo_ctx = merged;
                                convo_ctx_len = total - 1;
                            }
                        } else {
                            convo_ctx = depth_ctx;
                            convo_ctx_len = depth_ctx_len;
                            depth_ctx = NULL;
                        }
                        if (depth_ctx)
                            alloc->free(alloc->ctx, depth_ctx, depth_ctx_len + 1);
                    }
                }
#endif

                /* Cap conversation context to avoid overflowing the provider context window.
                 * 32 KB is ~8K tokens — leaves room for system prompt, history, and response. */
                if (convo_ctx && convo_ctx_len > 32768) {
                    convo_ctx[32768] = '\0';
                    convo_ctx_len = 32768;
                }

                /* Prepend group-chat instruction when responding in a group */
                if (msgs[batch_start].is_group) {
                    const char *hint = HU_GROUP_CHAT_PROMPT_HINT;
                    size_t hint_len = sizeof(HU_GROUP_CHAT_PROMPT_HINT) - 1;
                    size_t new_len = hint_len + (convo_ctx ? convo_ctx_len : 0);
                    char *new_ctx = (char *)alloc->alloc(alloc->ctx, new_len + 1);
                    if (new_ctx) {
                        memcpy(new_ctx, hint, hint_len);
                        if (convo_ctx && convo_ctx_len > 0)
                            memcpy(new_ctx + hint_len, convo_ctx, convo_ctx_len);
                        new_ctx[new_len] = '\0';
                        if (convo_ctx)
                            alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                        convo_ctx = new_ctx;
                        convo_ctx_len = new_len;
                    }
                }

                /* ── Situational context injections ─────────────────────────── */
#ifndef HU_IS_TEST
                {
                    char inject_buf[3072];
                    size_t inject_pos = 0;

                    /* Cold restart: detect >4h conversation gap */
                    if (history_entries && history_count > 0) {
                        size_t n = hu_conversation_build_cold_restart_hint(
                            history_entries, history_count, inject_buf + inject_pos,
                            sizeof(inject_buf) - inject_pos);
                        if (n > 0) {
                            inject_pos += n;
                            inject_buf[inject_pos++] = '\n';
                        }
                    }

                    /* Group chat: mention sender's name for natural addressing */
                    if (msgs[batch_start].is_group && combined_len > 0) {
                        const char *sender = msgs[batch_start].session_key;
                        size_t sender_len = strlen(sender);
                        /* Use first name or handle prefix before @ */
                        const char *at = memchr(sender, '@', sender_len);
                        size_t first_len = at ? (size_t)(at - sender) : sender_len;
                        if (first_len > 0 && first_len < 32) {
                            size_t n = hu_conversation_build_group_mention_hint(
                                sender, first_len, true, inject_buf + inject_pos,
                                sizeof(inject_buf) - inject_pos);
                            if (n > 0) {
                                inject_pos += n;
                                inject_buf[inject_pos++] = '\n';
                            }
                        }
                    }

                    /* Link awareness: detect URLs in their message */
                    if (combined_len > 0) {
                        size_t n = hu_conversation_build_link_context(
                            combined, combined_len, inject_buf + inject_pos,
                            sizeof(inject_buf) - inject_pos);
                        if (n > 0) {
                            inject_pos += n;
                            inject_buf[inject_pos++] = '\n';
                        }
                    }

                    /* Voice message awareness */
                    if (combined_len > 0 &&
                        (memmem(combined, combined_len, "[Voice Message]", 15) != NULL)) {
                        static const char vm_hint[] =
                            "[VOICE MESSAGE] They sent a voice message. React naturally — "
                            "\"just listened\" or respond to the likely content. "
                            "Don't say \"I can't listen to audio\".";
                        size_t vm_len = sizeof(vm_hint) - 1;
                        if (inject_pos + vm_len + 1 < sizeof(inject_buf)) {
                            memcpy(inject_buf + inject_pos, vm_hint, vm_len);
                            inject_pos += vm_len;
                            inject_buf[inject_pos++] = '\n';
                        }
                    }

                    /* Emoji frequency matching: mirror their emoji usage level */
                    if (history_entries && history_count >= 3) {
                        size_t n = hu_conversation_build_emoji_mirror_hint(
                            history_entries, history_count, inject_buf + inject_pos,
                            sizeof(inject_buf) - inject_pos);
                        if (n > 0) {
                            inject_pos += n;
                            inject_buf[inject_pos++] = '\n';
                        }
                    }

                    /* Reaction-to-their-reaction: detect tapbacks on our messages */
                    if (history_entries && history_count >= 2) {
                        size_t n = hu_conversation_build_reaction_received_hint(
                            history_entries, history_count, inject_buf + inject_pos,
                            sizeof(inject_buf) - inject_pos);
                        if (n > 0) {
                            inject_pos += n;
                            inject_buf[inject_pos++] = '\n';
                        }
                    }

                    /* Edit/unsend awareness: detect edited or unsent messages */
                    {
                        bool any_edited = false;
                        bool any_unsent = false;
                        for (size_t bi = batch_start; bi <= batch_end; bi++) {
                            if (msgs[bi].was_edited)
                                any_edited = true;
                            if (msgs[bi].was_unsent)
                                any_unsent = true;
                        }
                        if (any_unsent) {
                            size_t n = hu_conversation_build_edit_awareness_hint(
                                false, true, inject_buf + inject_pos,
                                sizeof(inject_buf) - inject_pos);
                            if (n > 0) {
                                inject_pos += n;
                                inject_buf[inject_pos++] = '\n';
                            }
                        } else if (any_edited) {
                            size_t n = hu_conversation_build_edit_awareness_hint(
                                true, false, inject_buf + inject_pos,
                                sizeof(inject_buf) - inject_pos);
                            if (n > 0) {
                                inject_pos += n;
                                inject_buf[inject_pos++] = '\n';
                            }
                        }
                    }

                    /* Inline reply awareness: look up the original message they replied to.
                     * Logic extracted into hu_imessage_build_inline_reply_hint_for_batch so
                     * the batch → lookup → hint composition can be unit-tested via
                     * hu_imessage_test_set_guid_lookup (see tests/test_imessage_extended.c). */
#ifdef HU_HAS_IMESSAGE
                    {
                        size_t n = hu_imessage_build_inline_reply_hint_for_batch(
                            alloc, &msgs[batch_start], (batch_end - batch_start + 1),
                            inject_buf + inject_pos, sizeof(inject_buf) - inject_pos);
                        if (n > 0) {
                            inject_pos += n;
                            inject_buf[inject_pos++] = '\n';
                        }
                    }
#endif

                    /* Contact photo context: if available, note it for richer understanding.
                     * The path is stored for potential vision-model multimodal prompting. */
                    if (sizeof(inject_buf) - inject_pos > 200) {
                        char photo_path[512];
                        size_t pp_len = hu_conversation_contact_photo_path(
                            batch_key, key_len, photo_path, sizeof(photo_path));
                        if (pp_len > 0) {
                            int n =
                                snprintf(inject_buf + inject_pos, sizeof(inject_buf) - inject_pos,
                                         "[CONTACT PHOTO] Profile photo on file at: %s\n"
                                         "You know what they look like. Reference appearance "
                                         "naturally if relevant (e.g. haircut, glasses).\n",
                                         photo_path);
                            if (n > 0 && (size_t)n < sizeof(inject_buf) - inject_pos)
                                inject_pos += (size_t)n;
                        }
                    }

                    /* Append all injections to convo_ctx */
                    if (inject_pos > 0) {
                        size_t new_len = inject_pos + (convo_ctx ? convo_ctx_len : 0);
                        char *merged = (char *)alloc->alloc(alloc->ctx, new_len + 1);
                        if (merged) {
                            memcpy(merged, inject_buf, inject_pos);
                            if (convo_ctx && convo_ctx_len > 0)
                                memcpy(merged + inject_pos, convo_ctx, convo_ctx_len);
                            merged[new_len] = '\0';
                            if (convo_ctx)
                                alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                            convo_ctx = merged;
                            convo_ctx_len = new_len;
                        }
                    }
                }
#endif

                /* SHIELD-005: Inbound crisis — force response and inject supportive context */
                if (inbound_crisis) {
                    action = HU_RESPONSE_FULL;
                    static const char crisis_directive[] =
                        "[CRISIS SUPPORT]: The user may be in distress. "
                        "Respond with empathy and care. Include crisis resources: "
                        "988 Suicide & Crisis Lifeline (call/text 988), "
                        "Crisis Text Line (text HOME to 741741). "
                        "Do not dismiss their feelings. Do not give advice. "
                        "Listen and validate.\n";
                    size_t cd_len = sizeof(crisis_directive) - 1;
                    size_t new_len = cd_len + convo_ctx_len;
                    char *merged = (char *)alloc->alloc(alloc->ctx, new_len + 1);
                    if (merged) {
                        memcpy(merged, crisis_directive, cd_len);
                        if (convo_ctx && convo_ctx_len > 0)
                            memcpy(merged + cd_len, convo_ctx, convo_ctx_len);
                        merged[new_len] = '\0';
                        if (convo_ctx)
                            alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                        convo_ctx = merged;
                        convo_ctx_len = new_len;
                    }
                }

                /* Set agent per-turn context fields (prompt builder reads these) */
                agent->contact_context = contact_ctx;
                agent->contact_context_len = contact_ctx_len;
                agent->conversation_context = convo_ctx;
                agent->conversation_context_len = convo_ctx_len;
                agent->ab_history_entries = history_entries;
                agent->ab_history_count = history_count;
                agent->max_response_chars = max_chars;

                /* Adaptive model selection: route to optimal model + thinking budget
                 * based on message content, relationship, and time of day */
#ifndef HU_IS_TEST
                if (config && config->agent.s3_local_model &&
                    !agent->sota.degradation_config.s3_local_model) {
                    agent->sota.degradation_config.s3_local_model = config->agent.s3_local_model;
                    agent->sota.degradation_config.s3_local_model_len =
                        strlen(config->agent.s3_local_model);
                }
                {
                    hu_model_router_config_t mr_cfg = hu_model_router_default_config();
                    if (config && config->agent.mr_reflexive_model) {
                        mr_cfg.reflexive_model = config->agent.mr_reflexive_model;
                        mr_cfg.reflexive_model_len = strlen(config->agent.mr_reflexive_model);
                    }
                    if (config && config->agent.mr_conversational_model) {
                        mr_cfg.conversational_model = config->agent.mr_conversational_model;
                        mr_cfg.conversational_model_len =
                            strlen(config->agent.mr_conversational_model);
                    }
                    if (config && config->agent.mr_analytical_model) {
                        mr_cfg.analytical_model = config->agent.mr_analytical_model;
                        mr_cfg.analytical_model_len = strlen(config->agent.mr_analytical_model);
                    }
                    if (config && config->agent.mr_deep_model) {
                        mr_cfg.deep_model = config->agent.mr_deep_model;
                        mr_cfg.deep_model_len = strlen(config->agent.mr_deep_model);
                    }
                    if (config && config->agent.mr_on_device_model) {
                        mr_cfg.on_device_model = config->agent.mr_on_device_model;
                        mr_cfg.on_device_model_len = strlen(config->agent.mr_on_device_model);
                    }
#ifdef HU_ENABLE_APPLE_INTELLIGENCE
                    if (!config || config->agent.mr_on_device_enabled) {
                        mr_cfg.on_device_available = hu_apple_probe(agent->alloc, NULL, 0);
                    }
#endif
                    const char *rel = NULL;
                    size_t rel_len = 0;
                    if (agent->persona) {
                        const hu_contact_profile_t *cp_mr =
                            hu_persona_find_contact(agent->persona, batch_key, key_len);
                        if (cp_mr && cp_mr->relationship) {
                            rel = cp_mr->relationship;
                            rel_len = strlen(cp_mr->relationship);
                        }
                    }
                    hu_model_selection_t sel;
                    if (config && config->agent.mr_judge_enabled && !llm_decides) {
                        static hu_route_cache_t judge_cache;
                        static bool judge_cache_inited = false;
                        if (!judge_cache_inited) {
                            hu_route_cache_init(&judge_cache);
                            judge_cache_inited = true;
                        }
                        const char *jm = mr_cfg.reflexive_model;
                        size_t jm_len = mr_cfg.reflexive_model_len;
                        if (config->agent.mr_judge_model) {
                            jm = config->agent.mr_judge_model;
                            jm_len = strlen(config->agent.mr_judge_model);
                        }
                        sel = hu_model_route_with_judge(
                            &mr_cfg, combined, combined_len, rel, rel_len, bth_hour, history_count,
                            &agent->provider, jm, jm_len, agent->alloc, &judge_cache);
                    } else {
                        sel = hu_model_route(&mr_cfg, combined, combined_len, rel, rel_len,
                                             bth_hour, history_count);
                    }
                    agent->turn_model = sel.model;
                    agent->turn_model_len = sel.model_len;
                    agent->turn_temperature = sel.temperature;
                    agent->turn_thinking_budget = sel.thinking_budget;
                    static const char *tier_names[] = {"reflexive", "conversational", "analytical",
                                                       "deep"};
                    hu_log_info("human", agent ? agent->observer : NULL,
                                "model route: %.*s (tier=%s, src=%s, thinking=%d) for %.*s",
                                (int)sel.model_len, sel.model,
                                tier_names[sel.tier < 4 ? sel.tier : 0],
                                hu_route_source_str(sel.source), sel.thinking_budget,
                                (int)(key_len > 20 ? 20 : key_len), batch_key);
                }
#endif

                /* Scope memory to this contact */
                agent->memory_session_id = batch_key;
                agent->memory_session_id_len = key_len;
                if (agent->memory && agent->memory->vtable) {
                    agent->memory->current_session_id = batch_key;
                    agent->memory->current_session_id_len = key_len;
                }

                /* F29: Backchannel — send brief cue and skip LLM when narrative detected.
                 * Flash typing briefly so the recipient sees "..." before the short
                 * reply appears, matching how a human types "mhm" or "haha". */
                if (use_backchannel && backchannel_len > 0 && ch->channel->vtable->send) {
                    if (ch->channel->vtable->start_typing) {
                        ch->channel->vtable->start_typing(ch->channel->ctx, batch_key, key_len);
                        unsigned int bc_delay_ms = 300 + (unsigned int)(rand() % 501);
                        struct timespec bc_ts = {.tv_sec = 0,
                                                 .tv_nsec = (long)bc_delay_ms * 1000000L};
                        nanosleep(&bc_ts, NULL);
                        turn_out_state.typing_started = true;
                    }
                    ch->channel->vtable->send(ch->channel->ctx, send_target, send_target_len,
                                              backchannel_buf, backchannel_len, NULL, 0);
                    hu_log_info("daemon", NULL, "backchannel sent (len=%zu)", backchannel_len);
                    goto skip_llm_this_batch;
                }

                /* Persona-based "reading" delay before typing — simulates the
                 * time a person spends reading the message before responding. */
                if (agent->persona && agent->persona->avg_response_time_sec > 0.0) {
                    double base_sec = agent->persona->avg_response_time_sec;
                    unsigned int jitter = (unsigned int)(rand() % 2000u);
                    unsigned int delay_ms = (unsigned int)(base_sec * 300.0) + jitter;
                    if (delay_ms > 8000)
                        delay_ms = 8000;
#ifndef HU_IS_TEST
                    hu_platform_sleep_ms(delay_ms);
#endif
                }

                /* Start typing indicator before LLM call */
                if (ch->channel->vtable->start_typing) {
                    ch->channel->vtable->start_typing(ch->channel->ctx, batch_key, key_len);
                }
                turn_out_state.typing_started = (ch->channel->vtable->start_typing != NULL);

                /* Thinking response: send filler first if message warrants it */
                {
                    hu_thinking_response_t thinking;
                    memset(&thinking, 0, sizeof(thinking));
                    const char *ch_name = ch->channel->vtable->name
                                              ? ch->channel->vtable->name(ch->channel->ctx)
                                              : NULL;
                    hu_thinking_context_t think_ctx = {
                        .persona = agent ? agent->persona : NULL,
                        .channel_name = ch_name,
                        .chat_id = send_target,
                        .chat_id_len = send_target_len,
                        .recency = agent ? &agent->filler_recency : NULL,
                        .seed = (uint32_t)time(NULL),
                    };
                    bool needs_thinking = hu_conversation_classify_thinking(
                        &think_ctx, combined, combined_len, history_entries, history_count,
                        &thinking);
                    if (needs_thinking && thinking.filler_len > 0 && ch->channel->vtable->send) {
                        ch->channel->vtable->send(ch->channel->ctx, send_target, send_target_len,
                                                  thinking.filler, thinking.filler_len, NULL, 0);
#ifndef HU_IS_TEST
                        if (agent && agent->bth_metrics)
                            agent->bth_metrics->thinking_responses++;
                        usleep((useconds_t)(thinking.delay_ms * 1000));
#endif
                    }
                }

                /* Tapback-vs-text decision: gate reaction and/or LLM flow */
                if (llm_decides) {
                    if (director_result_valid && director_result.action == DIR_SILENCE) {
                        bool has_question = memchr(combined, '?', combined_len) != NULL;
                        bool looks_like_greeting =
                            (combined_len < 30 &&
                             (strstr(combined, "hey") || strstr(combined, "Hey") ||
                              strstr(combined, "hi") || strstr(combined, "Hi") ||
                              strstr(combined, "yo") || strstr(combined, "Yo") ||
                              strstr(combined, "sup") || strstr(combined, "hello") ||
                              strstr(combined, "Hello") || strstr(combined, "what")));
                        if (has_question || looks_like_greeting) {
                            hu_log_info(
                                "human", agent ? agent->observer : NULL,
                                "director: silence overridden (greeting/question detected)");
                            director_result.action = DIR_TEXT;
                        } else {
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "director: silence (leave on read)");
                            goto skip_llm_this_batch;
                        }
                    }
                    if (director_result_valid && director_result.action == DIR_TAPBACK &&
                        ch->channel->vtable->react) {
                        int64_t msg_id = msgs[batch_end].message_id;
                        if (msg_id > 0 && director_result.reaction != HU_REACTION_NONE) {
                            /* Natural delay: humans see a message, then react 1-3s later */
                            unsigned int tapback_delay_ms = 1000 + (unsigned int)(rand() % 2001);
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "director: tapback reaction=%d msg_id=%lld "
                                        "(delay %ums)",
                                        (int)director_result.reaction, (long long)msg_id,
                                        tapback_delay_ms);
                            if (ch->channel->vtable->mark_read)
                                ch->channel->vtable->mark_read(ch->channel->ctx, batch_key,
                                                               key_len);
                            struct timespec ts = {.tv_sec = tapback_delay_ms / 1000,
                                                  .tv_nsec =
                                                      (long)(tapback_delay_ms % 1000) * 1000000L};
                            nanosleep(&ts, NULL);
                            hu_error_t react_err =
                                ch->channel->vtable->react(ch->channel->ctx, batch_key, key_len,
                                                           msg_id, director_result.reaction);
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "tapback result: %s", hu_error_string(react_err));
                            if (react_err == HU_OK) {
                                if (agent->bth_metrics)
                                    agent->bth_metrics->reactions_sent++;
                                goto skip_llm_this_batch;
                            }
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "tapback failed, falling back to text response");
                        }
                    }
                    hu_log_info("human", agent ? agent->observer : NULL,
                                "director: text response, sending to LLM");
                    goto llm_decides_skip_tapback;
                }
                {
                    hu_tapback_decision_t tapback_decision =
                        hu_conversation_classify_tapback_decision(
                            combined, combined_len, history_entries, history_count,
                            contact_for_tapback, (uint32_t)time(NULL));

                    if (tapback_decision == HU_NO_RESPONSE) {
                        hu_log_info("human", agent ? agent->observer : NULL,
                                    "tapback decision: no response for %.*s",
                                    (int)(combined_len > 40 ? 40 : combined_len), combined);
                        goto skip_llm_this_batch;
                    }

                    if (tapback_decision == HU_TAPBACK_ONLY && ch->channel->vtable->react) {
                        hu_reaction_type_t reaction = HU_REACTION_NONE;
                        const char *vision_desc = NULL;
                        size_t vision_desc_len = 0;
                        if (hu_conversation_extract_vision_description(
                                combined, combined_len, &vision_desc, &vision_desc_len) &&
                            vision_desc && vision_desc_len > 0) {
                            reaction = hu_conversation_classify_photo_reaction(
                                vision_desc, vision_desc_len, contact_for_tapback,
                                (uint32_t)time(NULL));
                        }
                        if (reaction == HU_REACTION_NONE) {
                            reaction = hu_conversation_classify_reaction(
                                combined, combined_len, false, history_entries, history_count,
                                (uint32_t)time(NULL));
                        }
                        bool tapback_sent = false;
                        if (reaction != HU_REACTION_NONE) {
                            int64_t msg_id = msgs[batch_end].message_id;
                            if (msg_id > 0) {
                                hu_error_t react_err = ch->channel->vtable->react(
                                    ch->channel->ctx, batch_key, key_len, msg_id, reaction);
                                if (react_err == HU_OK) {
                                    tapback_sent = true;
                                    if (agent->bth_metrics)
                                        agent->bth_metrics->reactions_sent++;
                                } else {
                                    hu_log_info(
                                        "human", agent ? agent->observer : NULL,
                                        "tapback-only react failed (%s), falling back to LLM",
                                        hu_error_string(react_err));
                                }
                            }
                        }
                        if (tapback_sent) {
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "tapback only (no text) for %.*s",
                                        (int)(combined_len > 40 ? 40 : combined_len), combined);
                            goto skip_llm_this_batch;
                        }
                    }

                    if (tapback_decision == HU_TAPBACK_AND_TEXT && ch->channel->vtable->react) {
                        hu_reaction_type_t reaction = HU_REACTION_NONE;
                        const char *vision_desc_txt = NULL;
                        size_t vision_desc_txt_len = 0;
                        if (hu_conversation_extract_vision_description(
                                combined, combined_len, &vision_desc_txt, &vision_desc_txt_len) &&
                            vision_desc_txt && vision_desc_txt_len > 0) {
                            reaction = hu_conversation_classify_photo_reaction(
                                vision_desc_txt, vision_desc_txt_len, contact_for_tapback,
                                (uint32_t)time(NULL));
                        }
                        if (reaction == HU_REACTION_NONE) {
                            reaction = hu_conversation_classify_reaction(
                                combined, combined_len, false, history_entries, history_count,
                                (uint32_t)time(NULL));
                        }
                        if (reaction != HU_REACTION_NONE) {
                            int64_t msg_id = msgs[batch_end].message_id;
                            if (msg_id > 0) {
                                hu_error_t react_err = ch->channel->vtable->react(
                                    ch->channel->ctx, batch_key, key_len, msg_id, reaction);
                                if (react_err == HU_OK) {
                                    if (agent->bth_metrics)
                                        agent->bth_metrics->reactions_sent++;
                                }
                            }
                        }
                    }
                }
#endif
            llm_decides_skip_tapback:;

            /* F45: Burst messaging — 3–4 rapid-fire thoughts for urgent/exciting context.
             * Skip in llm_decides mode — burst is an extra LLM call. */
#ifndef HU_IS_TEST
                if (!llm_decides) {
                    float burst_prob = 0.03f;
                    if (agent && agent->persona)
                        burst_prob = agent->persona->humanization.burst_message_probability;
                    uint32_t burst_seed =
                        (uint32_t)time(NULL) * 1103515245u + 12345u + (uint32_t)(uintptr_t)combined;
                    if (hu_conversation_should_burst(combined, combined_len, history_entries,
                                                     history_count, burst_seed, burst_prob)) {
                        char *burst_convo = NULL;
                        size_t burst_convo_len = 0;
                        char burst_buf[512];
                        size_t burst_len =
                            hu_conversation_build_burst_prompt(burst_buf, sizeof(burst_buf));
                        if (burst_len > 0) {
                            burst_convo_len = burst_len + 1 + (convo_ctx ? convo_ctx_len : 0) + 1;
                            burst_convo = (char *)alloc->alloc(alloc->ctx, burst_convo_len);
                            if (burst_convo) {
                                memcpy(burst_convo, burst_buf, burst_len);
                                burst_convo[burst_len] = '\n';
                                if (convo_ctx && convo_ctx_len > 0)
                                    memcpy(burst_convo + burst_len + 1, convo_ctx, convo_ctx_len);
                                burst_convo[burst_convo_len - 1] = '\0';
                                agent->conversation_context = burst_convo;
                                agent->conversation_context_len = burst_convo_len - 1;
                            }
                        }
                        if (ch->channel->vtable->start_typing)
                            ch->channel->vtable->start_typing(ch->channel->ctx, batch_key, key_len);
                        char *burst_response = NULL;
                        size_t burst_response_len = 0;
                        hu_error_t burst_err = hu_agent_turn(agent, combined, combined_len,
                                                             &burst_response, &burst_response_len);
                        if (ch->channel->vtable->stop_typing)
                            ch->channel->vtable->stop_typing(ch->channel->ctx, batch_key, key_len);
                        if (burst_err == HU_OK && burst_response && burst_response_len > 0 &&
                            ch->channel->vtable->send) {
                            char burst_msgs[4][256];
                            int n = hu_conversation_parse_burst_response(
                                burst_response, burst_response_len, burst_msgs, 4);
                            for (int bi = 0; bi < n; bi++) {
                                if (burst_msgs[bi][0]) {
                                    size_t bm_len = strlen(burst_msgs[bi]);
                                    hu_validator_chain_apply_default_in_place(
                                        alloc, agent ? agent->observer : NULL, NULL, 0, "burst msg",
                                        burst_msgs[bi], &bm_len, sizeof(burst_msgs[bi]));
                                    if (bm_len == 0)
                                        continue;
                                    bm_len = hu_conversation_vary_complexity(
                                        burst_msgs[bi], bm_len, burst_seed + (uint32_t)bi);
                                    if (bm_len > 1 && burst_msgs[bi][0] >= 'A' &&
                                        burst_msgs[bi][0] <= 'Z' && burst_msgs[bi][1] >= 'a' &&
                                        burst_msgs[bi][1] <= 'z' && burst_msgs[bi][0] != 'I') {
                                        burst_msgs[bi][0] = (char)(burst_msgs[bi][0] + 32);
                                    }
                                    if (bm_len > 1 && burst_msgs[bi][bm_len - 1] == '.') {
                                        burst_msgs[bi][bm_len - 1] = '\0';
                                        bm_len--;
                                    }
                                    ch->channel->vtable->send(ch->channel->ctx, send_target,
                                                              send_target_len, burst_msgs[bi],
                                                              bm_len, NULL, 0);
                                    if (bi < n - 1) {
                                        unsigned int delay_ms =
                                            1000u + (burst_seed + (uint32_t)bi) % 2000u;
                                        hu_platform_sleep_ms(delay_ms);
                                    }
                                }
                            }
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "burst: %d messages for %.*s", n,
                                        (int)(key_len > 20 ? 20 : key_len), batch_key);
                        }
                        if (burst_response)
                            agent->alloc->free(agent->alloc->ctx, burst_response,
                                               burst_response_len + 1);
                        if (burst_convo) {
                            alloc->free(alloc->ctx, burst_convo, burst_convo_len);
                            agent->conversation_context = convo_ctx;
                            agent->conversation_context_len = convo_ctx_len;
                        }
                        goto skip_llm_this_batch;
                    }
                }
#endif

                /* Humanness: silence intuition — skip LLM for messages that
                 * deserve presence, not a full response */
                {
                    hu_emotional_weight_t s_ew =
                        hu_emotional_weight_classify(combined, combined_len);
                    bool s_has_question = false;
                    for (size_t qi = 0; qi < combined_len; qi++) {
                        if (combined[qi] == '?') {
                            s_has_question = true;
                            break;
                        }
                    }
                    hu_silence_response_t s_resp =
                        hu_silence_intuit(combined, combined_len, s_ew,
                                          (uint32_t)agent->history_count, s_has_question);
                    if (s_resp == HU_SILENCE_ACTUAL_SILENCE) {
                        hu_log_info("human", agent ? agent->observer : NULL,
                                    "silence intuition: actual silence for %.*s",
                                    (int)(key_len > 20 ? 20 : key_len), batch_key);
                        goto skip_llm_this_batch;
                    }
                    if (s_resp == HU_SILENCE_PRESENCE_ONLY ||
                        s_resp == HU_SILENCE_BRIEF_ACKNOWLEDGE) {
                        size_t ack_len = 0;
                        char *ack = hu_silence_build_acknowledgment(alloc, s_resp, &ack_len);
                        if (ack && ack_len > 0 && ch->channel->vtable->send) {
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "silence intuition: \"%.*s\" for %.*s", (int)ack_len, ack,
                                        (int)(key_len > 20 ? 20 : key_len), batch_key);
                            ch->channel->vtable->send(ch->channel->ctx, send_target,
                                                      send_target_len, ack, ack_len, NULL, 0);
                            alloc->free(alloc->ctx, ack, ack_len + 1);
                            goto skip_llm_this_batch;
                        }
                    }
                }

                bool retried = false;
                char *turing_rejected_resp = NULL;
                size_t turing_rejected_len = 0;
                hu_log_info("human", agent ? agent->observer : NULL,
                            "calling agent turn for %.*s...", (int)(key_len > 20 ? 20 : key_len),
                            batch_key);
                do {
                    if (response) {
                        agent->alloc->free(agent->alloc->ctx, response, response_len + 1);
                        response = NULL;
                        response_len = 0;
                    }
                    daemon_out_bus_bridge.active_turn = &turn_out_state;
                    {
                        hu_daemon_stream_ctx_t stream_ctx;
                        memset(&stream_ctx, 0, sizeof(stream_ctx));
                        stream_ctx.bus = &daemon_outbound_bus;
                        stream_ctx.alloc = alloc;
                        if (agent->active_channel && agent->active_channel_len > 0) {
                            size_t nc = agent->active_channel_len < HU_BUS_CHANNEL_LEN - 1
                                            ? agent->active_channel_len
                                            : HU_BUS_CHANNEL_LEN - 1;
                            memcpy(stream_ctx.channel, agent->active_channel, nc);
                            stream_ctx.channel[nc] = '\0';
                        } else if (ch->channel->vtable->name) {
                            const char *cn = ch->channel->vtable->name(ch->channel->ctx);
                            if (cn) {
                                int scn =
                                    snprintf(stream_ctx.channel, HU_BUS_CHANNEL_LEN, "%s", cn);
                                (void)scn;
                            }
                        }
                        {
                            size_t ik = key_len < HU_BUS_ID_LEN - 1 ? key_len : HU_BUS_ID_LEN - 1;
                            memcpy(stream_ctx.id, batch_key, ik);
                            stream_ctx.id[ik] = '\0';
                        }
                        /* Inject director's scene direction into conversation context.
                         * The director call was made earlier (before delays) and the
                         * result is stored in director_result. */
                        if (llm_decides && director_result_valid &&
                            director_result.direction[0] != '\0') {
                            size_t dn_len = strlen(director_result.direction);
                            static const char dn_hdr[] =
                                "\n--- Scene Direction (this message only) ---\n";
                            static const char dn_tail[] = "\n";
                            size_t new_len = convo_ctx_len + sizeof(dn_hdr) - 1 + dn_len +
                                             sizeof(dn_tail) - 1 + 1;
                            char *new_convo = (char *)alloc->alloc(alloc->ctx, new_len);
                            if (new_convo) {
                                if (convo_ctx && convo_ctx_len > 0)
                                    memcpy(new_convo, convo_ctx, convo_ctx_len);
                                memcpy(new_convo + convo_ctx_len, dn_hdr, sizeof(dn_hdr) - 1);
                                memcpy(new_convo + convo_ctx_len + sizeof(dn_hdr) - 1,
                                       director_result.direction, dn_len);
                                memcpy(new_convo + convo_ctx_len + sizeof(dn_hdr) - 1 + dn_len,
                                       dn_tail, sizeof(dn_tail) - 1);
                                new_convo[new_len - 1] = '\0';
                                alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                convo_ctx = new_convo;
                                convo_ctx_len = new_len - 1;
                                agent->conversation_context = convo_ctx;
                                agent->conversation_context_len = convo_ctx_len;
                            }
                        }

                        size_t saved_tools = 0;
                        size_t saved_specs = 0;
                        if (llm_decides) {
                            saved_tools = agent->tools_count;
                            saved_specs = agent->tool_specs_count;
                            agent->tools_count = 0;
                            agent->tool_specs_count = 0;
                        }
                        if (!retried) {
                            err = hu_agent_turn_stream_v2(agent, combined, combined_len,
                                                          daemon_stream_event_cb, &stream_ctx,
                                                          &response, &response_len);
                        } else {
                            err = hu_agent_turn(agent, combined, combined_len, &response,
                                                &response_len);
                        }
                        if (llm_decides) {
                            agent->tools_count = saved_tools;
                            agent->tool_specs_count = saved_specs;
                        }
                    }
                    daemon_out_bus_bridge.active_turn = NULL;
                    hu_log_info("human", agent ? agent->observer : NULL,
                                "agent turn result: err=%s response_len=%zu for %.*s",
                                hu_error_string(err), response_len,
                                (int)(key_len > 20 ? 20 : key_len), batch_key);
                    if (err == HU_OK && (!response || response_len == 0)) {
                        g_empty_agent_response_streak++;
                        hu_log_error(
                            "human", agent ? agent->observer : NULL,
                            "empty assistant response (consecutive=%u) for %.*s — check MLX "
                            "server, response_guard retry, and cloud fallback logs",
                            g_empty_agent_response_streak, (int)key_len, batch_key);
                        if (g_empty_agent_response_streak >= 3U)
                            hu_log_error(
                                "human", agent ? agent->observer : NULL,
                                "ALERT: 3+ consecutive empty assistant responses; channel may be "
                                "silent to users");
                    } else if (err == HU_OK && response_len > 0) {
                        g_empty_agent_response_streak = 0;
                    }
                    /* Hex dump first 80 bytes of response for encoding diagnostics */
                    if (err == HU_OK && response && response_len > 0) {
                        char hex[256];
                        size_t hlen = 0;
                        size_t dump_n = response_len < 80 ? response_len : 80;
                        for (size_t hi = 0; hi < dump_n && hlen + 3 < sizeof(hex); hi++) {
                            static const char hx[] = "0123456789abcdef";
                            unsigned char rb = (unsigned char)response[hi];
                            hex[hlen++] = hx[rb >> 4];
                            hex[hlen++] = hx[rb & 0xf];
                            hex[hlen++] = ' ';
                        }
                        hex[hlen] = '\0';
                        hu_log_info("human", agent ? agent->observer : NULL,
                                    "response hex[0..%zu]: %s", dump_n, hex);
                    }
                    if (err != HU_OK)
                        hu_log_error("human", agent ? agent->observer : NULL,
                                     "agent turn failed for %.*s: %s", (int)key_len, batch_key,
                                     hu_error_string(err));

                    /* W14 counterfactual rehearsal — enqueue at most once
                     * per hour for the active contact after a successful
                     * turn. Counterfactual rehearsal walks recent
                     * episodic memory for the contact and synthesises
                     * "what could have been said differently?" pairs that
                     * the W13 learner uses to refine the persona's tone
                     * without needing real conversational data. The
                     * scheduler tick (60s cadence) gates dispatch on
                     * idle/AC; we just enqueue. The static rate limit is
                     * intentionally global, not per-contact, to keep the
                     * queue from filling under bursts; per-contact
                     * fairness is handled by the scheduler's job-kind
                     * priority (see scheduler.c). */
                    if (err == HU_OK && agent && agent->w14_scheduler && batch_key && key_len > 0) {
                        static int64_t last_cf_enqueue_ms = 0;
                        int64_t now_ms = (int64_t)time(NULL) * 1000LL;
                        if (last_cf_enqueue_ms == 0 || now_ms - last_cf_enqueue_ms >= 3600000LL) {
                            hu_error_t cf_err = hu_w14_scheduler_enqueue_counterfactual(
                                agent->w14_scheduler, batch_key, key_len, 50 /* budget_ms */);
                            if (cf_err == HU_OK) {
                                last_cf_enqueue_ms = now_ms;
                                hu_log_info("human", agent->observer,
                                            "w14: enqueued counterfactual rehearsal for %.*s",
                                            (int)(key_len > 32 ? 32 : key_len), batch_key);
                            }
                            /* enqueue failures are silent — the next
                             * eligible turn will retry naturally */
                        }
                    }

                    /* Best-of-N: generate additional candidates, score with Turing heuristic */
                    if (err == HU_OK && response && response_len > 0 && !retried && config &&
                        config->agent.best_of_n >= 2 && !llm_decides) {
                        uint32_t n_extra = config->agent.best_of_n - 1;
                        if (n_extra > 4)
                            n_extra = 4;
                        hu_turing_score_t best_ts;
                        hu_turing_score_heuristic(response, response_len, combined, combined_len,
                                                  max_chars, &best_ts);
                        if (agent->active_channel)
                            hu_turing_apply_channel_weights(&best_ts, agent->active_channel,
                                                            agent->active_channel_len);
                        if (agent->persona && agent->persona->traits_count > 0)
                            hu_turing_apply_persona_alignment(
                                &best_ts, response, response_len,
                                (const char *const *)agent->persona->traits,
                                agent->persona->traits_count,
                                (const char *const *)agent->persona->preferred_vocab,
                                agent->persona->preferred_vocab_count,
                                (const char *const *)agent->persona->avoided_vocab,
                                agent->persona->avoided_vocab_count);
                        int best_score = best_ts.overall;
                        double orig_temp = agent->turn_temperature;
                        for (uint32_t ci = 0; ci < n_extra; ci++) {
                            agent->turn_temperature =
                                (orig_temp > 0.0 ? orig_temp : agent->temperature) +
                                0.1 * (double)(ci + 1);
                            if (agent->turn_temperature > 1.5)
                                agent->turn_temperature = 1.5;
                            char *cand = NULL;
                            size_t cand_len = 0;
                            hu_agent_clear_history(agent);
                            agent->proactive_turn = true;
                            hu_error_t cerr =
                                hu_agent_turn(agent, combined, combined_len, &cand, &cand_len);
                            agent->proactive_turn = false;
                            if (cerr != HU_OK || !cand || cand_len == 0) {
                                if (cand)
                                    alloc->free(alloc->ctx, cand, cand_len + 1);
                                continue;
                            }
                            hu_turing_score_t cand_ts;
                            hu_turing_score_heuristic(cand, cand_len, combined, combined_len,
                                                      max_chars, &cand_ts);
                            if (agent->active_channel)
                                hu_turing_apply_channel_weights(&cand_ts, agent->active_channel,
                                                                agent->active_channel_len);
                            if (agent->persona && agent->persona->traits_count > 0)
                                hu_turing_apply_persona_alignment(
                                    &cand_ts, cand, cand_len,
                                    (const char *const *)agent->persona->traits,
                                    agent->persona->traits_count,
                                    (const char *const *)agent->persona->preferred_vocab,
                                    agent->persona->preferred_vocab_count,
                                    (const char *const *)agent->persona->avoided_vocab,
                                    agent->persona->avoided_vocab_count);
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "bon candidate %u/%u: score=%d (best=%d)", ci + 2,
                                        config->agent.best_of_n, cand_ts.overall, best_score);
                            if (cand_ts.overall > best_score) {
                                hu_dpo_record_from_retry(&agent->sota.dpo_collector, combined,
                                                         combined_len, response, response_len, cand,
                                                         cand_len);
                                alloc->free(alloc->ctx, response, response_len + 1);
                                response = cand;
                                response_len = cand_len;
                                best_score = cand_ts.overall;
                            } else {
                                hu_dpo_record_from_retry(&agent->sota.dpo_collector, combined,
                                                         combined_len, cand, cand_len, response,
                                                         response_len);
                                alloc->free(alloc->ctx, cand, cand_len + 1);
                            }
                        }
                        agent->turn_temperature = orig_temp;
                    }

#ifndef HU_IS_TEST
                    if (err != HU_OK) {
                        if (ch->channel->vtable->stop_typing) {
                            ch->channel->vtable->stop_typing(ch->channel->ctx, batch_key, key_len);
                        }
                    }
                    if (err == HU_OK && (!response || response_len == 0)) {
                        if (ch->channel->vtable->stop_typing) {
                            ch->channel->vtable->stop_typing(ch->channel->ctx, batch_key, key_len);
                        }
                    }

                    /* Persona constitutional critique — skip in llm_decides mode
                     * (director already provides quality guidance) */
                    if (err == HU_OK && response && response_len > 0 && !retried &&
                        agent->constitutional_enabled && !llm_decides) {
                        hu_constitutional_config_t persona_cfg = hu_constitutional_config_persona();
                        hu_critique_result_t cr = {0};
                        hu_error_t ce = hu_constitutional_critique(
                            alloc, &agent->provider, agent->model_name, agent->model_name_len,
                            combined, combined_len, response, response_len, &persona_cfg, &cr);
                        if (ce == HU_OK && cr.verdict == HU_CRITIQUE_REWRITE &&
                            cr.revised_response && cr.revised_response_len > 0) {
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "constitutional rewrite (principle=%d): %.*s",
                                        cr.principle_index,
                                        (int)(cr.reasoning_len > 80 ? 80 : cr.reasoning_len),
                                        cr.reasoning ? cr.reasoning : "");
                            hu_dpo_record_from_retry(&agent->sota.dpo_collector, combined,
                                                     combined_len, response, response_len,
                                                     cr.revised_response, cr.revised_response_len);
                            alloc->free(alloc->ctx, response, response_len + 1);
                            response = cr.revised_response;
                            response_len = cr.revised_response_len;
                            cr.revised_response = NULL;
                        }
                        hu_critique_result_free(alloc, &cr);
                    }

                    /* AI-tell filter: catch known robotic phrases and force retry.
                     * Skip retry in llm_decides mode (too expensive with local model). */
                    if (err == HU_OK && response && response_len > 0 && !retried && !llm_decides) {
                        static const char *ai_tells[] = {
                            "I understand how you",
                            "I am here to support",
                            "I am here for you",
                            "that must be really",
                            "I appreciate you sharing",
                            "feel free to",
                            "I hear you",
                            "I'd be happy to",
                            "sorry to hear",
                            "going through that",
                            "here to support",
                            "I can only imagine",
                            "According to the available",
                            "According to my",
                            "significant negative impact",
                            "fail to account for",
                        };
                        bool has_ai_tell = false;
                        for (size_t ati = 0; ati < sizeof(ai_tells) / sizeof(ai_tells[0]); ati++) {
                            if (hu_strcasestr(response, ai_tells[ati])) {
                                has_ai_tell = true;
                                hu_log_info("human", agent ? agent->observer : NULL,
                                            "ai-tell detected: \"%s\" in response", ai_tells[ati]);
                                break;
                            }
                        }
                        if (has_ai_tell) {
                            retried = true;
                            agent->alloc->free(agent->alloc->ctx, response, response_len + 1);
                            response = NULL;
                            response_len = 0;
                            if (convo_ctx) {
                                static const char tell_hint[] =
                                    "[CRITICAL OVERRIDE: Your response was REJECTED because it "
                                    "sounded like a therapy chatbot. You MUST respond in 3-8 "
                                    "words MAXIMUM. Pick ONE of these patterns: "
                                    "'damn I'm sorry', 'ugh that's the worst', "
                                    "'yeah I've been there too', 'that's rough'. "
                                    "DO NOT use 'I understand', 'going through', 'sorry to hear', "
                                    "'here for you'. Be BRIEF. Be a FRIEND not a counselor.]";
                                size_t new_len = sizeof(tell_hint) - 1 + 1 + convo_ctx_len + 1;
                                char *new_convo = (char *)alloc->alloc(alloc->ctx, new_len);
                                if (new_convo) {
                                    memcpy(new_convo, tell_hint, sizeof(tell_hint) - 1);
                                    new_convo[sizeof(tell_hint) - 1] = '\n';
                                    memcpy(new_convo + sizeof(tell_hint), convo_ctx, convo_ctx_len);
                                    new_convo[new_len - 1] = '\0';
                                    alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                    convo_ctx = new_convo;
                                    convo_ctx_len = new_len - 1;
                                    agent->conversation_context = convo_ctx;
                                    agent->conversation_context_len = convo_ctx_len;
                                }
                            }
                            if (ch->channel->vtable->start_typing)
                                ch->channel->vtable->start_typing(ch->channel->ctx, batch_key,
                                                                  key_len);
                            if (ch->channel->vtable->stop_typing) {
                                ch->channel->vtable->stop_typing(ch->channel->ctx, batch_key,
                                                                 key_len);
                            }
                            continue;
                        }
                    }

                    /* Quality gate: check response for unnatural patterns.
                     * If needs_revision, retry once with hint.
                     * Skip retry in llm_decides mode (director handles quality). */
                    if (err == HU_OK && response && response_len > 0 && history_entries &&
                        !llm_decides) {
                        hu_quality_score_t qscore = hu_conversation_evaluate_quality(
                            response, response_len, history_entries, history_count, max_chars);
                        if (qscore.needs_revision && !retried) {
                            retried = true;
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "quality retry: score=%d (b=%d v=%d w=%d n=%d) "
                                        "for %.40s...\n",
                                        qscore.total, qscore.brevity, qscore.validation,
                                        qscore.warmth, qscore.naturalness,
                                        response_len > 40 ? response : response);
                            agent->alloc->free(agent->alloc->ctx, response, response_len + 1);
                            response = NULL;
                            response_len = 0;
                            /* Prepend data-driven retry hint from quality score */
                            if (convo_ctx) {
                                const char *hint =
                                    qscore.guidance[0] != '\0'
                                        ? qscore.guidance
                                        : (qscore.brevity < 10
                                               ? "Your response was much longer than their "
                                                 "messages. Match their energy."
                                               : (qscore.warmth < 10
                                                      ? "Your response felt distant. Show you care."
                                                      : "Your phrasing felt formal. Drop the "
                                                        "formality."));
                                size_t hint_len = strlen(hint);
                                size_t new_len = hint_len + 1 + convo_ctx_len + 1;
                                char *new_convo = (char *)alloc->alloc(alloc->ctx, new_len);
                                if (new_convo) {
                                    memcpy(new_convo, hint, hint_len);
                                    new_convo[hint_len] = '\n';
                                    memcpy(new_convo + hint_len + 1, convo_ctx, convo_ctx_len);
                                    new_convo[new_len - 1] = '\0';
                                    alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                    convo_ctx = new_convo;
                                    convo_ctx_len = new_len - 1;
                                    agent->conversation_context = convo_ctx;
                                    agent->conversation_context_len = convo_ctx_len;
                                }
                            }
                            if (ch->channel->vtable->start_typing)
                                ch->channel->vtable->start_typing(ch->channel->ctx, batch_key,
                                                                  key_len);
                            if (ch->channel->vtable->stop_typing) {
                                ch->channel->vtable->stop_typing(ch->channel->ctx, batch_key,
                                                                 key_len);
                            }
                            continue;
                        } else if (qscore.needs_revision) {
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "quality warning: score=%d (b=%d v=%d w=%d n=%d) "
                                        "for %.40s...\n",
                                        qscore.total, qscore.brevity, qscore.validation,
                                        qscore.warmth, qscore.naturalness,
                                        response_len > 40 ? response : response);
                        }
                    }

                    /* Turing score gate: retry if heuristic score is too low.
                     * Only fires once (shares retried flag with quality gate). */
                    if (err == HU_OK && response && response_len > 0 && !retried && !llm_decides) {
                        hu_turing_score_t pre_tscore;
                        hu_error_t pre_ts_err = hu_turing_score_heuristic(
                            response, response_len, combined, combined_len, max_chars, &pre_tscore);
                        if (pre_ts_err == HU_OK && agent->active_channel)
                            hu_turing_apply_channel_weights(&pre_tscore, agent->active_channel,
                                                            agent->active_channel_len);
                        if (pre_ts_err == HU_OK && agent->persona &&
                            agent->persona->traits_count > 0)
                            hu_turing_apply_persona_alignment(
                                &pre_tscore, response, response_len,
                                (const char *const *)agent->persona->traits,
                                agent->persona->traits_count,
                                (const char *const *)agent->persona->preferred_vocab,
                                agent->persona->preferred_vocab_count,
                                (const char *const *)agent->persona->avoided_vocab,
                                agent->persona->avoided_vocab_count);
                        if (pre_ts_err == HU_OK && pre_tscore.overall < 6) {
                            retried = true;
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "turing retry: %d/10 [%s] for %.40s...", pre_tscore.overall,
                                        hu_turing_verdict_name(pre_tscore.verdict),
                                        response_len > 40 ? response : response);
                            /* Build targeted hint from weakest dimensions */
                            const char *turing_hint =
                                (pre_tscore.dimensions[HU_TURING_NON_ROBOTIC] < 5)
                                    ? "Your response sounds like a chatbot. Drop formal "
                                      "phrasing, use contractions, be casual."
                                : (pre_tscore.dimensions[HU_TURING_ENERGY_MATCHING] < 5)
                                    ? "Your response energy doesn't match theirs. Mirror "
                                      "their vibe — if short, be short."
                                : (pre_tscore.dimensions[HU_TURING_GENUINE_WARMTH] < 5)
                                    ? "Your response feels cold. Show you actually care "
                                      "about them specifically."
                                : (pre_tscore.dimensions[HU_TURING_VULNERABILITY_WILLINGNESS] < 5)
                                    ? "Your response feels guarded. Be more real — share "
                                      "what you actually think or feel."
                                    : "Your response feels unnatural. Text like a real "
                                      "human friend would.";
                            /* DPO: save rejected response for pairing after retry */
                            if (turing_rejected_resp)
                                alloc->free(alloc->ctx, turing_rejected_resp,
                                            turing_rejected_len + 1);
                            turing_rejected_resp = hu_strndup(alloc, response, response_len);
                            turing_rejected_len = turing_rejected_resp ? response_len : 0;
                            agent->alloc->free(agent->alloc->ctx, response, response_len + 1);
                            response = NULL;
                            response_len = 0;
                            if (convo_ctx) {
                                size_t hint_len = strlen(turing_hint);
                                size_t new_len = hint_len + 1 + convo_ctx_len + 1;
                                char *new_convo = (char *)alloc->alloc(alloc->ctx, new_len);
                                if (new_convo) {
                                    memcpy(new_convo, turing_hint, hint_len);
                                    new_convo[hint_len] = '\n';
                                    memcpy(new_convo + hint_len + 1, convo_ctx, convo_ctx_len);
                                    new_convo[new_len - 1] = '\0';
                                    alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                    convo_ctx = new_convo;
                                    convo_ctx_len = new_len - 1;
                                    agent->conversation_context = convo_ctx;
                                    agent->conversation_context_len = convo_ctx_len;
                                }
                            }
                            if (ch->channel->vtable->start_typing)
                                ch->channel->vtable->start_typing(ch->channel->ctx, batch_key,
                                                                  key_len);
                            continue;
                        }
                    }
                    /* LLM judge gate for contacts with consistently low scores.
                     * Skip entirely in llm_decides mode (director handles quality,
                     * retries are too expensive with local model). */
#ifdef HU_ENABLE_SQLITE
                    if (err == HU_OK && response && response_len > 0 && !retried && !llm_decides &&
                        agent->memory) {
                        sqlite3 *ljdb = hu_sqlite_memory_get_db(agent->memory);
                        if (ljdb) {
                            int contact_dims[HU_TURING_DIM_COUNT];
                            if (hu_turing_get_contact_dimensions(ljdb, batch_key, key_len,
                                                                 contact_dims) == HU_OK) {
                                int csum = 0, ccnt = 0;
                                for (int d = 0; d < HU_TURING_DIM_COUNT; d++) {
                                    if (contact_dims[d] > 0) {
                                        csum += contact_dims[d];
                                        ccnt++;
                                    }
                                }
                                bool high_stakes = (ccnt > 0 && csum / ccnt < 7);
                                hu_provider_t *judge_prov = (llm_decides && g_classify_provider_ok)
                                                                ? &g_classify_provider
                                                                : &agent->provider;
                                const char *judge_model = (llm_decides && g_classify_provider_ok)
                                                              ? g_classify_model
                                                              : agent->model_name;
                                size_t judge_model_len = (llm_decides && g_classify_provider_ok)
                                                             ? g_classify_model_len
                                                             : agent->model_name_len;
                                if (high_stakes && judge_prov->vtable && judge_prov->vtable->chat) {
                                    hu_turing_score_t llm_tscore;
                                    hu_error_t llm_terr = hu_turing_score_llm(
                                        alloc, judge_prov, judge_model, judge_model_len, response,
                                        response_len, combined, combined_len, &llm_tscore);
                                    if (llm_terr == HU_OK && llm_tscore.overall < 6) {
                                        retried = true;
                                        hu_log_info("human", agent ? agent->observer : NULL,
                                                    "llm judge retry: %d/10 for %.*s",
                                                    llm_tscore.overall,
                                                    (int)(key_len > 20 ? 20 : key_len), batch_key);
                                        hu_dpo_record_from_feedback(&agent->sota.dpo_collector,
                                                                    combined, combined_len,
                                                                    response, response_len, false);
                                        agent->alloc->free(agent->alloc->ctx, response,
                                                           response_len + 1);
                                        response = NULL;
                                        response_len = 0;
                                        if (convo_ctx) {
                                            static const char lj_hint[] =
                                                "The LLM judge flagged your response as "
                                                "sounding artificial. Be authentically human.";
                                            size_t lj_len = sizeof(lj_hint) - 1;
                                            size_t new_len = lj_len + 1 + convo_ctx_len + 1;
                                            char *new_convo =
                                                (char *)alloc->alloc(alloc->ctx, new_len);
                                            if (new_convo) {
                                                memcpy(new_convo, lj_hint, lj_len);
                                                new_convo[lj_len] = '\n';
                                                memcpy(new_convo + lj_len + 1, convo_ctx,
                                                       convo_ctx_len);
                                                new_convo[new_len - 1] = '\0';
                                                alloc->free(alloc->ctx, convo_ctx,
                                                            convo_ctx_len + 1);
                                                convo_ctx = new_convo;
                                                convo_ctx_len = new_len - 1;
                                                agent->conversation_context = convo_ctx;
                                                agent->conversation_context_len = convo_ctx_len;
                                            }
                                        }
                                        if (ch->channel->vtable->stop_typing) {
                                            ch->channel->vtable->stop_typing(ch->channel->ctx,
                                                                             batch_key, key_len);
                                        }
                                        continue;
                                    }
                                }
                            }
                        }
                    }
#endif

                    break;
                } while (1);

                /* DPO: pair rejected response from Turing retry with chosen retry result */
                if (turing_rejected_resp && turing_rejected_len > 0 && response &&
                    response_len > 0) {
                    hu_dpo_record_from_retry(&agent->sota.dpo_collector, combined, combined_len,
                                             turing_rejected_resp, turing_rejected_len, response,
                                             response_len);
                }
                if (turing_rejected_resp) {
                    alloc->free(alloc->ctx, turing_rejected_resp, turing_rejected_len + 1);
                    turing_rejected_resp = NULL;
                }

                /* Text naturalizer: lowercase first char, strip trailing period */
                if (err == HU_OK && response && response_len > 0) {
                    if (response_len > 1 && response[0] >= 'A' && response[0] <= 'Z' &&
                        response[1] >= 'a' && response[1] <= 'z' && response[0] != 'I') {
                        response[0] = (char)(response[0] + 32);
                    }
                    if (response_len > 1 && response[response_len - 1] == '.') {
                        response[response_len - 1] = '\0';
                        response_len--;
                    }
                }

                /* Replay learning: analyze conversation and store insights for future prompts.
                 * Skip in llm_decides mode — post-turn analysis is too slow. */
                if (history_entries && history_count > 0 && !llm_decides) {
                    hu_replay_result_t replay = {0};
                    hu_error_t rerr =
                        hu_replay_analyze(alloc, history_entries, history_count, 2000, &replay);
                    if (rerr == HU_OK) {
#ifndef HU_IS_TEST
                        if (agent->bth_metrics)
                            agent->bth_metrics->replay_analyses++;
#endif
                        size_t rctx_len = 0;
                        char *rctx = hu_replay_build_context(alloc, &replay, &rctx_len);
                        if (rctx && rctx_len > 0) {
                            /* 2026-05-16: removed the unconditional mirror of
                             * rctx into the process-global `replay_insights`
                             * static buffer.  That buffer was a cross-contact
                             * leak channel.  The per-contact memory-store
                             * write below is the only durable path. */
                            if (history_count > 2 && agent->memory && agent->memory->vtable &&
                                agent->memory->vtable->store) {
                                static const char cat_name[] = "replay_insights";
                                hu_memory_category_t cat = {
                                    .tag = HU_MEMORY_CATEGORY_CUSTOM,
                                    .data.custom = {.name = cat_name,
                                                    .name_len = sizeof(cat_name) - 1},
                                };
                                /* Per-contact key; pairs with the per-contact
                                 * read at the F25 prompt-assembly site above. */
                                char ri_key[320];
                                size_t ri_key_len = 0;
                                if (hu_proactive_build_replay_key(batch_key, key_len, ri_key,
                                                                  sizeof(ri_key), &ri_key_len)) {
                                    agent->memory->vtable->store(agent->memory->ctx, ri_key,
                                                                 ri_key_len, rctx, rctx_len, &cat,
                                                                 batch_key, key_len);
                                }
                            }
                        }
                        if (rctx)
                            alloc->free(alloc->ctx, rctx, rctx_len + 1);
                    }
                    hu_replay_result_deinit(&replay, alloc);
                }
#endif

                /* ── Phase 3 post-turn: inner thought accumulation ──────── */
                if (err == HU_OK && inner_thought_store_ok && combined_len > 0 && batch_key &&
                    key_len > 0) {
                    /* P2-3 (2026-05-16): NEVER memcpy raw user text here.
                     * The inner_thought.topic surfaces in the system prompt
                     * as "[Inner thought: ...]" — a prompt-injection vector
                     * AND a verbatim-text leak. Extract a clean noun phrase
                     * first; if extraction yields nothing, SKIP. */
                    char it_topic[128] = {0};
                    size_t it_topic_len = hu_conversation_extract_topic(combined, combined_len,
                                                                        it_topic, sizeof(it_topic));
                    if (it_topic_len > 0) {
                        uint64_t it_now_ms = (uint64_t)time(NULL) * 1000ULL;
                        (void)hu_inner_thought_accumulate(&inner_thought_store, batch_key, key_len,
                                                          it_topic, it_topic_len, it_topic,
                                                          it_topic_len, 0.5, it_now_ms);
                    }
                }

                /* ── Phase 3 post-turn: humor audience tracking ──────── */
#ifdef HU_ENABLE_SQLITE
                if (err == HU_OK && response && response_len > 0 && agent->memory &&
                    agent->persona && agent->persona->humor.type) {
                    sqlite3 *ha_db = hu_sqlite_memory_get_db(agent->memory);
                    if (ha_db) {
                        /* Check if user's NEXT message signals humor failure */
                        bool humor_failed = hu_humor_detect_failure(combined, combined_len);
                        hu_humor_audience_t ha_cur = {0};
                        (void)hu_humor_audience_load(ha_db, batch_key, &ha_cur);
                        hu_humor_type_t pref = hu_humor_audience_preferred_type(&ha_cur);
                        (void)hu_humor_audience_record(ha_db, batch_key, pref, !humor_failed);
                    }
                }
#endif

                /* ── Phase 3 post-turn: increment turn counter for anti-sycophancy ── */
                daemon_turn_counter++;

                /* ── Phase 4 post-turn: style drift self-tracking ─────── */
#ifdef HU_ENABLE_SQLITE
                if (err == HU_OK && response && response_len > 0 && agent->memory && !llm_decides)
                    (void)hu_style_fingerprint_update_self(agent->memory, alloc, response,
                                                           response_len);
#endif

                /* ── Phase 4 post-turn: voice vulnerability tracking ──── */
                if (err == HU_OK && combined_len > 0 && agent->persona) {
                    float vuln = hu_voice_vulnerability_from_content(combined, combined_len);
                    if (vuln > 0.0f) {
                        float cur = agent->persona->voice.vulnerability_level;
                        cur = cur * 0.7f + vuln * 0.3f;
                        if (cur > 1.0f)
                            cur = 1.0f;
                        agent->persona->voice.vulnerability_level = cur;
                    }
                }

                /* ── Phase 4 post-turn: conversation repair detection ─── */
                if (err == HU_OK && combined_len > 0) {
                    hu_repair_signal_t rs = {0};
                    if (hu_repair_detect(combined, combined_len, &rs) == HU_OK &&
                        rs.should_acknowledge)
                        repair_signal = rs;
                }

                /* ── BTH post-turn: Theory of Mind record (t1b-post) ──────── */
#ifndef HU_IS_TEST
                if (err == HU_OK && response && response_len > 0 && !llm_decides) {
                    size_t tom_idx = (size_t)-1;
                    for (size_t ti = 0; ti < tom_contact_count; ti++) {
                        if (strncmp(tom_contact_keys[ti], batch_key, key_len) == 0 &&
                            tom_contact_keys[ti][key_len] == '\0') {
                            tom_idx = ti;
                            break;
                        }
                    }
                    if (tom_idx != (size_t)-1) {
                        hu_fc_result_t fc_tom;
                        memset(&fc_tom, 0, sizeof(fc_tom));
                        (void)hu_fast_capture(alloc, combined, combined_len, &fc_tom);
                        if (fc_tom.primary_topic && fc_tom.primary_topic[0]) {
                            (void)hu_tom_record_belief(
                                &tom_states[tom_idx], alloc, fc_tom.primary_topic,
                                strlen(fc_tom.primary_topic), HU_BELIEF_KNOWS, 0.8f);
                        }
                        for (size_t ei = 0; ei < fc_tom.entity_count && ei < 3; ei++) {
                            if (fc_tom.entities[ei].name[0]) {
                                (void)hu_tom_record_belief(
                                    &tom_states[tom_idx], alloc, fc_tom.entities[ei].name,
                                    strlen(fc_tom.entities[ei].name), HU_BELIEF_KNOWS, 0.6f);
                            }
                        }
                        hu_fc_result_deinit(&fc_tom, alloc);
                    }
                }

                /* ── BTH post-turn: Voice maturity update (t1e-post) ──────── */
                if (err == HU_OK && response && response_len > 0 && !llm_decides) {
                    size_t vm_idx = (size_t)-1;
                    for (size_t vi = 0; vi < voice_contact_count; vi++) {
                        if (strncmp(voice_contact_keys[vi], batch_key, key_len) == 0 &&
                            voice_contact_keys[vi][key_len] == '\0') {
                            vm_idx = vi;
                            break;
                        }
                    }
                    if (vm_idx != (size_t)-1) {
                        hu_fc_result_t fc_vm;
                        memset(&fc_vm, 0, sizeof(fc_vm));
                        (void)hu_fast_capture(alloc, combined, combined_len, &fc_vm);
                        bool had_emotion = fc_vm.emotion_count > 0;
                        bool had_topic = fc_vm.primary_topic && fc_vm.primary_topic[0];
                        bool had_humor = false;
                        if (combined_len > 0) {
                            for (size_t ci = 0; ci + 2 < combined_len; ci++) {
                                if ((combined[ci] == 'l' || combined[ci] == 'L') &&
                                    (combined[ci + 1] == 'o' || combined[ci + 1] == 'O') &&
                                    (combined[ci + 2] == 'l' || combined[ci + 2] == 'L')) {
                                    had_humor = true;
                                    break;
                                }
                                if ((combined[ci] == 'h') &&
                                    (ci + 3 < combined_len && combined[ci + 1] == 'a' &&
                                     combined[ci + 2] == 'h' && combined[ci + 3] == 'a')) {
                                    had_humor = true;
                                    break;
                                }
                            }
                        }
                        hu_voice_profile_update(&voice_profiles[vm_idx], had_emotion, had_topic,
                                                had_humor);
                        hu_fc_result_deinit(&fc_vm, alloc);
                    }
                }

                /* ── Phase 6 post-turn: ToM baseline, self-awareness, reciprocity (F58, F62, F63)
                 * ── */
#ifndef HU_IS_TEST
#ifdef HU_ENABLE_SQLITE
                if (err == HU_OK && response && response_len > 0 && agent->memory) {
                    if (history_entries && history_count > 0) {
                        (void)hu_theory_of_mind_update_baseline(agent->memory, alloc, batch_key,
                                                                key_len, history_entries,
                                                                history_count);
                    }
                    bool we_initiated = (history_count > 0 && history_entries &&
                                         history_entries[history_count - 1].from_me);
                    char topic_buf[128] = {0};
                    size_t topic_len = 0;
                    if (combined_len > 0 && combined_len < sizeof(topic_buf)) {
                        size_t copy = combined_len;
                        if (copy > 80)
                            copy = 80;
                        memcpy(topic_buf, combined, copy);
                        topic_buf[copy] = '\0';
                        topic_len = strlen(topic_buf);
                    }
                    (void)hu_self_awareness_record_send(
                        alloc, agent->memory, batch_key, key_len, we_initiated,
                        topic_len > 0 ? topic_buf : NULL, topic_len);
                    bool we_asked = (response && (memchr(response, '?', response_len) != NULL));
                    bool we_shared = (response_len > 40);
                    (void)hu_self_awareness_update_reciprocity(alloc, agent->memory, batch_key,
                                                               key_len, we_initiated, we_asked,
                                                               we_shared);
                    /* Optional: set mood from life_sim state when notable */
                    if (agent->persona && (agent->persona->daily_routine.weekday_count > 0 ||
                                           agent->persona->daily_routine.weekend_count > 0)) {
                        time_t now_ts = time(NULL);
                        struct tm tm_buf;
                        struct tm *lt = hu_platform_localtime_r(&now_ts, &tm_buf);
                        int dow = lt ? lt->tm_wday : 0;
                        uint32_t seed = (uint32_t)now_ts * 1103515245u + 12345u;
                        hu_life_sim_state_t ls = hu_life_sim_get_current(
                            &agent->persona->daily_routine, (int64_t)now_ts, dow, seed);
                        if (ls.mood_modifier && strcmp(ls.mood_modifier, "neutral") != 0) {
                            if (strcmp(ls.mood_modifier, "tired") == 0)
                                (void)hu_mood_set(alloc, agent->memory, HU_MOOD_TIRED, 0.4f,
                                                  "life_sim", 8);
                            else if (strcmp(ls.mood_modifier, "stressed") == 0)
                                (void)hu_mood_set(alloc, agent->memory, HU_MOOD_STRESSED, 0.4f,
                                                  "life_sim", 8);
                            else if (strcmp(ls.mood_modifier, "energetic") == 0 ||
                                     strcmp(ls.mood_modifier, "energetic_after") == 0)
                                (void)hu_mood_set(alloc, agent->memory, HU_MOOD_ENERGIZED, 0.4f,
                                                  "life_sim", 8);
                        }
                    }
                }
#endif
#endif

                /* F144-F146 (Pillar 25): LoRA training sample collection */
#ifdef HU_ENABLE_SQLITE
                if (err == HU_OK && response && response_len > 0 && agent->memory && batch_key &&
                    key_len > 0) {
                    sqlite3 *lora_db = hu_sqlite_memory_get_db(agent->memory);
                    if (lora_db) {
                        char lsql[1024];
                        size_t lsql_len = 0;
                        if (hu_lora_insert_training_sample_sql(
                                response, response_len, combined, combined_len,
                                (uint64_t)time(NULL), lsql, sizeof(lsql), &lsql_len) == HU_OK) {
                            sqlite3_stmt *ls = NULL;
                            if (sqlite3_prepare_v2(lora_db, lsql, (int)lsql_len, &ls, NULL) ==
                                SQLITE_OK) {
                                sqlite3_step(ls);
                                sqlite3_finalize(ls);
                            }
                        }
                    }
                }

                /* F160-F161 (Pillar 32): Behavioral feedback from response outcomes */
#ifdef HU_HAS_SKILLS
                if (err == HU_OK && response && response_len > 0 && agent->memory && batch_key &&
                    key_len > 0) {
                    sqlite3 *fb_db = hu_sqlite_memory_get_db(agent->memory);
                    if (fb_db) {
                        bool fb_emoji = false, fb_laugh = false;
                        if (combined_len > 0) {
                            for (size_t fi = 0; fi < combined_len; fi++) {
                                unsigned char fc = (unsigned char)combined[fi];
                                if (fc >= 0xF0) {
                                    fb_emoji = true;
                                    break;
                                }
                            }
                            static const char *const laugh_words[] = {"lol", "haha", "lmao", "rofl",
                                                                      NULL};
                            for (const char *const *lw = laugh_words; *lw; lw++) {
                                if (memmem(combined, combined_len, *lw, strlen(*lw))) {
                                    fb_laugh = true;
                                    break;
                                }
                            }
                        }
                        hu_feedback_class_t fb_class = hu_feedback_classify(
                            0, combined_len, fb_emoji, false, false, fb_laugh, false, false);
                        const char *fb_sig = hu_feedback_class_str(fb_class);
                        hu_feedback_record(fb_db, "response_style", 14, batch_key, key_len, fb_sig,
                                           strlen(fb_sig), response, response_len,
                                           (int64_t)time(NULL));
                    }
                }
#endif
#endif

                /* Phase 7: Post-conversation episode creation (>= 5 messages) */
#ifdef HU_ENABLE_SQLITE
                if (err == HU_OK && response && response_len > 0 && agent->memory &&
                    history_count >= 3 && batch_key && key_len > 0) {
                    sqlite3 *db = hu_sqlite_memory_get_db(agent->memory);
                    if (db) {
                        char summary_buf[1024];
                        size_t sum_len = 0;
                        int n = snprintf(summary_buf, sizeof(summary_buf),
                                         "User: %.*s. Assistant: %.*s",
                                         (int)(combined_len > 200 ? 200 : combined_len), combined,
                                         (int)(response_len > 200 ? 200 : response_len), response);
                        if (n > 0 && (size_t)n < sizeof(summary_buf))
                            sum_len = (size_t)n;
                        if (sum_len > 0) {
                            int64_t episode_id = 0;
                            (void)hu_episode_store_insert(alloc, db, batch_key, key_len,
                                                          summary_buf, sum_len, NULL, 0, NULL, 0,
                                                          0.5, "conversation", 12, &episode_id);
                        }
                    }
                }
#endif

                /* Phase 9 (F115): Record interaction quality */
#if defined(HU_ENABLE_SQLITE)
                if (err == HU_OK && agent && agent->memory && response && response_len > 0 &&
                    batch_key && key_len > 0) {
                    sqlite3 *q_db = hu_sqlite_memory_get_db(agent->memory);
                    if (q_db) {
                        float quality = 1.0f;
                        if (response_len < 20)
                            quality = 0.5f;
                        if (response_len < 5)
                            quality = 0.2f;
                        time_t t_q = time(NULL);
                        hu_interaction_quality_record(q_db, batch_key, quality, 0.5f, NULL,
                                                      (int64_t)t_q);
                    }
                }
#endif

                /* ── BTH post-turn: Graph recall tracking + reconsolidate (t1f) */
#ifdef HU_ENABLE_SQLITE
                if (err == HU_OK && response && response_len > 0 && graph) {
                    hu_deep_extract_result_t de_light;
                    memset(&de_light, 0, sizeof(de_light));
                    if (hu_deep_extract_lightweight(alloc, combined, combined_len, &de_light) ==
                        HU_OK) {
                        for (size_t fi = 0; fi < de_light.fact_count; fi++) {
                            if (!de_light.facts[fi].subject)
                                continue;
                            hu_graph_entity_t ent;
                            memset(&ent, 0, sizeof(ent));
                            if (hu_graph_find_entity(
                                    graph, batch_key, key_len, de_light.facts[fi].subject,
                                    strlen(de_light.facts[fi].subject), &ent) == HU_OK &&
                                ent.id > 0) {
                                (void)hu_graph_record_recall(graph, batch_key, key_len, ent.id);
                                if (de_light.facts[fi].object) {
                                    (void)hu_graph_reconsolidate(graph, alloc, batch_key, key_len,
                                                                 de_light.facts[fi].subject,
                                                                 strlen(de_light.facts[fi].subject),
                                                                 de_light.facts[fi].object,
                                                                 strlen(de_light.facts[fi].object));
                                }
                            }
                        }
                        hu_deep_extract_result_deinit(&de_light, alloc);
                    }
                }
#endif

                /* ── BTH post-turn: LLM deep extraction (t1g) ─────────────── */
                /* Skip in llm_decides mode — too expensive with local model for
                 * casual conversation, and the agent_turn call here uses full
                 * tools which overwhelms the single-threaded MLX server. */
                if (err == HU_OK && response && response_len > 0 && agent->memory && graph &&
                    !llm_decides) {
                    char convo_buf[4096];
                    int cb_w = snprintf(convo_buf, sizeof(convo_buf), "User: %.*s\nAssistant: %.*s",
                                        (int)combined_len, combined, (int)response_len, response);
                    if (cb_w > 0 && (size_t)cb_w < sizeof(convo_buf)) {
                        char *de_prompt = NULL;
                        size_t de_prompt_len = 0;
                        if (hu_deep_extract_build_prompt(alloc, convo_buf, (size_t)cb_w, &de_prompt,
                                                         &de_prompt_len) == HU_OK &&
                            de_prompt && de_prompt_len > 0) {
                            agent->proactive_turn = true;
                            char *de_response = NULL;
                            size_t de_response_len = 0;
                            hu_error_t de_err = hu_agent_turn(agent, de_prompt, de_prompt_len,
                                                              &de_response, &de_response_len);
                            agent->proactive_turn = false;
                            if (de_err == HU_OK && de_response && de_response_len > 0) {
                                hu_deep_extract_result_t de_result;
                                memset(&de_result, 0, sizeof(de_result));
                                if (hu_deep_extract_parse(alloc, de_response, de_response_len,
                                                          &de_result) == HU_OK) {
                                    for (size_t ri = 0; ri < de_result.relation_count; ri++) {
                                        if (!de_result.relations[ri].entity_a ||
                                            !de_result.relations[ri].entity_b)
                                            continue;
                                        int64_t src_id = 0, tgt_id = 0;
                                        (void)hu_graph_upsert_entity(
                                            graph, batch_key, key_len,
                                            de_result.relations[ri].entity_a,
                                            strlen(de_result.relations[ri].entity_a),
                                            HU_ENTITY_UNKNOWN, NULL, &src_id);
                                        (void)hu_graph_upsert_entity(
                                            graph, batch_key, key_len,
                                            de_result.relations[ri].entity_b,
                                            strlen(de_result.relations[ri].entity_b),
                                            HU_ENTITY_UNKNOWN, NULL, &tgt_id);
                                        if (src_id > 0 && tgt_id > 0) {
                                            const char *rel_str =
                                                de_result.relations[ri].relation
                                                    ? de_result.relations[ri].relation
                                                    : "";
                                            (void)hu_graph_upsert_relation(
                                                graph, batch_key, key_len, src_id, tgt_id,
                                                HU_REL_RELATED_TO,
                                                (float)de_result.relations[ri].confidence, rel_str,
                                                strlen(rel_str));
                                        }
                                    }
                                    hu_deep_extract_result_deinit(&de_result, alloc);
                                }
                            }
                            if (de_response)
                                agent->alloc->free(agent->alloc->ctx, de_response,
                                                   de_response_len + 1);
                            alloc->free(alloc->ctx, de_prompt, de_prompt_len + 1);
                        }
                    }
                }
#endif

            skip_llm_this_batch:
                if (turn_out_state.typing_started && ch->channel->vtable->stop_typing)
                    ch->channel->vtable->stop_typing(ch->channel->ctx, batch_key, key_len);
                /* Clear per-turn context and free */
#ifndef HU_IS_TEST
                agent->contact_context = NULL;
                agent->contact_context_len = 0;
                agent->conversation_context = NULL;
                agent->conversation_context_len = 0;
                agent->ab_history_entries = NULL;
                agent->ab_history_count = 0;
                agent->turn_model = NULL;
                agent->turn_model_len = 0;
                agent->lean_prompt = false;
                agent->turn_temperature = 0.0;
                agent->turn_thinking_budget = 0;
                agent->max_response_chars = 0;
                agent->memory_session_id = NULL;
                agent->memory_session_id_len = 0;
                if (agent->memory && agent->memory->vtable) {
                    agent->memory->current_session_id = NULL;
                    agent->memory->current_session_id_len = 0;
                }
                if (contact_ctx)
                    alloc->free(alloc->ctx, contact_ctx, contact_ctx_len + 1);
                if (convo_ctx)
                    alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
#if defined(HU_ENABLE_SQLITE) && !defined(HU_IS_TEST)
                if (cross_channel_ctx)
                    alloc->free(alloc->ctx, cross_channel_ctx, cross_channel_ctx_len + 1);
#endif
#endif

                /* Persist each individual message + the single response */
                if (agent->session_store && agent->session_store->vtable &&
                    agent->session_store->vtable->save_message) {
                    for (size_t b = batch_start; b <= batch_end; b++) {
                        size_t blen = strlen(msgs[b].content);
                        if (blen > 0) {
                            agent->session_store->vtable->save_message(agent->session_store->ctx,
                                                                       batch_key, key_len, "user",
                                                                       4, msgs[b].content, blen);
                        }
                    }
                    if (err == HU_OK && response && response_len > 0) {
                        agent->session_store->vtable->save_message(agent->session_store->ctx,
                                                                   batch_key, key_len, "assistant",
                                                                   9, response, response_len);
                    }
                }

                /* FU-1: record that the REACTIVE path just sent to this contact so
                 * proactive paths (F25, scheduler, proactive check-in, photo,
                 * morning) defer for HU_DAEMON_REACTIVE_GATE_WINDOW_S seconds.
                 * Only record when we actually emitted a reply — silence/skipped
                 * batches must not suppress later proactive sends. */
                if (err == HU_OK && response && response_len > 0 && agent && batch_key &&
                    key_len > 0) {
                    hu_contact_send_recency_record(&agent->contact_send_recency, batch_key, key_len,
                                                   (int64_t)time(NULL), HU_SEND_PATH_REACTIVE);
                }

                /* Store conversation summary as long-term memory */
                if (err == HU_OK && response && response_len > 0 && agent->memory) {
                    store_conversation_summary(alloc, agent->memory, graph, agent, batch_key,
                                               key_len, combined, combined_len, response,
                                               response_len);
                }

#ifdef HU_ENABLE_SQLITE
                /* Task 18: Extraction pipeline — post-turn storage */
                if (err == HU_OK && response && response_len > 0 && agent->memory) {
                    (void)hu_superhuman_extract_and_store(agent->memory, alloc, batch_key, key_len,
                                                          combined, combined_len, response,
                                                          response_len, NULL, 0);
                }

                /* Evolved opinions: extract opinionated statements from responses */
                if (err == HU_OK && response && response_len > 0 && agent->memory) {
                    sqlite3 *op_db = hu_sqlite_memory_get_db(agent->memory);
                    if (op_db) {
                        (void)hu_evolved_opinions_extract_and_store(op_db, response, response_len,
                                                                    (int64_t)time(NULL));
                    }
                }
#endif

#ifndef HU_IS_TEST
                /* F27: If we responded to negative emotion, set pending to record engagement
                 * when we get their next reply.
                 * Skip in llm_decides: avoids post-response emotion LLM call. */
                if (err == HU_OK && response && response_len > 0 && agent->memory &&
                    history_entries && history_count > 0 && !llm_decides) {
                    hu_emotional_state_t emo_pend =
                        hu_daemon_detect_emotion(alloc, agent, history_entries, history_count);
                    bool should_pend = emo_pend.concerning ||
                                       (emo_pend.dominant_emotion &&
                                        (strcmp(emo_pend.dominant_emotion, "sad") == 0 ||
                                         strcmp(emo_pend.dominant_emotion, "stressed") == 0 ||
                                         strcmp(emo_pend.dominant_emotion, "anxious") == 0 ||
                                         strcmp(emo_pend.dominant_emotion, "worried") == 0));
                    if (should_pend && key_len < sizeof(comfort_pending[0].key)) {
                        char resp_type[32];
                        classify_comfort_response_type(response, response_len, resp_type,
                                                       sizeof(resp_type));
                        const char *emotion_str =
                            emo_pend.dominant_emotion && emo_pend.dominant_emotion[0]
                                ? emo_pend.dominant_emotion
                                : "concerning";
                        size_t slot = HU_COMFORT_PENDING_MAX;
                        for (size_t cp_i = 0; cp_i < HU_COMFORT_PENDING_MAX; cp_i++) {
                            if (comfort_pending[cp_i].key[0] == '\0' ||
                                (memcmp(comfort_pending[cp_i].key, batch_key, key_len) == 0 &&
                                 comfort_pending[cp_i].key[key_len] == '\0')) {
                                slot = cp_i;
                                break;
                            }
                        }
                        if (slot < HU_COMFORT_PENDING_MAX) {
                            memcpy(comfort_pending[slot].key, batch_key, key_len);
                            comfort_pending[slot].key[key_len] = '\0';
                            snprintf(comfort_pending[slot].emotion,
                                     sizeof(comfort_pending[slot].emotion), "%s", emotion_str);
                            snprintf(comfort_pending[slot].response_type,
                                     sizeof(comfort_pending[slot].response_type), "%s", resp_type);
                        }
                    }
                }

                /* Episodic: summarize this interaction (LLM when provider available, else
                 * rule-based). Skip in llm_decides mode — extra LLM call is too slow. */
                if (err == HU_OK && response && response_len > 0 && agent->memory && !llm_decides) {
                    const char *ep_msgs[2] = {combined, response};
                    size_t ep_lens[2] = {combined_len, response_len};
                    size_t summary_len = 0;
                    char *summary = hu_episodic_summarize_session_llm(
                        alloc, &agent->provider, ep_msgs, ep_lens, 2, &summary_len);
                    if (summary && summary_len > 0) {
                        if (hu_episodic_store(agent->memory, alloc, batch_key, key_len, summary,
                                              summary_len) != HU_OK)
                            hu_log_error("human", agent ? agent->observer : NULL,
                                         "episodic store failed");
                        alloc->free(alloc->ctx, summary, summary_len + 1);
                    } else if (summary) {
                        alloc->free(alloc->ctx, summary, summary_len + 1);
                    }
                }

                /* Promote STM entities to persistent memory every 5 turns */
                if (err == HU_OK && ++promotion_counter % 5 == 0 && agent->stm.turn_count > 0 &&
                    agent->memory) {
                    hu_promotion_config_t promo_cfg = {
                        .min_mention_count = 2,
                        .min_importance = 0.3,
                        .max_entities = 10,
                    };
                    if (hu_promotion_run(alloc, &agent->stm, agent->memory, &promo_cfg) != HU_OK)
                        hu_log_error("human", agent ? agent->observer : NULL,
                                     "promotion run failed");
                }
#endif

                /* Emotion promotion: promote emotions from STM to long-term memory (every turn) */
                if (err == HU_OK && response && response_len > 0 && agent->stm.turn_count > 0 &&
                    agent->memory && agent->memory->vtable && agent->memory->vtable->store) {
                    (void)hu_promotion_run_emotions(alloc, &agent->stm, agent->memory, batch_key,
                                                    key_len);
#ifndef HU_IS_TEST
                    if (agent->bth_metrics)
                        agent->bth_metrics->emotions_promoted++;
#endif
                }

                /* ── BTH Tier 3: AI disclosure logging (t3c) ────── */
                /* SHIELD-002: Honest disclosure is allowed. Log but do not suppress. */
#ifndef HU_IS_TEST
                if (err == HU_OK && response && response_len > 0) {
                    if (hu_conversation_check_ai_disclosure(response, response_len)) {
                        hu_log_info("human", agent ? agent->observer : NULL,
                                    "AI disclosure detected in response (allowed)");
                    }
                }
#endif

#ifndef HU_IS_TEST
                /* Free history before post-send block when that block will not run (it also
                 * uses history for inline reply, double-text, GIF, music). */
                if (history_entries && !(err == HU_OK && response && response_len > 0)) {
                    alloc->free(alloc->ctx, history_entries,
                                history_count * sizeof(hu_channel_history_entry_t));
                    history_entries = NULL;
                    history_count = 0;
                }
#endif

                size_t response_alloc_len = response_len;
                uint32_t typo_seed = 0;
                char *original_response = NULL;
                size_t original_len = 0;
                if (err == HU_OK && response && response_len > 0) {
                    const char *proactive_vis_m[1] = {NULL};
                    size_t proactive_vis_n = 0;
                    char proactive_vis_storage[1024];
                    proactive_vis_storage[0] = '\0';
                    if (ch->channel && ch->channel->vtable && ch->channel->vtable->send &&
                        combined_len > 0) {
                        hu_visual_proactive_media_kind_t pv_kind = HU_VISUAL_MEDIA_NONE;
                        const char *pv_reason = NULL;
                        (void)pv_reason;
                        uint64_t pv_now = (uint64_t)time(NULL) * 1000ULL;
                        bool pv_pro_gov = true;
#ifndef HU_IS_TEST
                        pv_pro_gov = hu_daemon_visual_attach_gov_allow(pv_now);
#endif
                        if (hu_visual_should_send_media(combined, combined_len, &pv_kind,
                                                        &pv_reason) &&
                            pv_kind != HU_VISUAL_MEDIA_NONE &&
                            hu_visual_proactive_media_governor_allow(pv_now) && pv_pro_gov) {
                            if (pv_kind == HU_VISUAL_MEDIA_IMAGE_SEARCH) {
                                size_t qlen = combined_len > 160 ? 160 : combined_len;
                                while (qlen > 0 &&
                                       (combined[qlen - 1] == ' ' || combined[qlen - 1] == '\n' ||
                                        combined[qlen - 1] == '\t' || combined[qlen - 1] == '\r'))
                                    qlen--;
                                if (qlen > 0 && hu_visual_search_image(
                                                    alloc, combined, qlen, proactive_vis_storage,
                                                    sizeof proactive_vis_storage) == HU_OK) {
                                    proactive_vis_m[0] = proactive_vis_storage;
                                    proactive_vis_n = 1;
                                }
                            } else if (pv_kind == HU_VISUAL_MEDIA_SCREENSHOT) {
                                if (hu_visual_generate_screenshot(
                                        alloc, agent->policy, proactive_vis_storage,
                                        sizeof proactive_vis_storage) == HU_OK) {
                                    proactive_vis_m[0] = proactive_vis_storage;
                                    proactive_vis_n = 1;
                                }
                            }
                        }
                    }

                    /* Merge proactive visuals + tool-generated media into a
                     * single array for the channel send paths below. */
                    const char *all_send_media[8];
                    size_t all_send_media_n = 0;
                    for (size_t pmi = 0; pmi < proactive_vis_n && all_send_media_n < 8; pmi++)
                        all_send_media[all_send_media_n++] = proactive_vis_m[pmi];
                    if (agent) {
                        for (size_t gmi = 0;
                             gmi < agent->generated_media_count && all_send_media_n < 8; gmi++)
                            all_send_media[all_send_media_n++] = agent->generated_media[gmi];
                    }
                    const char *const *all_send_media_ptr =
                        all_send_media_n > 0 ? all_send_media : NULL;
                    size_t all_send_media_cnt = all_send_media_n;

                    /* Strip pipeline + send buffer cursor: shared by prod and HU_IS_TEST so tests
                     * exercise the same outbound text shaping as production. */
                    char *send_buf_ack = NULL;
                    hu_validator_chain_apply_default_in_place(
                        alloc, agent ? agent->observer : NULL, NULL, 0, "main response send",
                        response, &response_len, response_len + 1);

#ifndef HU_IS_TEST
                    /* Apply typing quirks from persona overlay as post-processing.
                     * This shrinks the buffer in-place; keep original size for free. */
                    const hu_persona_overlay_t *overlay = NULL;
                    if (agent->persona && agent->active_channel) {
                        overlay = hu_persona_find_overlay(agent->persona, agent->active_channel,
                                                          agent->active_channel_len);
                        if (overlay && overlay->typing_quirks && overlay->typing_quirks_count > 0) {
                            response_len = hu_conversation_apply_typing_quirks(
                                response, response_len, (const char *const *)overlay->typing_quirks,
                                overlay->typing_quirks_count);
                        }
                    }

                    /* BTH Tier 3: Stylometric variance (t3a) — apply contractions */
                    response_len = hu_conversation_vary_complexity(response, response_len,
                                                                   (uint32_t)time(NULL));

                    /* BTH Tier 2: Filler injection (t2c) — casual channel fillers */
                    {
                        size_t filler_cap = response_alloc_len + 1;
                        if (filler_cap < response_len + 16) {
                            char *grown = (char *)agent->alloc->realloc(agent->alloc->ctx, response,
                                                                        response_alloc_len + 1,
                                                                        response_len + 16);
                            if (grown) {
                                response = grown;
                                response_alloc_len = response_len + 15;
                                filler_cap = response_len + 16;
                            }
                        }
                        const char *ch_name =
                            agent->active_channel ? agent->active_channel : "unknown";
                        size_t ch_name_len = agent->active_channel ? agent->active_channel_len : 7;
                        response_len = hu_conversation_apply_fillers(
                            response, response_len, filler_cap, (uint32_t)time(NULL), ch_name,
                            ch_name_len);
                    }

                    /* BTH: Text disfluency (F33) — natural imperfections, casual only */
                    {
                        size_t disfluency_cap = response_alloc_len + 1;
                        if (disfluency_cap < response_len + 16) {
                            char *grown = (char *)agent->alloc->realloc(agent->alloc->ctx, response,
                                                                        response_alloc_len + 1,
                                                                        response_len + 16);
                            if (grown) {
                                response = grown;
                                response_alloc_len = response_len + 15;
                                disfluency_cap = response_len + 16;
                            }
                        }
                        const hu_contact_profile_t *contact_profile =
                            (agent->persona && batch_key && key_len > 0)
                                ? hu_persona_find_contact(agent->persona, batch_key, key_len)
                                : NULL;
                        const char *formality =
                            (overlay && overlay->formality) ? overlay->formality : NULL;
                        size_t formality_len = formality ? strlen(formality) : 0;
                        float disfluency_freq =
                            agent->persona ? agent->persona->humanization.disfluency_frequency
                                           : 0.15f;
                        response_len = hu_conversation_apply_disfluency(
                            response, response_len, disfluency_cap, (uint32_t)time(NULL),
                            disfluency_freq, contact_profile, formality, formality_len);
                    }

                    /* Typo simulation: requires "occasional_typos" quirk and buffer capacity */
                    bool has_typo_quirk = false;
                    if (overlay && overlay->typing_quirks) {
                        for (size_t tq = 0; tq < overlay->typing_quirks_count; tq++) {
                            if (overlay->typing_quirks[tq] &&
                                strcmp(overlay->typing_quirks[tq], "occasional_typos") == 0) {
                                has_typo_quirk = true;
                                break;
                            }
                        }
                    }

                    original_len = response_len;
                    if (has_typo_quirk && response && response_len > 0) {
                        original_response = (char *)alloc->alloc(alloc->ctx, response_len + 1);
                        if (original_response) {
                            memcpy(original_response, response, response_len);
                            original_response[response_len] = '\0';
                        }
                    }

                    if (has_typo_quirk && response && response_len > 0) {
                        size_t cap = response_alloc_len + 1;
                        if (cap <= response_len + 1) {
                            char *grown = (char *)agent->alloc->realloc(agent->alloc->ctx, response,
                                                                        response_alloc_len + 1,
                                                                        response_len + 2);
                            if (grown) {
                                response = grown;
                                response_alloc_len = response_len + 1;
                                cap = response_len + 2;
                            }
                        }
                        if (cap > response_len + 1) {
                            typo_seed = (uint32_t)time(NULL);
                            size_t new_len =
                                hu_conversation_apply_typos(response, response_len, cap, typo_seed);
                            response_len = new_len;
                            if (agent->bth_metrics)
                                agent->bth_metrics->typos_applied++;
                        }
                    }
                    /* ── F40: Inline reply (quoted text fallback) ─────────────
                     * When classifier says inline reply, prepend "> {quoted}\n\n"
                     * (text-quote fallback for channels without native threading). */
#ifndef HU_IS_TEST
                    {
                        if (response && response_len > 0 &&
                            hu_conversation_should_inline_reply(history_entries, history_count,
                                                                combined, combined_len)) {
                            size_t quote_len = combined_len > 80 ? 80 : combined_len;
                            size_t prefix_len = 2 + quote_len + 2; /* "> " + quote + "\n\n" */
                            size_t total_len = prefix_len + response_len;
                            char *prefixed = (char *)alloc->alloc(alloc->ctx, total_len + 1);
                            if (prefixed) {
                                memcpy(prefixed, "> ", 2);
                                memcpy(prefixed + 2, combined, quote_len);
                                prefixed[2 + quote_len] = '\n';
                                prefixed[2 + quote_len + 1] = '\n';
                                memcpy(prefixed + prefix_len, response, response_len + 1);
                                alloc->free(alloc->ctx, response, response_alloc_len + 1);
                                response = prefixed;
                                response_len = total_len;
                                response_alloc_len = total_len;
                            }
                        }
                    }
#endif
                    /* ── Pre-send re-check: abort if real user responded while
                     * we were generating.  Prevents piling onto a conversation
                     * the user is actively handling. ─────────────────────────── */
#ifndef HU_IS_TEST
                    {
                        const char *chn_name = ch->channel->vtable->name
                                                   ? ch->channel->vtable->name(ch->channel->ctx)
                                                   : NULL;
                        const hu_channel_daemon_config_t *dcfg_ps =
                            get_active_daemon_config(config, chn_name);
                        int window = 120;
                        if (dcfg_ps && dcfg_ps->user_response_window_sec > 0)
                            window = dcfg_ps->user_response_window_sec;
                        if (chn_name && ch->channel->vtable->human_active_recently &&
                            ch->channel->vtable->human_active_recently(ch->channel->ctx, batch_key,
                                                                       key_len, window)) {
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "pre-send abort: real user active "
                                        "for %.*s — dropping generated response",
                                        (int)(key_len > 20 ? 20 : key_len), batch_key);
                            goto skip_send;
                        }
                    }
#endif
                    /* Bus final delivery: post-processed text (send_event FINAL or send). */
                    turn_out_state.text_delivered_via_bus = false;
                    if (err == HU_OK && response && response_len > 0) {
                        hu_bus_event_t rev;
                        memset(&rev, 0, sizeof(rev));
                        rev.type = HU_BUS_MESSAGE_SENT;
                        if (agent->active_channel && agent->active_channel[0]) {
                            int nc4 = snprintf(rev.channel, HU_BUS_CHANNEL_LEN, "%s",
                                               agent->active_channel);
                            (void)nc4;
                        } else if (ch->channel->vtable->name) {
                            const char *cn4 = ch->channel->vtable->name(ch->channel->ctx);
                            if (cn4)
                                (void)snprintf(rev.channel, HU_BUS_CHANNEL_LEN, "%s", cn4);
                        }
                        {
                            size_t idk = key_len < HU_BUS_ID_LEN - 1 ? key_len : HU_BUS_ID_LEN - 1;
                            memcpy(rev.id, batch_key, idk);
                            rev.id[idk] = '\0';
                        }
                        rev.payload = response;
                        daemon_bus_set_message(&rev, response, response_len);
                        daemon_out_bus_bridge.delivery_turn = &turn_out_state;
                        hu_bus_publish(&daemon_outbound_bus, &rev);
                        daemon_out_bus_bridge.delivery_turn = NULL;
                    }
                    /* ── Voice decision: TTS when channel has voice_enabled ───── */
                    bool sent_voice = false;
                    {
                        const char *chn_voice = ch->channel->vtable->name
                                                    ? ch->channel->vtable->name(ch->channel->ctx)
                                                    : NULL;
                        const hu_channel_daemon_config_t *dcfg_voice =
                            get_active_daemon_config(config, chn_voice);
                        bool voice_channel_ok = dcfg_voice && dcfg_voice->voice_enabled;

                        /* Unified duplex + Realtime (`voice.mode`: "realtime" or legacy
                         * `voice.tts_provider`: "realtime"). */
                        hu_voice_session_t unified_voice = {0};
                        bool unified_voice_active = false;
                        bool cfg_realtime =
                            config &&
                            ((config->voice.mode && strcmp(config->voice.mode, "realtime") == 0) ||
                             (config->voice.tts_provider &&
                              strcmp(config->voice.tts_provider, "realtime") == 0));
                        if (voice_channel_ok && config && chn_voice && cfg_realtime) {
                            size_t chn_len = strlen(chn_voice);
                            if (hu_voice_session_start(alloc, &unified_voice, chn_voice, chn_len,
                                                       config) == HU_OK)
                                unified_voice_active = true;
                        }

#if defined(HU_ENABLE_CARTESIA)
                        if (voice_channel_ok && agent->persona &&
                            agent->persona->voice.voice_id[0] &&
                            agent->persona->voice_messages.enabled) {
                            hu_voice_decision_t vdec = hu_voice_decision_classify(
                                response, response_len, combined, combined_len,
                                &agent->persona->voice_messages, true, bth_hour,
                                (uint32_t)(time(NULL) ^ (uintptr_t)combined));
                            if (vdec == HU_VOICE_SEND_VOICE) {
                                const char *cartesia_key =
                                    hu_config_get_provider_key(config, "cartesia");
                                if (cartesia_key && cartesia_key[0]) {
                                    char voice_transcript[4096];
                                    size_t vt_len = response_len < sizeof(voice_transcript) - 64
                                                        ? response_len
                                                        : sizeof(voice_transcript) - 64;
                                    memcpy(voice_transcript, response, vt_len);
                                    voice_transcript[vt_len] = '\0';
                                    vt_len = hu_conversation_inject_nonverbals(
                                        voice_transcript, vt_len, sizeof(voice_transcript),
                                        (uint32_t)time(NULL), agent->persona->voice.nonverbals);

                                    const char *emo_str = hu_cartesia_emotion_from_context(
                                        combined, combined_len, response, response_len,
                                        (uint8_t)bth_hour);

                                    /* Emotion voice map: detect emotion + derive expressive params
                                     */
                                    hu_voice_emotion_t detected_emotion = HU_VOICE_EMOTION_NEUTRAL;
                                    float emotion_confidence = 0.0f;
                                    hu_emotion_detect_from_text(response, response_len,
                                                                &detected_emotion,
                                                                &emotion_confidence);
                                    hu_voice_params_t evo_params =
                                        hu_emotion_voice_map(detected_emotion);
                                    float base_speed = agent->persona->voice.default_speed > 0.f
                                                           ? agent->persona->voice.default_speed
                                                           : 0.95f;

                                    hu_cartesia_tts_config_t tts_cfg = {
                                        .model_id = agent->persona->voice.model[0]
                                                        ? agent->persona->voice.model
                                                        : "sonic-3-2026-01-12",
                                        .voice_id = agent->persona->voice.voice_id,
                                        .emotion = emo_str,
                                        .speed = base_speed * evo_params.rate_factor,
                                        .volume = 1.0f,
                                        .nonverbals = agent->persona->voice.nonverbals,
                                    };

                                    const char *voice_fmt = hu_tts_format_for_channel(chn_voice);
                                    unsigned char *audio_bytes = NULL;
                                    size_t audio_len = 0;
                                    hu_error_t tts_err = hu_cartesia_tts_synthesize(
                                        alloc, cartesia_key, strlen(cartesia_key), voice_transcript,
                                        vt_len, &tts_cfg, voice_fmt, &audio_bytes, &audio_len);
                                    if (tts_err == HU_OK && audio_bytes && audio_len > 0) {
                                        char audio_path[512];
                                        hu_error_t pipe_err;
                                        if (strcmp(voice_fmt, "caf") == 0) {
                                            pipe_err =
                                                hu_audio_mp3_to_caf(alloc, audio_bytes, audio_len,
                                                                    audio_path, sizeof(audio_path));
                                        } else {
                                            const char *temp_ext = "mp3";
                                            if (strcmp(voice_fmt, "wav") == 0)
                                                temp_ext = "wav";
                                            else if (strcmp(voice_fmt, "ogg") == 0)
                                                /* Cartesia has no OGG; WAV on disk until Opus
                                                 * encode */
                                                temp_ext = "wav";
                                            pipe_err = hu_audio_tts_bytes_to_temp(
                                                alloc, audio_bytes, audio_len, temp_ext, audio_path,
                                                sizeof(audio_path));
                                        }
                                        hu_cartesia_tts_free_bytes(alloc, audio_bytes, audio_len);
                                        if (pipe_err == HU_OK) {
                                            const char *media_paths[] = {audio_path};
                                            hu_error_t send_err = ch->channel->vtable->send(
                                                ch->channel->ctx, batch_key, key_len, "", 0,
                                                media_paths, 1);
                                            hu_audio_cleanup_temp(audio_path);
                                            if (send_err == HU_OK)
                                                sent_voice = true;
                                        }
                                    } else if (audio_bytes) {
                                        hu_cartesia_tts_free_bytes(alloc, audio_bytes, audio_len);
                                    }
                                }
                            }
                        }
#endif
                        /* Fallback: unified voice pipeline when persona Cartesia path did not send.
                         */
                        if (!sent_voice && voice_channel_ok && !unified_voice_active && config) {
                            hu_voice_config_t voice_cfg = {0};
                            if (hu_voice_config_from_settings(config, &voice_cfg) == HU_OK &&
                                voice_cfg.tts_provider && voice_cfg.tts_provider[0]) {
                                void *audio = NULL;
                                size_t audio_len = 0;
                                hu_error_t tts_err = hu_voice_tts(alloc, &voice_cfg, response,
                                                                  response_len, &audio, &audio_len);
                                if (tts_err == HU_OK && audio && audio_len > 0) {
                                    unsigned char *audio_bytes = (unsigned char *)audio;
                                    char audio_path[512];
                                    hu_error_t pipe_err = HU_ERR_IO;
#if defined(HU_ENABLE_CARTESIA)
                                    {
                                        const char *voice_fmt =
                                            hu_tts_format_for_channel(chn_voice);
                                        if (strcmp(voice_fmt, "caf") == 0) {
                                            pipe_err =
                                                hu_audio_mp3_to_caf(alloc, audio_bytes, audio_len,
                                                                    audio_path, sizeof(audio_path));
                                        } else {
                                            const char *temp_ext = "mp3";
                                            if (strcmp(voice_fmt, "wav") == 0)
                                                temp_ext = "wav";
                                            else if (strcmp(voice_fmt, "ogg") == 0)
                                                temp_ext = "wav";
                                            pipe_err = hu_audio_tts_bytes_to_temp(
                                                alloc, audio_bytes, audio_len, temp_ext, audio_path,
                                                sizeof(audio_path));
                                        }
                                    }
#else
                                    {
                                        char *tmp_dir = hu_platform_get_temp_dir(alloc);
                                        if (tmp_dir) {
                                            int np = snprintf(audio_path, sizeof(audio_path),
                                                              "%s/human_dtts_%lld.mp3", tmp_dir,
                                                              (long long)time(NULL));
                                            size_t tdl = strlen(tmp_dir);
                                            alloc->free(alloc->ctx, tmp_dir, tdl + 1);
                                            if (np > 0 && (size_t)np < sizeof(audio_path)) {
                                                FILE *tf = fopen(audio_path, "wb");
                                                if (tf) {
                                                    if (fwrite(audio_bytes, 1, audio_len, tf) ==
                                                        audio_len)
                                                        pipe_err = HU_OK;
                                                    fclose(tf);
                                                    if (pipe_err != HU_OK)
                                                        (void)unlink(audio_path);
                                                }
                                            }
                                        }
                                    }
#endif
                                    alloc->free(alloc->ctx, audio, audio_len);
                                    if (pipe_err == HU_OK) {
                                        const char *media_paths[] = {audio_path};
                                        hu_error_t send_err = ch->channel->vtable->send(
                                            ch->channel->ctx, batch_key, key_len, "", 0,
                                            media_paths, 1);
#if defined(HU_ENABLE_CARTESIA)
                                        hu_audio_cleanup_temp(audio_path);
#else
                                        (void)unlink(audio_path);
#endif
                                        if (send_err == HU_OK)
                                            sent_voice = true;
                                    }
                                } else if (audio) {
                                    alloc->free(alloc->ctx, audio, audio_len);
                                }
                            }
                        }
                        if (unified_voice_active) {
                            hu_voice_session_warn_first_byte_latency_if_needed(&unified_voice);
                            (void)hu_voice_session_stop(&unified_voice);
                        }
                    }
                    if (!sent_voice && !turn_out_state.text_delivered_via_bus) {
                        const char *eff_ch = ch->channel->vtable->name
                                                 ? ch->channel->vtable->name(ch->channel->ctx)
                                                 : "unknown";
                        /* Strip invalid UTF-8 and surrogate-encoded garbage.
                         * Keeps: ASCII printable, newlines, valid multi-byte UTF-8 (including
                         * emoji). Strips: invalid sequences, lone surrogates (U+D800-U+DFFF encoded
                         * as 3-byte). */
                        {
                            size_t w = 0;
                            for (size_t r = 0; r < response_len;) {
                                unsigned char b = (unsigned char)response[r];
                                if (b < 0x80) {
                                    if (b >= 0x20 || b == '\n' || b == '\t')
                                        response[w++] = response[r];
                                    r++;
                                } else {
                                    size_t seq = 0;
                                    if ((b & 0xE0) == 0xC0)
                                        seq = 2;
                                    else if ((b & 0xF0) == 0xE0)
                                        seq = 3;
                                    else if ((b & 0xF8) == 0xF0)
                                        seq = 4;
                                    if (seq == 0 || r + seq > response_len) {
                                        r++;
                                        continue;
                                    }
                                    bool valid = true;
                                    for (size_t k = 1; k < seq; k++) {
                                        if (((unsigned char)response[r + k] & 0xC0) != 0x80) {
                                            valid = false;
                                            break;
                                        }
                                    }
                                    /* Reject 3-byte sequences encoding surrogates (U+D800-U+DFFF)
                                     */
                                    if (valid && seq == 3) {
                                        unsigned int s_cp =
                                            ((b & 0x0F) << 12) |
                                            (((unsigned char)response[r + 1] & 0x3F) << 6) |
                                            ((unsigned char)response[r + 2] & 0x3F);
                                        if (s_cp >= 0xD800 && s_cp <= 0xDFFF)
                                            valid = false;
                                    }
                                    if (valid) {
                                        if (w != r)
                                            memmove(response + w, response + r, seq);
                                        w += seq;
                                    }
                                    r += valid ? seq : 1;
                                }
                            }
                            if (w < response_len) {
                                response[w] = '\0';
                                response_len = w;
                            }
                        }
                        /* Strip meta-reasoning: local models sometimes emit (parenthetical
                         * analysis) instead of just the message. Remove any leading text
                         * up to and including the last ')' if the response starts with '('. */
                        if (llm_decides && response_len > 0 && response[0] == '(') {
                            char *last_paren = NULL;
                            for (size_t ri = 0; ri < response_len; ri++) {
                                if (response[ri] == ')')
                                    last_paren = response + ri;
                            }
                            if (last_paren) {
                                char *clean = last_paren + 1;
                                while (*clean == ' ' || *clean == '\n' || *clean == '\r')
                                    clean++;
                                size_t new_len = response_len - (size_t)(clean - response);
                                if (new_len > 0 && new_len < response_len) {
                                    memmove(response, clean, new_len);
                                    response[new_len] = '\0';
                                    response_len = new_len;
                                    hu_log_info("human", agent ? agent->observer : NULL,
                                                "stripped meta-reasoning, clean len=%zu", new_len);
                                } else if (new_len == 0) {
                                    /* Entire response was meta-reasoning; use a fallback */
                                    static const char fb[] = "hey whats up";
                                    memcpy(response, fb, sizeof(fb));
                                    response_len = sizeof(fb) - 1;
                                    hu_log_info(
                                        "human", agent ? agent->observer : NULL,
                                        "meta-reasoning fallback (entire response was reasoning)");
                                }
                            }
                        }
                        const char *send_ptr = response;
                        size_t send_len = response_len;
                        {
                            time_t now_ts = time(NULL);
                            time_t msg_ts = (msgs[batch_start].timestamp_sec > 0)
                                                ? (time_t)msgs[batch_start].timestamp_sec
                                                : poll_receive_time;
                            int64_t delay_secs = (int64_t)(now_ts - msg_ts);
                            struct tm tm_recv, tm_now;
                            struct tm *pr = localtime_r(&msg_ts, &tm_recv);
                            struct tm *pn = localtime_r(&now_ts, &tm_now);
                            int recv_hr = pr ? pr->tm_hour : 0;
                            int curr_hr = pn ? pn->tm_hour : 0;
                            const char *ack = hu_missed_message_acknowledgment(
                                delay_secs, recv_hr, curr_hr, (uint32_t)now_ts);
                            if (ack) {
                                size_t ack_len = strlen(ack);
                                if (response_len <= SIZE_MAX - ack_len &&
                                    ack_len + response_len <= SIZE_MAX - 3) {
                                    send_buf_ack = (char *)alloc->alloc(
                                        alloc->ctx, ack_len + 2 + response_len + 1);
                                }
                                if (send_buf_ack) {
                                    memcpy(send_buf_ack, ack, ack_len);
                                    send_buf_ack[ack_len] = '\n';
                                    send_buf_ack[ack_len + 1] = '\n';
                                    memcpy(send_buf_ack + ack_len + 2, response, response_len + 1);
                                    send_ptr = send_buf_ack;
                                    send_len = ack_len + 2 + response_len;
                                }
                            }
                        }
                        /* SHIELD-004/005: Moderation + crisis escalation before send */
                        {
                            hu_moderation_result_t mod_r;
                            memset(&mod_r, 0, sizeof(mod_r));
                            if (hu_moderation_check(alloc, send_ptr, send_len, &mod_r) == HU_OK &&
                                mod_r.flagged) {
                                hu_log_warn("human", agent ? agent->observer : NULL,
                                            "moderation flagged output (categories: %s%s%s%s), "
                                            "blocking send",
                                            mod_r.violence ? "violence " : "",
                                            mod_r.hate ? "hate " : "",
                                            mod_r.sexual ? "sexual " : "",
                                            mod_r.self_harm ? "self-harm " : "");
                                static const char mod_safe_reply[] =
                                    "ugh brain fart — lemme rephrase that";
                                if (send_buf_ack) {
                                    alloc->free(alloc->ctx, send_buf_ack, send_len + 1);
                                    send_buf_ack = NULL;
                                }
                                send_ptr = mod_safe_reply;
                                send_len = sizeof(mod_safe_reply) - 1;
                                if (mod_r.self_harm) {
                                    char *crisis = NULL;
                                    size_t crisis_len = 0;
                                    if (hu_crisis_response_build(alloc, &crisis, &crisis_len) ==
                                            HU_OK &&
                                        crisis) {
                                        size_t new_len = send_len + 2 + crisis_len;
                                        char *merged =
                                            (char *)alloc->alloc(alloc->ctx, new_len + 1);
                                        if (merged) {
                                            memcpy(merged, send_ptr, send_len);
                                            merged[send_len] = '\n';
                                            merged[send_len + 1] = '\n';
                                            memcpy(merged + send_len + 2, crisis, crisis_len);
                                            merged[new_len] = '\0';
                                            send_buf_ack = merged;
                                            send_ptr = send_buf_ack;
                                            send_len = new_len;
                                        }
                                        alloc->free(alloc->ctx, crisis, crisis_len + 1);
                                    }
                                }
                            }
                        }
                        /* SHIELD-001: Companion safety check before send */
                        {
                            hu_companion_safety_result_t cs_r;
                            if (hu_companion_safety_check(alloc, send_ptr, send_len, NULL, 0,
                                                          &cs_r) == HU_OK &&
                                cs_r.flagged) {
                                hu_log_warn("human", agent ? agent->observer : NULL,
                                            "companion safety flagged output, replacing");
                                static const char cs_safe_reply[] =
                                    "wait actually scratch that — bad take on my end";
                                if (send_buf_ack) {
                                    alloc->free(alloc->ctx, send_buf_ack, send_len + 1);
                                    send_buf_ack = NULL;
                                }
                                send_ptr = cs_safe_reply;
                                send_len = sizeof(cs_safe_reply) - 1;
                            }
                        }
                        /* MEM-002: Memory claim verification gate + TRUST-006 updates */
#ifdef HU_ENABLE_SQLITE
                        if (hu_memory_has_claim_language(send_ptr, send_len)) {
                            sqlite3 *vc_db = hu_sqlite_memory_get_db(agent->memory);
                            if (vc_db) {
                                hu_claim_result_t claim_r;
                                if (hu_memory_verify_claim(alloc, vc_db, batch_key, key_len,
                                                           send_ptr, send_len, &claim_r) == HU_OK) {
                                    hu_trust_state_t ts;
                                    bool have_ts = (hu_daemon_get_trust_state(batch_key, key_len,
                                                                              &ts) == HU_OK);
                                    int64_t now_ts = (int64_t)time(NULL);
                                    if (claim_r.confidence >= 0.15) {
                                        /* Verified claim — boost trust */
                                        if (have_ts) {
                                            hu_trust_update(&ts, HU_TRUST_VERIFIED_CLAIM, now_ts);
                                            (void)hu_daemon_set_trust_state(batch_key, key_len,
                                                                            &ts);
                                        }
                                    } else {
                                        /* Unverified claim — erode trust and hedge */
                                        if (have_ts) {
                                            hu_trust_update(&ts, HU_TRUST_FABRICATION, now_ts);
                                            (void)hu_daemon_set_trust_state(batch_key, key_len,
                                                                            &ts);
                                        }
                                        char *hedged = NULL;
                                        size_t hedged_len = 0;
                                        if (hu_memory_hedge_claim(alloc, send_ptr, send_len,
                                                                  &hedged, &hedged_len) == HU_OK &&
                                            hedged) {
                                            if (send_buf_ack) {
                                                alloc->free(alloc->ctx, send_buf_ack, send_len + 1);
                                                send_buf_ack = NULL;
                                            }
                                            send_buf_ack = hedged;
                                            send_ptr = send_buf_ack;
                                            send_len = hedged_len;
                                        }
                                        hu_log_info("human", agent ? agent->observer : NULL,
                                                    "memory claim unverified: "
                                                    "conf=%.2f prov=%d contact=%d",
                                                    claim_r.confidence, claim_r.has_provenance,
                                                    claim_r.contact_match);
                                    }
                                    if (have_ts && hu_trust_detect_erosion(&ts)) {
                                        hu_log_info("human", agent ? agent->observer : NULL,
                                                    "TRUST-006: trust eroded for "
                                                    "%.*s (level=%.2f)",
                                                    (int)(key_len > 20 ? 20 : key_len), batch_key,
                                                    ts.trust_level);
                                    }
                                }
                            }
                        }
#endif
                        /* Split response into natural multi-message fragments */
                        uint32_t split_max = 0;
                        if (ch->channel->vtable->get_response_constraints) {
                            hu_channel_response_constraints_t constraints = {0};
                            if (ch->channel->vtable->get_response_constraints(
                                    ch->channel->ctx, &constraints) == HU_OK) {
                                split_max = constraints.max_chars;
                            }
                        }

                        /* F2: Choreography-driven message delivery */
                        hu_message_plan_t choreo_plan = {0};
                        bool use_choreography = false;
                        if (agent && agent->frontiers.initialized) {
                            hu_choreography_config_t choreo_cfg = hu_choreography_config_default();
                            choreo_cfg.energy_level = agent->frontiers.somatic.energy;
                            hu_error_t ce = hu_choreography_plan(
                                alloc, send_ptr, send_len, &choreo_cfg,
                                (uint32_t)(time(NULL) ^ (uintptr_t)send_ptr), &choreo_plan);
                            if (ce == HU_OK && choreo_plan.segment_count > 1)
                                use_choreography = true;
                        }

                        if (use_choreography) {
                            for (size_t seg = 0; seg < choreo_plan.segment_count; seg++) {
                                if (seg > 0 && choreo_plan.segments[seg].delay_ms > 0) {
                                    uint32_t dms = choreo_plan.segments[seg].delay_ms;
                                    if (bth_hour >= 1 && bth_hour < 7)
                                        dms *= 4;
                                    else if (bth_hour >= 0 && bth_hour < 1)
                                        dms *= 3;
                                    else if (bth_hour >= 22)
                                        dms *= 2;
#ifndef HU_IS_TEST
                                    if (choreo_plan.segments[seg].show_typing_indicator &&
                                        ch->channel->vtable->start_typing)
                                        ch->channel->vtable->start_typing(ch->channel->ctx,
                                                                          batch_key, key_len);
                                    usleep((useconds_t)(dms * 1000));
#endif
                                }
                                const char *const *pv_ptr = (seg == 0 && all_send_media_cnt > 0)
                                                                ? all_send_media_ptr
                                                                : NULL;
                                size_t pv_cnt =
                                    (seg == 0 && all_send_media_cnt > 0) ? all_send_media_cnt : 0;
                                ch->channel->vtable->send(
                                    ch->channel->ctx, send_target, send_target_len,
                                    choreo_plan.segments[seg].text,
                                    choreo_plan.segments[seg].text_len, pv_ptr, pv_cnt);
                            }
                            hu_choreography_plan_free(alloc, &choreo_plan);
                        }

                        hu_message_fragment_t fragments[4];
                        size_t frag_count = 0;
                        if (!use_choreography) {
                            frag_count = hu_conversation_split_response(alloc, send_ptr, send_len,
                                                                        fragments, 4, split_max);
                        }
                        if (frag_count > 0) {
                            /* Stephanie2 active waiting: thinking + typing time per fragment */
                            for (size_t f = 0; f < frag_count; f++) {
                                if (f > 0) {
                                    /* Thinking time: 500-1500ms between fragments */
                                    uint32_t think_ms =
                                        500 + ((uint32_t)(f * 397 + send_len) % 1000);
                                    /* Typing time: ~60 WPM -> ~5 chars/sec */
                                    uint32_t type_ms = (uint32_t)(fragments[f].text_len * 200);
                                    if (type_ms > 3000)
                                        type_ms = 3000;
                                    uint32_t total_ms = think_ms + type_ms;
                                    if (fragments[f].delay_ms > total_ms)
                                        total_ms = fragments[f].delay_ms;
                                    /* Late-night: stretch inter-fragment delay too */
                                    if (bth_hour >= 1 && bth_hour < 7)
                                        total_ms = total_ms * 4;
                                    else if (bth_hour >= 0 && bth_hour < 1)
                                        total_ms = total_ms * 3;
                                    else if (bth_hour >= 22)
                                        total_ms = total_ms * 2;
                                    usleep((useconds_t)(total_ms * 1000));
                                }
#ifndef HU_IS_TEST
                                {
                                    const char *eff = hu_conversation_classify_effect(
                                        fragments[f].text, fragments[f].text_len);
                                    if (eff)
                                        hu_log_info("human", agent ? agent->observer : NULL,
                                                    "%s effect: %s (%.*s)", eff_ch, eff,
                                                    (int)(fragments[f].text_len > 60
                                                              ? 60
                                                              : fragments[f].text_len),
                                                    fragments[f].text);
                                }
#endif
                                const char *const *pv_ptr =
                                    (f == 0 && all_send_media_cnt > 0) ? all_send_media_ptr : NULL;
                                size_t pv_cnt =
                                    (f == 0 && all_send_media_cnt > 0) ? all_send_media_cnt : 0;

                                /* Burst cadence: split fragments into separate
                                 * bubbles for a more human texting feel. The
                                 * decision is channel-class-aware:
                                 *   - TEXT_FAST (iMessage/SMS): split short
                                 *     multi-sentence replies one sentence per
                                 *     bubble, AND chunk long prose (>120 chars)
                                 *   - TEXT_ASYNC and others: only chunk >120
                                 *     char prose (existing behavior)
                                 * See hu_conversation_split_for_cadence in
                                 * src/context/conversation.c for the full rules. */
                                bool did_double_text = false;
#ifndef HU_IS_TEST
                                {
                                    const char *cad_ch_name =
                                        ch->channel->vtable->name
                                            ? ch->channel->vtable->name(ch->channel->ctx)
                                            : NULL;
                                    hu_channel_class_t cad_cls =
                                        cad_ch_name ? hu_channel_class_for_name(cad_ch_name)
                                                    : HU_CHANNEL_CLASS_UNKNOWN;
                                    char dt_chunks[4][512];
                                    size_t dt_n = hu_conversation_split_for_cadence(
                                        fragments[f].text, fragments[f].text_len, cad_cls,
                                        dt_chunks, 4);
                                    if (dt_n >= 2) {
                                        did_double_text = true;
                                        for (size_t dt = 0; dt < dt_n; dt++) {
                                            if (dt > 0) {
                                                uint32_t dt_ms =
                                                    300 + (uint32_t)(strlen(dt_chunks[dt]) * 50);
                                                if (dt_ms > 2000)
                                                    dt_ms = 2000;
                                                usleep((useconds_t)(dt_ms * 1000));
                                            }
                                            ch->channel->vtable->send(
                                                ch->channel->ctx, batch_key, key_len, dt_chunks[dt],
                                                strlen(dt_chunks[dt]), (dt == 0) ? pv_ptr : NULL,
                                                (dt == 0) ? pv_cnt : 0);
                                        }
                                    }
                                }
#endif
                                if (!did_double_text)
                                    ch->channel->vtable->send(
                                        ch->channel->ctx, batch_key, key_len, fragments[f].text,
                                        fragments[f].text_len, pv_ptr, pv_cnt);
#if defined(HU_ENABLE_RL_FULL)
                                if (config && config->reaction_collection.enabled && f == 0 &&
                                    fragments[f].text && fragments[f].text_len > 0 &&
                                    ch->channel->vtable->name) {
                                    const char *ch_name =
                                        ch->channel->vtable->name(ch->channel->ctx);
                                    if (ch_name) {
                                        char msg_ref[96];
                                        msg_ref[0] = '\0';
                                        if (strcmp(ch_name, "imessage") == 0) {
                                            const char *db = NULL;
                                            if (config->reaction_collection.chatdb_path[0])
                                                db = config->reaction_collection.chatdb_path;
                                            else {
                                                const char *env = getenv("HU_CHATDB");
                                                if (env && env[0])
                                                    db = env;
                                            }
                                            if (!db) {
                                                static char home_db[512];
                                                const char *hm = getenv("HOME");
                                                if (hm && hm[0]) {
                                                    snprintf(home_db, sizeof(home_db),
                                                             "%s/Library/Messages/chat.db", hm);
                                                    db = home_db;
                                                }
                                            }
                                            if (db && hu_imessage_lookup_latest_sent_guid(
                                                          db, batch_key, fragments[f].text, msg_ref,
                                                          sizeof(msg_ref)) != HU_OK) {
                                                snprintf(msg_ref, sizeof(msg_ref), "out-%lld",
                                                         (long long)time(NULL));
                                            }
                                        } else {
                                            snprintf(msg_ref, sizeof(msg_ref), "out-%lld",
                                                     (long long)time(NULL));
                                        }
                                        hu_reaction_handler_register_assistant_message_for_production(
                                            ch_name, batch_key, msg_ref,
                                            combined[0] ? combined : "", fragments[f].text);
                                    }
                                }
#endif
                                if (pv_cnt > 0) {
                                    uint64_t pv_rec = (uint64_t)time(NULL) * 1000ULL;
                                    hu_visual_proactive_media_governor_record(pv_rec);
#ifndef HU_IS_TEST
                                    hu_daemon_visual_attach_gov_after_send(pv_rec);
#endif
                                }
                            }
                            for (size_t f = 0; f < frag_count; f++) {
                                if (fragments[f].text)
                                    alloc->free(alloc->ctx, fragments[f].text,
                                                fragments[f].text_len + 1);
                            }
                        } else {
#ifndef HU_IS_TEST
                            {
                                const char *eff =
                                    hu_conversation_classify_effect(send_ptr, send_len);
                                if (eff)
                                    hu_log_info("human", agent ? agent->observer : NULL,
                                                "%s effect: %s (%.*s)", eff_ch, eff,
                                                (int)(send_len > 60 ? 60 : send_len), send_ptr);
                            }
#endif
                            {
                                char *fmt_text = NULL;
                                size_t fmt_len = 0;
                                const char *send_text = send_ptr;
                                size_t send_text_len = send_len;
                                if (ch->channel->vtable->name) {
                                    const char *fmt_ch =
                                        ch->channel->vtable->name(ch->channel->ctx);
                                    if (fmt_ch &&
                                        hu_channel_format_outbound(alloc, fmt_ch, strlen(fmt_ch),
                                                                   send_ptr, send_len, &fmt_text,
                                                                   &fmt_len) == HU_OK &&
                                        fmt_text) {
                                        send_text = fmt_text;
                                        send_text_len = fmt_len;
                                    }
                                }
                                const char *const *pv_ptr =
                                    all_send_media_cnt > 0 ? all_send_media_ptr : NULL;
                                size_t pv_cnt = all_send_media_cnt;
                                ch->channel->vtable->send(ch->channel->ctx, send_target,
                                                          send_target_len, send_text, send_text_len,
                                                          pv_ptr, pv_cnt);
                                if (fmt_text)
                                    alloc->free(alloc->ctx, fmt_text, fmt_len + 1);
                                if (pv_cnt > 0) {
                                    uint64_t pv_rec = (uint64_t)time(NULL) * 1000ULL;
                                    hu_visual_proactive_media_governor_record(pv_rec);
#ifndef HU_IS_TEST
                                    hu_daemon_visual_attach_gov_after_send(pv_rec);
#endif
                                }
                            }
                        }
                        /* Send correction after main message (2.5–5s delay) */
                        if (original_response) {
                            if (has_typo_quirk && typo_seed != 0 && response && response_len > 0) {
                                char correction[128];
                                size_t corr_len = hu_conversation_generate_correction(
                                    original_response, original_len, response, response_len,
                                    correction, sizeof(correction), typo_seed + 1, 40);
                                if (corr_len > 0) {
                                    unsigned int delay_ms = 2500 + (unsigned int)(typo_seed % 2500);
                                    usleep((useconds_t)(delay_ms * 1000));
                                    ch->channel->vtable->send(ch->channel->ctx, send_target,
                                                              send_target_len, correction, corr_len,
                                                              NULL, 0);
                                    if (agent->bth_metrics)
                                        agent->bth_metrics->corrections_sent++;
                                }
                            }
                            alloc->free(alloc->ctx, original_response, original_len + 1);
                        }
#else
#ifndef HU_IS_TEST
                        {
                            const char *eff = hu_conversation_classify_effect(send_ptr, send_len);
                            if (eff)
                                hu_log_info("human", agent ? agent->observer : NULL,
                                            "%s effect: %s (%.*s)", eff_ch, eff,
                                            (int)(send_len > 60 ? 60 : send_len), send_ptr);
                        }
#endif
                        {
                            const char *send_ptr = (const char *)response;
                            size_t send_len = response_len;
                            char *fmt_text = NULL;
                            size_t fmt_len = 0;
                            const char *send_text = send_ptr;
                            size_t send_text_len = send_len;
                            if (ch->channel->vtable->name) {
                                const char *fmt_ch = ch->channel->vtable->name(ch->channel->ctx);
                                if (fmt_ch &&
                                    hu_channel_format_outbound(alloc, fmt_ch, strlen(fmt_ch),
                                                               send_ptr, send_len, &fmt_text,
                                                               &fmt_len) == HU_OK &&
                                    fmt_text) {
                                    send_text = fmt_text;
                                    send_text_len = fmt_len;
                                }
                            }
                            const char *const *pv_ptr =
                                all_send_media_cnt > 0 ? all_send_media_ptr : NULL;
                            size_t pv_cnt = all_send_media_cnt;
                            ch->channel->vtable->send(ch->channel->ctx, send_target,
                                                      send_target_len, send_text, send_text_len,
                                                      pv_ptr, pv_cnt);
                            if (fmt_text)
                                alloc->free(alloc->ctx, fmt_text, fmt_len + 1);
                            if (pv_cnt > 0) {
                                uint64_t pv_rec = (uint64_t)time(NULL) * 1000ULL;
                                hu_visual_proactive_media_governor_record(pv_rec);
#ifndef HU_IS_TEST
                                hu_daemon_visual_attach_gov_after_send(pv_rec);
#endif
                            }
                        }
#endif
                        if (send_buf_ack)
                            alloc->free(alloc->ctx, send_buf_ack, send_len + 1);
                    }
                }

                /* F32: Update style fingerprint with our sent response */
                if (agent->memory && batch_key && key_len > 0 && response && response_len > 0 &&
                    !llm_decides)
                    (void)hu_style_fingerprint_update(agent->memory, alloc, batch_key, key_len,
                                                      response, response_len);

#if !defined(HU_IS_TEST) && defined(HU_ENABLE_SQLITE)
                /* Turing score: evaluate response human-likeness post-send.
                 * Skip in llm_decides mode — heuristic scoring adds latency. */
                if (response && response_len > 0 && agent->memory && !llm_decides) {
                    hu_turing_score_t tscore;
                    hu_error_t ts_err = hu_turing_score_heuristic(
                        response, response_len, combined, combined_len,
                        agent->max_response_chars ? agent->max_response_chars : 0u, &tscore);
                    if (ts_err == HU_OK && agent->active_channel) {
                        hu_turing_apply_channel_weights(&tscore, agent->active_channel,
                                                        agent->active_channel_len);
                    }
                    if (ts_err == HU_OK && agent->persona && agent->persona->traits_count > 0) {
                        hu_turing_apply_persona_alignment(
                            &tscore, response, response_len,
                            (const char *const *)agent->persona->traits,
                            agent->persona->traits_count,
                            (const char *const *)agent->persona->preferred_vocab,
                            agent->persona->preferred_vocab_count,
                            (const char *const *)agent->persona->avoided_vocab,
                            agent->persona->avoided_vocab_count);
                    }
                    if (ts_err == HU_OK) {
                        if (agent->bth_metrics)
                            agent->bth_metrics->total_turns++;
                        /* Voice-aware scoring: when voice pipeline is active, give
                         * more weight to S2S voice dimensions (12-17) */
#if defined(HU_ENABLE_CARTESIA)
                        if (agent->persona && agent->persona->voice.voice_id[0] &&
                            agent->persona->voice_messages.enabled) {
                            int voice_sum = 0;
                            for (int vd = 12; vd < HU_TURING_DIM_COUNT; vd++)
                                voice_sum += tscore.dimensions[vd];
                            int voice_avg = voice_sum / 6;
                            int text_sum = 0;
                            for (int td = 0; td < 12; td++)
                                text_sum += tscore.dimensions[td];
                            int text_avg = text_sum / 12;
                            int blended = (text_avg * 60 + voice_avg * 40 + 50) / 100;
                            if (blended != tscore.overall) {
                                hu_log_info("human", agent ? agent->observer : NULL,
                                            "voice-aware turing: %d/10 (text=%d, voice=%d)",
                                            blended, text_avg, voice_avg);
                                tscore.overall = blended;
                                tscore.verdict = blended >= 8   ? HU_TURING_HUMAN
                                                 : blended >= 6 ? HU_TURING_BORDERLINE
                                                                : HU_TURING_AI_DETECTED;
                            }
                        }
#endif
                        hu_log_info("human", agent ? agent->observer : NULL,
                                    "turing: %d/10 [%s] for %.*s", tscore.overall,
                                    hu_turing_verdict_name(tscore.verdict),
                                    (int)(key_len > 20 ? 20 : key_len), batch_key);
                        sqlite3 *ts_db = hu_sqlite_memory_get_db(agent->memory);
                        if (ts_db) {
                            (void)hu_turing_init_tables(ts_db);
                            (void)hu_turing_store_score(ts_db, batch_key, key_len,
                                                        (int64_t)time(NULL), &tscore);
                        }

                        /* A/B test recording: record score for active tests */
                        if (ts_db && batch_key && key_len > 0) {
                            (void)hu_ab_test_init_table(ts_db);
                            static const char *ab_tests[] = {"disfluency_freq", "backchannel_prob",
                                                             "double_text_prob"};
                            for (size_t ab = 0; ab < 3; ab++) {
                                bool is_b =
                                    hu_ab_test_pick_variant(batch_key, key_len, ab_tests[ab]);
                                (void)hu_ab_test_record(ts_db, ab_tests[ab], is_b, tscore.overall);
                            }
                        }

                        /* DPO: record high-scoring responses as positive examples */
                        if (tscore.overall >= 8) {
                            hu_dpo_record_from_feedback(&agent->sota.dpo_collector, combined,
                                                        combined_len, response, response_len, true);
                        }
                    }
                }
#endif

#if !defined(HU_IS_TEST)
                /* F9: Double-text — natural afterthought follow-up.
                 * When llm_decides is active, use the fast classify provider. */
                {
                    const hu_provider_vtable_t *dt_vtable = (llm_decides && g_classify_provider_ok)
                                                                ? g_classify_provider.vtable
                                                                : agent->provider.vtable;
                    void *dt_ctx = (llm_decides && g_classify_provider_ok) ? g_classify_provider.ctx
                                                                           : agent->provider.ctx;
                    if (response && response_len > 0 && agent->persona &&
                        ch->channel->vtable->send && dt_vtable && dt_vtable->chat_with_system) {
                        float dt_prob = agent->persona->humanization.double_text_probability;
                        uint32_t dt_seed = (uint32_t)time(NULL) * 1103515245u + 12345u +
                                           (uint32_t)(uintptr_t)response;
                        if (hu_conversation_should_double_text(response, response_len,
                                                               history_entries, history_count,
                                                               bth_hour, dt_seed, dt_prob)) {
                            char dt_user[512];
                            int dt_n = snprintf(
                                dt_user, sizeof(dt_user),
                                "You just sent this message: \"%.*s\"\n"
                                "Add a brief, natural follow-up thought (1 short sentence max). "
                                "Something you'd double-text a moment later.",
                                (int)(response_len > 200 ? 200 : response_len), response);
                            if (dt_n > 0 && (size_t)dt_n < sizeof(dt_user)) {
                                char *dt_resp = NULL;
                                size_t dt_resp_len = 0;
                                const char *dt_model =
                                    (llm_decides && g_classify_provider_ok)
                                        ? g_classify_model
                                        : (agent->model_name ? agent->model_name
                                                             : "gemini-3.1-flash-lite-preview");
                                size_t dt_model_len =
                                    (llm_decides && g_classify_provider_ok)
                                        ? g_classify_model_len
                                        : (agent->model_name ? agent->model_name_len : 31);
                                hu_error_t dt_err = dt_vtable->chat_with_system(
                                    dt_ctx, alloc,
                                    "You are texting as this person. Keep it casual, short, "
                                    "lowercase. "
                                    "No quotes, no explanation, just the follow-up text.",
                                    93, dt_user, (size_t)dt_n, dt_model, dt_model_len, 0.9,
                                    &dt_resp, &dt_resp_len);
                                if (dt_err == HU_OK && dt_resp && dt_resp_len > 0 &&
                                    dt_resp_len < 200) {
                                    /* Post-process double-text through the same BTH pipeline */
                                    hu_validator_chain_apply_default_in_place(
                                        alloc, agent ? agent->observer : NULL, NULL, 0,
                                        "double-text send", dt_resp, &dt_resp_len, dt_resp_len + 1);
                                    if (dt_resp_len > 0) {
                                        dt_resp_len = hu_conversation_vary_complexity(
                                            dt_resp, dt_resp_len, dt_seed);
                                        if (dt_resp_len > 1 && dt_resp[0] >= 'A' &&
                                            dt_resp[0] <= 'Z' && dt_resp[1] >= 'a' &&
                                            dt_resp[1] <= 'z' && dt_resp[0] != 'I') {
                                            dt_resp[0] = (char)(dt_resp[0] + 32);
                                        }
                                        if (dt_resp_len > 1 && dt_resp[dt_resp_len - 1] == '.') {
                                            dt_resp[dt_resp_len - 1] = '\0';
                                            dt_resp_len--;
                                        }
                                        unsigned int dt_delay = 10000u + (dt_seed % 35000u);
                                        usleep((useconds_t)(dt_delay * 1000u));
                                        ch->channel->vtable->send(ch->channel->ctx, send_target,
                                                                  send_target_len, dt_resp,
                                                                  dt_resp_len, NULL, 0);
                                        if (agent->bth_metrics)
                                            agent->bth_metrics->double_texts++;
                                    }
                                }
                                if (dt_resp)
                                    alloc->free(alloc->ctx, dt_resp, dt_resp_len + 1);
                            }
                        }
                    }
                } /* end dt_vtable scope */

                /* Self-reaction: occasionally haha/emphasize own message (~2%).
                 * Skip for groups: get_latest_sent_rowid uses handle.id SQL. */
                if (response && response_len > 0 && ch->channel->vtable->react &&
                    !msgs[batch_start].is_group) {
                    hu_reaction_type_t self_r = hu_conversation_classify_self_reaction(
                        response, response_len, (uint32_t)time(NULL));
                    if (self_r != HU_REACTION_NONE) {
                        usleep(1500000 + ((uint32_t)time(NULL) % 3000000));
#ifdef HU_HAS_IMESSAGE
                        int64_t sent_id = hu_imessage_get_latest_sent_rowid(batch_key, key_len);
#else
                        int64_t sent_id = msgs[batch_end].message_id + 1;
#endif
                        if (sent_id > 0) {
                            ch->channel->vtable->react(ch->channel->ctx, send_target,
                                                       send_target_len, sent_id, self_r);
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "self-reaction on own message: %d", (int)self_r);
                        }
                    }
                }

                /* GIF/music calibration: skip for group chats where batch_key
                 * is a chat identifier rather than a handle.id — SQL queries
                 * would return wrong results. */
#ifdef HU_HAS_IMESSAGE
                if (!msgs[batch_start].is_group) {
                    {
                        int gif_taps = hu_imessage_count_recent_gif_tapbacks(batch_key, key_len);
                        if (gif_taps > 0) {
                            for (int gt = 0; gt < gif_taps; gt++)
                                hu_conversation_gif_cal_record_reaction(batch_key, key_len);
                            static uint64_t last_gif_cal_save_ms;
                            uint64_t gcnow = (uint64_t)time(NULL) * 1000ULL;
                            if (gcnow - last_gif_cal_save_ms > 30000) {
                                last_gif_cal_save_ms = gcnow;
                                const char *rh = getenv("HOME");
                                if (rh) {
                                    char rcp[512];
                                    int rn = snprintf(rcp, sizeof(rcp),
                                                      "%s/.human/gif_calibration.json", rh);
                                    if (rn > 0 && (size_t)rn < sizeof(rcp))
                                        hu_conversation_gif_cal_save(rcp, (size_t)rn);
                                }
                            }
                        }
                    }

                    /* Music taste: detect positive tapbacks on our music messages via SQL */
                    {
                        int music_taps =
                            hu_imessage_count_recent_music_tapbacks(batch_key, key_len);
                        if (music_taps > 0) {
                            for (int mt = 0; mt < music_taps; mt++)
                                hu_music_taste_record_reaction(batch_key, key_len);
                        }
                    }
                } /* !is_group guard for GIF/music calibration */
#endif

                /* Load calibration data on first pass */
                {
                    static bool gif_cal_loaded;
                    if (!gif_cal_loaded) {
                        gif_cal_loaded = true;
                        const char *gh = getenv("HOME");
                        if (gh) {
                            char gcp[512];
                            int gn =
                                snprintf(gcp, sizeof(gcp), "%s/.human/gif_calibration.json", gh);
                            if (gn > 0 && (size_t)gn < sizeof(gcp))
                                hu_conversation_gif_cal_load(gcp, (size_t)gn);
                        }
                    }
                    static bool music_taste_loaded;
                    if (!music_taste_loaded) {
                        music_taste_loaded = true;
                        const char *mh = getenv("HOME");
                        if (mh) {
                            char mtp[512];
                            int mn = snprintf(mtp, sizeof(mtp), "%s/.human/music_taste.json", mh);
                            if (mn > 0 && (size_t)mn < sizeof(mtp))
                                hu_music_taste_load(mtp, (size_t)mn);
                        }
                    }
                }

                /* GIF reaction: send a GIF when the moment calls for it */
                bool gif_sent_this_turn = false;
                if (combined_len > 0 && ch->channel->vtable->send) {
                    float gif_prob = 0.10f;
                    const char *contact_rel = NULL;
                    size_t contact_rel_len = 0;
                    if (agent->persona) {
                        gif_prob = agent->persona->humanization.gif_probability > 0.0f
                                       ? agent->persona->humanization.gif_probability
                                       : 0.10f;
                        const hu_contact_profile_t *gif_cp =
                            hu_persona_find_contact(agent->persona, batch_key, key_len);
                        if (gif_cp) {
                            contact_rel = gif_cp->relationship_type ? gif_cp->relationship_type
                                                                    : gif_cp->relationship;
                            contact_rel_len = contact_rel ? strlen(contact_rel) : 0;
                        }
                    }
                    gif_prob = hu_conversation_adjust_gif_probability(gif_prob, contact_rel,
                                                                      contact_rel_len);
                    /* Calibration: adjust based on historical hit rate for this contact */
                    float cal_rate = hu_conversation_gif_cal_hit_rate(batch_key, key_len);
                    if (cal_rate < 0.2f)
                        gif_prob *= 0.5f; /* they rarely react to our GIFs — send fewer */
                    else if (cal_rate > 0.7f)
                        gif_prob *= 1.3f; /* they love our GIFs — send more */
                    if (gif_prob > 1.0f)
                        gif_prob = 1.0f;
                    uint32_t gif_seed =
                        (uint32_t)time(NULL) * 2654435761u + (uint32_t)(uintptr_t)combined;
                    uint64_t gif_now_ms = (uint64_t)time(NULL) * 1000ULL;
                    if (hu_conversation_should_send_gif(combined, combined_len, history_entries,
                                                        history_count, gif_seed, gif_prob) &&
                        hu_conversation_gif_rate_allow(batch_key, key_len, gif_now_ms, 5, 600000)) {
                        const char *tenor_key =
                            config ? hu_config_get_provider_key(config, "tenor") : NULL;
                        if (tenor_key && tenor_key[0]) {
                            char gif_style[128];
                            size_t gs_len = hu_conversation_build_gif_style_hint(
                                contact_rel, contact_rel_len, gif_style, sizeof(gif_style));
                            char gif_prompt[640];
                            size_t gp_len = hu_conversation_build_gif_search_prompt(
                                combined, combined_len, gif_prompt, sizeof(gif_prompt));
                            if (gp_len > 0 && gs_len > 0 &&
                                gp_len + 1 + gs_len < sizeof(gif_prompt)) {
                                gif_prompt[gp_len] = ' ';
                                memcpy(gif_prompt + gp_len + 1, gif_style, gs_len);
                                gp_len += 1 + gs_len;
                                gif_prompt[gp_len] = '\0';
                            }
                            if (gp_len > 0) {
#ifdef HU_HAS_IMESSAGE
                                char *gif_query = NULL;
                                size_t gif_query_len = 0;
                                const char *gif_model = agent->model_name
                                                            ? agent->model_name
                                                            : "gemini-3.1-flash-lite-preview";
                                size_t gif_model_len =
                                    agent->model_name ? agent->model_name_len : 31;
                                if (agent->provider.vtable &&
                                    agent->provider.vtable->chat_with_system) {
                                    (void)agent->provider.vtable->chat_with_system(
                                        agent->provider.ctx, alloc,
                                        "Return ONLY a 2-4 word GIF search query. No quotes, no "
                                        "explanation.",
                                        68, gif_prompt, gp_len, gif_model, gif_model_len, 0.8,
                                        &gif_query, &gif_query_len);
                                }
                                if (gif_query && gif_query_len > 0 && gif_query_len < 100) {
                                    char *gif_path =
                                        hu_imessage_fetch_gif(alloc, gif_query, gif_query_len,
                                                              tenor_key, strlen(tenor_key));
                                    if (gif_path) {
                                        usleep(2000000 + (gif_seed % 3000000));
                                        const char *media[] = {gif_path};
                                        ch->channel->vtable->send(ch->channel->ctx, batch_key,
                                                                  key_len, "", 0, media, 1);
                                        (void)unlink(gif_path);
                                        hu_conversation_gif_rate_record(batch_key, key_len,
                                                                        gif_now_ms);
                                        hu_conversation_gif_cal_record_send(
                                            batch_key, key_len, gif_query, gif_query_len);
                                        {
                                            const char *cal_home = getenv("HOME");
                                            if (cal_home) {
                                                char cal_path[512];
                                                int cp_n = snprintf(
                                                    cal_path, sizeof(cal_path),
                                                    "%s/.human/gif_calibration.json", cal_home);
                                                if (cp_n > 0 && (size_t)cp_n < sizeof(cal_path))
                                                    hu_conversation_gif_cal_save(cal_path,
                                                                                 (size_t)cp_n);
                                            }
                                        }
                                        gif_sent_this_turn = true;
                                        hu_log_info("human", agent ? agent->observer : NULL,
                                                    "sent GIF: query=\"%.*s\"", (int)gif_query_len,
                                                    gif_query);
                                        size_t gp_path_len = strlen(gif_path);
                                        alloc->free(alloc->ctx, gif_path, gp_path_len + 1);
                                    }
                                }
                                if (gif_query)
                                    alloc->free(alloc->ctx, gif_query, gif_query_len + 1);
#endif /* HU_HAS_IMESSAGE */
                            }
                        }
                    }
                }

                /* Sticker-like reaction images: send expressive images for
                 * emotionally resonant messages (only if GIF wasn't sent) */
                if (combined_len > 0 && ch->channel->vtable->send && !gif_sent_this_turn) {
                    float sticker_prob = 0.08f;
                    if (agent->persona && agent->persona->humanization.gif_probability > 0.0f)
                        sticker_prob = agent->persona->humanization.gif_probability * 0.4f;
                    uint32_t stk_seed =
                        (uint32_t)time(NULL) * 48271u + (uint32_t)(uintptr_t)combined;
                    const char *last_resp = response;
                    size_t last_resp_len = response ? response_len : 0;
                    if (hu_conversation_should_send_sticker(combined, combined_len, last_resp,
                                                            last_resp_len, stk_seed,
                                                            sticker_prob)) {
                        const char *home = getenv("HOME");
                        if (home) {
                            char stk_dir[512];
                            int sd_n =
                                snprintf(stk_dir, sizeof(stk_dir), "%s/.human/stickers", home);
                            if (sd_n > 0 && (size_t)sd_n < sizeof(stk_dir)) {
                                char stk_path[640];
                                size_t sp_len = hu_conversation_select_sticker(
                                    combined, combined_len, stk_seed, stk_dir, (size_t)sd_n,
                                    stk_path, sizeof(stk_path));
                                if (sp_len > 0 && access(stk_path, R_OK) == 0) {
                                    usleep(1500000 + (stk_seed % 2000000));
                                    const char *media[] = {stk_path};
                                    ch->channel->vtable->send(ch->channel->ctx, send_target,
                                                              send_target_len, "", 0, media, 1);
                                    hu_log_info("human", agent ? agent->observer : NULL,
                                                "sent sticker: %s", stk_path);
                                }
                            }
                        }
                    }
                }

                /* Music teaser: share a song with 30s preview + artwork */
                if (combined_len > 0 && ch->channel->vtable->send && !gif_sent_this_turn) {
                    float music_prob = 0.05f;
                    if (agent->persona) {
                        music_prob = agent->persona->humanization.gif_probability > 0.0f
                                         ? agent->persona->humanization.gif_probability * 0.3f
                                         : 0.05f;
                    }
                    /* Boost probability if taste hit rate is high for this contact */
                    float taste_rate = hu_music_taste_hit_rate(batch_key, key_len);
                    if (taste_rate > 0.5f && music_prob < 0.15f)
                        music_prob = 0.15f;

                    uint32_t music_seed =
                        (uint32_t)time(NULL) * 16807u + (uint32_t)(uintptr_t)combined;
                    if (hu_conversation_should_send_music(combined, combined_len, history_entries,
                                                          history_count, music_seed, music_prob)) {
                        /* Build taste-enriched prompt */
                        char taste_snippet[256] = {0};
                        size_t taste_len = hu_music_taste_build_prompt(
                            batch_key, key_len, taste_snippet, sizeof(taste_snippet));

                        char music_prompt[768];
                        size_t mp_len = hu_conversation_build_music_prompt(
                            combined, combined_len, music_prompt, sizeof(music_prompt));

                        /* Append taste context to prompt if available */
                        if (taste_len > 0 && mp_len > 0 &&
                            mp_len + taste_len + 2 < sizeof(music_prompt)) {
                            music_prompt[mp_len++] = '\n';
                            memcpy(music_prompt + mp_len, taste_snippet, taste_len);
                            mp_len += taste_len;
                            music_prompt[mp_len] = '\0';
                        }

                        if (mp_len > 0 && agent->provider.vtable &&
                            agent->provider.vtable->chat_with_system) {
                            char *music_suggestion = NULL;
                            size_t music_suggestion_len = 0;
                            static const char music_sys[] =
                                "Suggest ONE song that fits the conversation mood. "
                                "Return in this exact format:\n"
                                "ARTIST - TITLE | your brief casual message\n"
                                "Example: Radiohead - Everything In Its Right Place | "
                                "this track is perfect for right now\n"
                                "Keep the message under 80 chars. No quotes, no URLs.";
                            const char *music_model = agent->model_name
                                                          ? agent->model_name
                                                          : "gemini-3.1-flash-lite-preview";
                            size_t music_model_len = agent->model_name ? agent->model_name_len : 31;
                            (void)agent->provider.vtable->chat_with_system(
                                agent->provider.ctx, alloc, music_sys, sizeof(music_sys) - 1,
                                music_prompt, mp_len, music_model, music_model_len, 0.9,
                                &music_suggestion, &music_suggestion_len);

                            hu_moderation_result_t music_mod = {0};
                            if (hu_moderation_check_local(alloc, music_suggestion,
                                                          music_suggestion_len,
                                                          &music_mod) == HU_OK &&
                                music_mod.flagged) {
                                hu_log_warn("human", agent ? agent->observer : NULL,
                                            "music teaser blocked by moderation");
                            } else if (music_suggestion && music_suggestion_len > 0 &&
                                       music_suggestion_len < 300) {
                                char search_query[256];
                                char casual_msg[256];
                                bool parsed = hu_music_parse_suggestion(
                                    music_suggestion, music_suggestion_len, search_query,
                                    sizeof(search_query), casual_msg, sizeof(casual_msg));

                                if (parsed && search_query[0] != '\0') {
                                    /* Detect user's streaming preference from history */
                                    hu_music_source_t pref = HU_MUSIC_SOURCE_ITUNES;
                                    if (history_entries && history_count > 0) {
                                        enum { MUSIC_HIST_CAP = 20 };
                                        char music_texts[MUSIC_HIST_CAP][512];
                                        size_t music_lens[MUSIC_HIST_CAP];
                                        const char *music_pref_ptrs[MUSIC_HIST_CAP];
                                        size_t music_n = history_count < (size_t)MUSIC_HIST_CAP
                                                             ? history_count
                                                             : (size_t)MUSIC_HIST_CAP;
                                        for (size_t mi = 0; mi < music_n; mi++) {
                                            size_t tlen = strnlen(history_entries[mi].text, 511);
                                            memcpy(music_texts[mi], history_entries[mi].text, tlen);
                                            music_texts[mi][tlen] = '\0';
                                            music_lens[mi] = tlen;
                                            music_pref_ptrs[mi] = music_texts[mi];
                                        }
                                        pref = hu_music_detect_preference(music_pref_ptrs,
                                                                          music_lens, music_n);
                                    }

                                    /* Always search iTunes (for the .m4a preview) */
                                    hu_music_result_t song = {0};
                                    size_t sq_len = strlen(search_query);
                                    hu_error_t search_err =
                                        hu_music_search(alloc, search_query, sq_len, &song);

                                    /* If user prefers Spotify, try to get the Spotify share link */
                                    hu_music_result_t spotify_song = {0};
                                    bool has_spotify = false;
                                    if (pref == HU_MUSIC_SOURCE_SPOTIFY) {
                                        const char *sp_cred =
                                            config ? hu_config_get_provider_key(config, "spotify")
                                                   : NULL;
                                        if (sp_cred) {
                                            /* Expect "client_id:client_secret" format */
                                            const char *colon = strchr(sp_cred, ':');
                                            if (colon) {
                                                char sp_id[128] = {0};
                                                size_t id_len = (size_t)(colon - sp_cred);
                                                if (id_len < sizeof(sp_id)) {
                                                    memcpy(sp_id, sp_cred, id_len);
                                                    has_spotify =
                                                        hu_music_search_spotify(
                                                            alloc, sp_id, colon + 1, search_query,
                                                            sq_len, &spotify_song) == HU_OK;
                                                }
                                            }
                                        }
                                    }

                                    /* Use Spotify URL if available, iTunes preview for audio */
                                    hu_music_result_t *link_song =
                                        has_spotify ? &spotify_song : &song;

                                    if (search_err == HU_OK &&
                                        (song.track_view_url ||
                                         (has_spotify && spotify_song.track_view_url))) {
                                        /* Rich-link mode: when the channel auto-unfurls bare URLs
                                         * (iMessage, Telegram, Discord, Slack, WhatsApp, Signal),
                                         * send the URL alone in its own bubble. The platform
                                         * renders the full rich card — album art, title, artist,
                                         * play button — from the URL. No .m4a download, no JPG
                                         * download, no caption (caption inline kills the unfurl).
                                         *
                                         * INVARIANT: the URL bubble body must be exactly the URL
                                         * bytes — no preamble, no trailing whitespace, no caption.
                                         * Pinned by tests/test_imessage_rich_link.c. */
                                        bool rich_link =
                                            hu_channel_supports_link_unfurl(ch->channel) &&
                                            link_song->track_view_url != NULL;

                                        if (rich_link) {
                                            const char *url = link_song->track_view_url;
                                            size_t url_len = strlen(url);

                                            /* Same human-pacing delay as the legacy path so the
                                             * share lands in a natural conversational rhythm. */
                                            usleep(3000000 + (music_seed % 4000000));

                                            ch->channel->vtable->send(ch->channel->ctx, batch_key,
                                                                      key_len, url, url_len, NULL,
                                                                      0);

                                            hu_log_info("human", agent ? agent->observer : NULL,
                                                        "sent music rich-link: %s - %s [%s]",
                                                        song.artist_name ? song.artist_name : "?",
                                                        song.track_name ? song.track_name : "?",
                                                        has_spotify ? "spotify" : "itunes");

                                            hu_music_taste_record_send(batch_key, key_len,
                                                                       song.artist_name,
                                                                       song.track_name);
                                            {
                                                static uint64_t last_taste_save_ms;
                                                uint64_t tnow = (uint64_t)time(NULL) * 1000ULL;
                                                if (tnow - last_taste_save_ms > 30000) {
                                                    last_taste_save_ms = tnow;
                                                    const char *th = getenv("HOME");
                                                    if (th) {
                                                        char tp[512];
                                                        int tn2 = snprintf(
                                                            tp, sizeof(tp),
                                                            "%s/.human/music_taste.json", th);
                                                        if (tn2 > 0 && (size_t)tn2 < sizeof(tp))
                                                            hu_music_taste_save(tp, (size_t)tn2);
                                                    }
                                                }
                                            }
                                        } else {
                                            /* Legacy: channel doesn't unfurl URLs (SMS etc.).
                                             * Build a text caption and attach the .m4a preview
                                             * + JPG artwork so the recipient still gets media. */
                                            char share_text[512];
                                            size_t casual_len = strlen(casual_msg);
                                            size_t st_len = hu_music_build_share_text(
                                                link_song, casual_len > 0 ? casual_msg : NULL,
                                                casual_len, share_text, sizeof(share_text));

                                            char preview_path[256] = {0};
                                            bool has_preview = false;
                                            if (song.preview_url) {
                                                has_preview =
                                                    hu_music_download_preview(
                                                        alloc, song.preview_url, preview_path,
                                                        sizeof(preview_path)) == HU_OK;
                                            }

                                            char artwork_path[256] = {0};
                                            bool has_artwork = false;
                                            const char *art_url = link_song->artwork_url
                                                                      ? link_song->artwork_url
                                                                      : song.artwork_url;
                                            if (art_url) {
                                                has_artwork = hu_music_download_artwork(
                                                                  alloc, art_url, artwork_path,
                                                                  sizeof(artwork_path)) == HU_OK;
                                            }

                                            usleep(3000000 + (music_seed % 4000000));

                                            if (st_len > 0) {
                                                int media_count = 0;
                                                const char *media[2];
                                                if (has_preview)
                                                    media[media_count++] = preview_path;
                                                if (has_artwork)
                                                    media[media_count++] = artwork_path;

                                                ch->channel->vtable->send(
                                                    ch->channel->ctx, batch_key, key_len,
                                                    share_text, st_len,
                                                    media_count > 0 ? media : NULL,
                                                    (size_t)media_count);

                                                hu_log_info("human", agent ? agent->observer : NULL,
                                                            "sent music %s: %s - %s [%s%s]",
                                                            has_preview ? "preview" : "link",
                                                            song.artist_name ? song.artist_name
                                                                             : "?",
                                                            song.track_name ? song.track_name : "?",
                                                            has_spotify ? "spotify" : "itunes",
                                                            has_artwork ? "+art" : "");

                                                hu_music_taste_record_send(batch_key, key_len,
                                                                           song.artist_name,
                                                                           song.track_name);
                                                {
                                                    static uint64_t last_taste_save_ms;
                                                    uint64_t tnow = (uint64_t)time(NULL) * 1000ULL;
                                                    if (tnow - last_taste_save_ms > 30000) {
                                                        last_taste_save_ms = tnow;
                                                        const char *th = getenv("HOME");
                                                        if (th) {
                                                            char tp[512];
                                                            int tn2 = snprintf(
                                                                tp, sizeof(tp),
                                                                "%s/.human/music_taste.json", th);
                                                            if (tn2 > 0 && (size_t)tn2 < sizeof(tp))
                                                                hu_music_taste_save(tp,
                                                                                    (size_t)tn2);
                                                        }
                                                    }
                                                }
                                            }

                                            if (has_preview)
                                                (void)unlink(preview_path);
                                            if (has_artwork)
                                                (void)unlink(artwork_path);
                                        }
                                    } else {
                                        hu_log_info("human", agent ? agent->observer : NULL,
                                                    "music search failed for: %s", search_query);
                                    }
                                    hu_music_result_free(alloc, &song);
                                    if (has_spotify)
                                        hu_music_result_free(alloc, &spotify_song);
                                }
                            }
                            if (music_suggestion)
                                alloc->free(alloc->ctx, music_suggestion, music_suggestion_len + 1);
                        }
                    }
                }

                /* Proactive image generation: occasionally create and send an image */
                if (combined_len > 0 && ch->channel->vtable->send && !gif_sent_this_turn &&
                    agent->provider.vtable && agent->provider.vtable->chat_with_system) {
                    float img_prob = 0.02f;
                    float roll_img = (float)(rand() % 10000) / 10000.0f;
                    if (roll_img < img_prob && getenv("OPENAI_API_KEY")) {
                        static const char img_sys[] =
                            "Given the conversation, suggest a fun image to generate "
                            "and share. Return ONLY a DALL-E prompt (under 200 chars) or SKIP.\n"
                            "Example: A cozy cat reading a tiny newspaper with coffee\n"
                            "Only suggest when the moment genuinely calls for a visual — "
                            "funny, sweet, or illustrative. Reply SKIP if it doesn't fit.";
                        char *img_suggest = NULL;
                        size_t img_suggest_len = 0;
                        const char *img_model =
                            agent->model_name ? agent->model_name : "gemini-3.1-flash-lite-preview";
                        size_t img_model_len = agent->model_name ? agent->model_name_len : 31;
                        (void)agent->provider.vtable->chat_with_system(
                            agent->provider.ctx, alloc, img_sys, sizeof(img_sys) - 1, combined,
                            combined_len, img_model, img_model_len, 0.7, &img_suggest,
                            &img_suggest_len);
                        if (img_suggest && img_suggest_len > 0 && img_suggest_len < 300 &&
                            strstr(img_suggest, "SKIP") == NULL) {
                            char img_path[512];
                            hu_error_t ierr = hu_image_gen_download(
                                alloc, img_suggest, img_suggest_len, img_path, sizeof(img_path));
                            if (ierr == HU_OK) {
                                const char *img_media[] = {img_path};
                                ch->channel->vtable->send(ch->channel->ctx, send_target,
                                                          send_target_len, NULL, 0, img_media, 1);
                                hu_log_info("human", agent ? agent->observer : NULL,
                                            "proactive image sent: %.*s", (int)img_suggest_len,
                                            img_suggest);
                                (void)unlink(img_path);
                            }
                        }
                        if (img_suggest)
                            alloc->free(alloc->ctx, img_suggest, img_suggest_len + 1);
                    }
                }
#endif

#ifndef HU_IS_TEST
            skip_send:
                if (original_response)
                    alloc->free(alloc->ctx, original_response, original_len + 1);
                if (ch && ch->channel && ch->channel->vtable && ch->channel->vtable->stop_typing) {
                    ch->channel->vtable->stop_typing(ch->channel->ctx, batch_key, key_len);
                }
#endif
                if (response) {
                    /* Bump consecutive response counter for this contact */
                    if (consec_idx == SIZE_MAX && consec_contact_count < HU_CONSEC_MAX_CONTACTS &&
                        key_len < sizeof(consec_contact_keys[0])) {
                        consec_idx = consec_contact_count++;
                        memcpy(consec_contact_keys[consec_idx], batch_key, key_len);
                        consec_contact_keys[consec_idx][key_len] = '\0';
                        consec_response_count[consec_idx] = 0;
                    }
                    if (consec_idx != SIZE_MAX)
                        consec_response_count[consec_idx]++;

                    /* Clean up tool-generated media paths after sending */
                    if (agent) {
                        for (size_t gmc = 0; gmc < agent->generated_media_count; gmc++) {
                            if (agent->generated_media[gmc]) {
                                (void)unlink(agent->generated_media[gmc]);
                                agent->alloc->free(agent->alloc->ctx, agent->generated_media[gmc],
                                                   strlen(agent->generated_media[gmc]) + 1);
                                agent->generated_media[gmc] = NULL;
                            }
                        }
                        agent->generated_media_count = 0;
                    }

                    agent->alloc->free(agent->alloc->ctx, response, response_alloc_len + 1);
                }
#ifndef HU_IS_TEST
                if (history_entries) {
                    alloc->free(alloc->ctx, history_entries,
                                history_count * sizeof(hu_channel_history_entry_t));
                    history_entries = NULL;
                    history_count = 0;
                }
#endif
            }
        }

#ifndef HU_IS_TEST
        if (inbox_watcher.memory) {
            struct timespec ts_inbox;
            clock_gettime(CLOCK_MONOTONIC, &ts_inbox);
            int64_t inbox_now = (int64_t)ts_inbox.tv_sec * 1000 + ts_inbox.tv_nsec / 1000000;
            if (inbox_now - last_inbox_poll_ms >= 60000) {
                size_t ingested = 0;
                hu_error_t poll_err = hu_inbox_poll(&inbox_watcher, &ingested);
                if (poll_err != HU_OK)
                    hu_log_error("human", agent ? agent->observer : NULL, "inbox: poll error %s",
                                 hu_error_string(poll_err));
                else if (ingested > 0)
                    hu_log_info("human", agent ? agent->observer : NULL,
                                "inbox: ingested %zu file(s)", ingested);
                last_inbox_poll_ms = inbox_now;
            }
        }
#if HU_HAS_PWA
        if (pwa_learner && tick_now - pwa_learn_last_ms >= pwa_learn_interval_ms) {
            pwa_learn_last_ms = tick_now;
            size_t ingested = 0;
            hu_pwa_learner_scan(pwa_learner, &ingested);
            if (ingested > 0 && getenv("HU_DEBUG"))
                hu_log_info("human", agent ? agent->observer : NULL,
                            "PWA learner ingested %zu items", ingested);
        }
#endif
#endif

        /* Channel health monitor tick (every 30s by default) */
        if (chan_monitor) {
            int64_t mon_now = (int64_t)time(NULL);
            if (mon_now - chan_monitor_last_ts >= 30) {
                hu_channel_monitor_tick(chan_monitor, mon_now);
                chan_monitor_last_ts = mon_now;
            }
        }

        /* Plan 2: Background observer registry tick */
        {
            int64_t bg_now = (int64_t)time(NULL);
            if (bg_now - bg_last_tick_epoch >= 60) {
                hu_bg_registry_tick_all(&bg_registry, alloc, NULL);
                bg_last_tick_epoch = bg_now;
            }
        }

#ifdef HU_HAS_IMESSAGE
        /* iMessage health watchdog (every 30s).
         *
         * The breaker (hu_imessage_breaker_*) only catches sqlite-level
         * failures on chat.db open. The watchdog also catches the silent
         * "process is alive but poll loop has not produced a successful read
         * in a while" failure mode, which on macOS most commonly means imsg
         * watch is hung or chat.db lock contention is starving us. Edge-
         * triggered logging means at most one transition line per state
         * change — no log spam if the daemon sits in OK or STALLED for
         * hours. */
        {
            static int64_t imsg_wd_last_epoch = 0;
            int64_t wd_now = (int64_t)time(NULL);
            if (wd_now - imsg_wd_last_epoch >= 30) {
                imsg_wd_last_epoch = wd_now;
                const char *env_thresh = getenv("HU_IMESSAGE_STALL_SECS");
                int64_t threshold = env_thresh && env_thresh[0] ? atoll(env_thresh)
                                                                : HU_IMESSAGE_DEFAULT_STALL_SECS;
                if (threshold <= 0)
                    threshold = HU_IMESSAGE_DEFAULT_STALL_SECS;
                for (size_t ci = 0; ci < channel_count; ci++) {
                    if (channels[ci].poll_fn == hu_imessage_poll && channels[ci].channel)
                        hu_imessage_watchdog_tick(channels[ci].channel, wd_now, threshold);
                }
            }
        }
#endif

#if defined(HU_ENABLE_RL_FULL) && defined(HU_HAS_IMESSAGE) && defined(__APPLE__) && \
    defined(HU_ENABLE_SQLITE)
        if (config && config->reaction_collection.enabled) {
            static int64_t reaction_watermark = 0;
            static int64_t reaction_last_poll_unix = 0;
            int64_t now_unix = (int64_t)time(NULL);
            if (reaction_watermark == 0)
                reaction_watermark = now_unix;
            (void)hu_daemon_tick_reaction_poll(&config->reaction_collection, now_unix,
                                               &reaction_last_poll_unix, &reaction_watermark);
        }
#endif

        struct timespec sleep_ts = {.tv_sec = tick_interval_ms / 1000,
                                    .tv_nsec = (long)(tick_interval_ms % 1000) * 1000000L};
        nanosleep(&sleep_ts, NULL);
    }

#undef HU_STOP_FLAG
    /* Phase 3: clean up inner thought store */
    if (inner_thought_store_ok) {
        hu_inner_thought_store_deinit(&inner_thought_store);
        inner_thought_store_ok = false;
    }

    hu_bus_unsubscribe(&daemon_outbound_bus, daemon_outbound_bus_cb, &daemon_out_bus_bridge);
    hu_bus_deinit(&daemon_outbound_bus);
    hu_inbox_deinit(&inbox_watcher);
    hu_bg_registry_deinit(&bg_registry, alloc);
    if (chan_monitor)
        hu_channel_monitor_destroy(chan_monitor);
    if (agent)
        agent->bth_metrics = NULL;
    /* FIX 13: close W14 scheduler BEFORE W7 facade. The scheduler borrows
     * the facade's hu_memory_t handle for its SQLite job queue, so the
     * facade must outlive every tick the scheduler will ever run. */
#if defined(HU_ENABLE_RL_FULL)
    hu_reaction_handler_set_collector(NULL);
#endif
    /* Phase 1c teardown: detach the personal-model sink. */
    hu_reaction_handler_set_personal_model(NULL);
    if (agent && agent->w14_scheduler) {
        hu_w14_scheduler_close(agent->w14_scheduler, alloc);
        agent->w14_scheduler = NULL;
    }
    /* FIX 12: close W7 facade BEFORE the graph it borrows. The facade does
     * not own the graph (the v1 backend takes a borrow), so closing in this
     * order is just hygiene -- but it keeps the assertion footprint clean. */
    if (agent && agent->w7_facade) {
        hu_w7_facade_close(agent->w7_facade, alloc);
        agent->w7_facade = NULL;
    }
    if (agent && agent->verifier_graph == graph)
        agent->verifier_graph = NULL;
    if (daemon_keystore) {
        hu_keystore_close(daemon_keystore, alloc);
        daemon_keystore = NULL;
    }
    if (graph)
        hu_graph_close(graph, alloc);
    if (agent && agent->outcomes == &daemon_outcomes)
        agent->outcomes = NULL;
    return HU_OK;
#endif /* HU_IS_TEST */
}

/* Daemon management, PID files, and platform install moved to daemon_lifecycle.c.
 * Public API: hu_daemon_start/stop/status, hu_daemon_write_pid/remove_pid,
 * hu_daemon_install/uninstall/logs (all in daemon.h).
 * Internal: hu_daemon_validate_home, hu_daemon_get_pid_path (daemon_lifecycle.h). */
