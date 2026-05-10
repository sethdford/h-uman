#include "human/agent/humanness.h"
#include "human/agent.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/platform.h"
#include <string.h>
#include <time.h>

#include "human/persona/life_sim.h"
#include "human/persona/style_clone.h"
#include "human/persona/voice_maturity.h"

#if defined(HU_ENABLE_SQLITE) && HU_ENABLE_SQLITE
#include "human/persona/mood.h"
#endif

#define HUMANNESS_BUF_INIT 2048

static hu_error_t buf_append(hu_allocator_t *alloc, char **buf, size_t *len, size_t *cap,
                             const char *data, size_t data_len) {
    if (!data || data_len == 0)
        return HU_OK;
    while (*len + data_len + 1 > *cap) {
        size_t new_cap = *cap * 2;
        char *nb = (char *)alloc->alloc(alloc->ctx, new_cap);
        if (!nb)
            return HU_ERR_OUT_OF_MEMORY;
        if (*buf) {
            memcpy(nb, *buf, *len);
            alloc->free(alloc->ctx, *buf, *cap);
        }
        *buf = nb;
        *cap = new_cap;
    }
    memcpy(*buf + *len, data, data_len);
    *len += data_len;
    (*buf)[*len] = '\0';
    return HU_OK;
}

hu_error_t hu_agent_build_turn_context(hu_agent_t *agent) {
    if (!agent || !agent->alloc)
        return HU_ERR_INVALID_ARGUMENT;

    if (agent->conversation_context && agent->conversation_context_len > 0)
        return HU_OK;

    hu_allocator_t *alloc = agent->alloc;
    char *buf = NULL;
    size_t len = 0;
    size_t cap = 0;

    buf = (char *)alloc->alloc(alloc->ctx, HUMANNESS_BUF_INIT);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    cap = HUMANNESS_BUF_INIT;
    buf[0] = '\0';

    /* 1. Time-of-day overlay */
#ifndef HU_IS_TEST
    {
        time_t now_ts = time(NULL);
        struct tm tm_buf;
        struct tm *lt = hu_platform_localtime_r(&now_ts, &tm_buf);
        if (lt) {
            const char *overlay = NULL;
            size_t overlay_len = 0;
            if (lt->tm_hour >= 22 || (lt->tm_hour >= 0 && lt->tm_hour < 1)) {
                static const char late[] =
                    "\nIt's late at night. You can be more relaxed, introspective, "
                    "slightly more open than during the day. If it feels natural, share "
                    "something personal or vulnerable. Late-night texts are more intimate.";
                overlay = late;
                overlay_len = sizeof(late) - 1;
            } else if (lt->tm_hour >= 1 && lt->tm_hour < 7) {
                static const char wee[] =
                    "\nIt's the middle of the night. Keep it brief and low-energy "
                    "unless the other person is clearly awake and chatty.";
                overlay = wee;
                overlay_len = sizeof(wee) - 1;
            } else if (lt->tm_hour >= 7 && lt->tm_hour < 9) {
                static const char morning[] =
                    "\nIt's early morning \xe2\x80\x94 you just woke up. Keep responses brief, "
                    "practical, slightly groggy. Short sentences. No deep philosophical "
                    "conversations yet.";
                overlay = morning;
                overlay_len = sizeof(morning) - 1;
            }
            if (overlay)
                (void)buf_append(alloc, &buf, &len, &cap, overlay, overlay_len);
        }
    }
#endif

    /* 2. Mood directive (SQLite-backed, persona-dependent) */
#if defined(HU_ENABLE_SQLITE) && HU_ENABLE_SQLITE
    if (agent->memory) {
        hu_mood_state_t mood_state;
        memset(&mood_state, 0, sizeof(mood_state));
        if (hu_mood_get_current(alloc, agent->memory, &mood_state) == HU_OK) {
            size_t mood_len = 0;
            char *mood_dir = hu_mood_build_directive(alloc, &mood_state, &mood_len);
            if (mood_dir && mood_len > 0) {
                (void)buf_append(alloc, &buf, &len, &cap, "\n", 1);
                (void)buf_append(alloc, &buf, &len, &cap, mood_dir, mood_len);
                alloc->free(alloc->ctx, mood_dir, mood_len + 1);
            } else if (mood_dir) {
                alloc->free(alloc->ctx, mood_dir, mood_len + 1);
            }
        }
    }
#endif

    /* 3. Life sim context */
    if (agent->persona && (agent->persona->daily_routine.weekday_count > 0 ||
                           agent->persona->daily_routine.weekend_count > 0)) {
#ifndef HU_IS_TEST
        time_t now_ts = time(NULL);
        struct tm tm_buf;
        struct tm *lt = hu_platform_localtime_r(&now_ts, &tm_buf);
        int dow = lt ? lt->tm_wday : 0;
        uint32_t seed = (uint32_t)now_ts * 1103515245u + 12345u;
        hu_life_sim_state_t ls_state =
            hu_life_sim_get_current(&agent->persona->daily_routine, (int64_t)now_ts, dow, seed);
        size_t ls_len = 0;
        char *ls_ctx = hu_life_sim_build_context(alloc, &ls_state, &ls_len);
        if (ls_ctx && ls_len > 0) {
            (void)buf_append(alloc, &buf, &len, &cap, "\n", 1);
            (void)buf_append(alloc, &buf, &len, &cap, ls_ctx, ls_len);
            alloc->free(alloc->ctx, ls_ctx, ls_len + 1);
        } else if (ls_ctx) {
            alloc->free(alloc->ctx, ls_ctx, ls_len + 1);
        }
#endif
    }

    /* 4. Voice maturity guidance */
    if (agent->persona) {
        if (!agent->voice_profile_initialized) {
            hu_voice_profile_init(&agent->voice_profile);
            agent->voice_profile_initialized = true;
        }
        char *vm_ctx = NULL;
        size_t vm_ctx_len = 0;
        if (hu_voice_build_guidance(&agent->voice_profile, alloc, &vm_ctx, &vm_ctx_len) == HU_OK &&
            vm_ctx && vm_ctx_len > 0) {
            (void)buf_append(alloc, &buf, &len, &cap, "\n", 1);
            (void)buf_append(alloc, &buf, &len, &cap, vm_ctx, vm_ctx_len);
            alloc->free(alloc->ctx, vm_ctx, vm_ctx_len + 1);
        } else if (vm_ctx) {
            alloc->free(alloc->ctx, vm_ctx, vm_ctx_len + 1);
        }
    }

    /* 5. Style clone from conversation history */
    if (agent->persona && agent->history && agent->history_count > 0) {
        const char *own_msgs[512];
        size_t own_count = 0;
        for (size_t i = 0; i < agent->history_count && own_count < 512; i++) {
            if (agent->history[i].role == HU_ROLE_ASSISTANT && agent->history[i].content &&
                agent->history[i].content_len > 0)
                own_msgs[own_count++] = agent->history[i].content;
        }
        if (own_count >= 10) {
            char *clone_prompt = NULL;
            size_t clone_len = 0;
            if (hu_style_clone_from_history(alloc, own_msgs, own_count, &clone_prompt,
                                            &clone_len) == HU_OK &&
                clone_prompt && clone_len > 0) {
                (void)buf_append(alloc, &buf, &len, &cap, "\n", 1);
                (void)buf_append(alloc, &buf, &len, &cap, clone_prompt, clone_len);
                alloc->free(alloc->ctx, clone_prompt, clone_len + 1);
            } else if (clone_prompt) {
                alloc->free(alloc->ctx, clone_prompt, clone_len + 1);
            }
        }
    }

    if (len > 0) {
        agent->conversation_context = buf;
        agent->conversation_context_len = len;
        agent->humanness_ctx_owned = true;
        /* Track the actual allocation separately so the free path can
         * distinguish "still owned by humanness" from "daemon overwrote
         * conversation_context mid-turn". */
        agent->humanness_ctx_buf = buf;
        agent->humanness_ctx_buf_len = len;
    } else {
        alloc->free(alloc->ctx, buf, cap);
    }
    return HU_OK;
}

