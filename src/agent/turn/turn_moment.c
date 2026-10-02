/* Per-turn moment cue for hu_agent_turn (S-stage helper) — see include/human/agent/turn_moment.h.
 */
#include "human/agent/turn_moment.h"
#include "human/agent.h"
#include "human/moment.h"
#include "human/persona.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Inbound messages this close to the newest one are the burst being answered. */
#define HU_TURN_MOMENT_BURST_S ((int64_t)600)

/* "YYYY-MM-DD HH:MM[:SS]" local time (load_conversation_history's format)
 * → epoch seconds, -1 when it does not parse. */
static int64_t history_ts(const char *ts) {
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    int sec = 0;
    if (!ts || sscanf(ts, "%d-%d-%d %d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour,
                      &tm.tm_min, &sec) < 5)
        return -1;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    tm.tm_sec = sec;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    return t == (time_t)-1 ? -1 : (int64_t)t;
}

size_t hu_turn_moment_render_entries(const struct hu_persona_t *persona,
                                     const struct hu_persona_overlay_t *overlay,
                                     const hu_channel_history_entry_t *entries, size_t count,
                                     int64_t now_s, char *buf, size_t cap) {
    if (!buf || cap == 0)
        return 0;
    buf[0] = '\0';
    if (!entries || count == 0)
        return 0; /* no thread: no evidence of a gap, so no greeting cue */

    int64_t newest = history_ts(entries[count - 1].timestamp);
    if (newest < 0)
        return 0;
    /* Everything before the burst being answered is the prior thread. */
    size_t prior = count;
    while (prior > 0 && !entries[prior - 1].from_me) {
        int64_t ts = history_ts(entries[prior - 1].timestamp);
        if (ts < 0 || newest - ts > HU_TURN_MOMENT_BURST_S)
            break;
        prior--;
    }

    int64_t *ts_s = NULL;
    bool *outbound = NULL;
    const char **text = NULL;
    int64_t last_their = -1, last_our = -1;
    if (prior > 0) {
        ts_s = (int64_t *)calloc(prior, sizeof(*ts_s));
        outbound = (bool *)calloc(prior, sizeof(*outbound));
        text = (const char **)calloc(prior, sizeof(*text));
        if (!ts_s || !outbound || !text) {
            free(ts_s);
            free(outbound);
            free(text);
            return 0;
        }
        for (size_t i = 0; i < prior; i++) {
            ts_s[i] = history_ts(entries[i].timestamp);
            outbound[i] = entries[i].from_me;
            text[i] = entries[i].text;
            int64_t *last = entries[i].from_me ? &last_our : &last_their;
            if (ts_s[i] > *last)
                *last = ts_s[i];
        }
    }
    struct hu_conversation_history_t *history =
        prior > 0 ? hu_moment_history_create(prior, ts_s, outbound, text) : NULL;
    free(ts_s);
    free(outbound);
    free(text);
    if (prior > 0 && !history)
        return 0;

    size_t n = 0;
    hu_moment_t moment;
    if (hu_moment_compose_for_reply(persona, overlay, history, last_their, last_our, NULL,
                                    entries[count - 1].from_me ? NULL : entries[count - 1].text,
                                    now_s, &moment) != HU_OK ||
        hu_moment_render_prompt(&moment, buf, cap, &n) != HU_OK) {
        buf[0] = '\0';
        n = 0;
    }
    hu_moment_history_free(history);
    return n;
}

size_t hu_turn_moment_render(const struct hu_agent *agent, int64_t now_s, char *buf, size_t cap) {
    if (buf && cap)
        buf[0] = '\0';
    if (!agent || !agent->persona)
        return 0;
    const struct hu_persona_overlay_t *overlay = NULL;
    if (agent->active_channel && agent->active_channel_len > 0)
        overlay = (const struct hu_persona_overlay_t *)hu_persona_find_overlay(
            agent->persona, agent->active_channel, agent->active_channel_len);
    return hu_turn_moment_render_entries((const struct hu_persona_t *)agent->persona, overlay,
                                         agent->ab_history_entries, agent->ab_history_count, now_s,
                                         buf, cap);
}