void hu_agent_free_turn_context(hu_agent_t *agent) {
    if (!agent || !agent->humanness_ctx_owned)
        return;

    /* Defensive: only free the buffer humanness actually allocated. If the
     * daemon overwrote agent->conversation_context with its own buffer
     * between turns, conversation_context will NOT match humanness_ctx_buf.
     * In that case we release the humanness allocation (via humanness_ctx_buf)
     * and leave conversation_context alone — the daemon owns that pointer
     * and will free it itself.
     *
     * Production crash regression (2026-05-10):
     *   tests/test_humanness_context.c::
     *     free_context_handles_daemon_override_after_humanness_owned */
    if (agent->humanness_ctx_buf && agent->alloc) {
        agent->alloc->free(agent->alloc->ctx, (void *)agent->humanness_ctx_buf,
                           agent->humanness_ctx_buf_len + 1);

        /* Only clear conversation_context if it still points at the buffer
         * we just freed. Otherwise the daemon owns that pointer. */
        if (agent->conversation_context == agent->humanness_ctx_buf) {
            agent->conversation_context = NULL;
            agent->conversation_context_len = 0;
        }
    }

    agent->humanness_ctx_buf = NULL;
    agent->humanness_ctx_buf_len = 0;
    agent->humanness_ctx_owned = false;
}

hu_error_t hu_agent_update_voice_profile(hu_agent_t *agent, const char *user_msg, size_t msg_len) {
    if (!agent || !agent->voice_profile_initialized)
        return HU_ERR_INVALID_ARGUMENT;

    bool had_emotion = false;
    bool had_topic = false;
    bool had_humor = false;

    if (user_msg && msg_len > 0) {
        static const char *emotion_words[] = {"feel", "sad",   "happy", "angry", "worried",
                                              "love", "hate",  "miss",  "hurt",  "afraid",
                                              "hope", "sorry", "proud", NULL};
        static const char *humor_words[] = {"lol", "lmao", "haha", "rofl", "funny", "joke", NULL};

        for (const char **w = emotion_words; *w; w++) {
            size_t wl = strlen(*w);
            for (size_t i = 0; i + wl <= msg_len; i++) {
                bool match = true;
                for (size_t j = 0; j < wl; j++) {
                    char c = user_msg[i + j];
                    if (c >= 'A' && c <= 'Z')
                        c = (char)(c + 32);
                    if (c != (*w)[j]) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    had_emotion = true;
                    break;
                }
            }
            if (had_emotion)
                break;
        }

        for (const char **w = humor_words; *w; w++) {
            size_t wl = strlen(*w);
            for (size_t i = 0; i + wl <= msg_len; i++) {
                bool match = true;
                for (size_t j = 0; j < wl; j++) {
                    char c = user_msg[i + j];
                    if (c >= 'A' && c <= 'Z')
                        c = (char)(c + 32);
                    if (c != (*w)[j]) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    had_humor = true;
                    break;
                }
            }
            if (had_humor)
                break;
        }

        had_topic = (msg_len > 50);
    }

    hu_voice_profile_update(&agent->voice_profile, had_emotion, had_topic, had_humor);
    return HU_OK;
}
