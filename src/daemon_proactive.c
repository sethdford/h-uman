/**
 * daemon_proactive.c — Proactive check-in subsystem extracted from daemon.c.
 *
 * Implements:
 *   - Contact activity LRU cache (per-contact last inbound channel tracking)
 *   - Proactive route parsing and activity-based override
 *   - Memory callback context builder (recall + degradation + protective filter)
 *   - Proactive prompt construction (starter, memory, weather, feeds, calendar, rules)
 */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif

#include "human/daemon_proactive.h"
#include "human/agent.h"
#include "human/agent/proactive.h"
#include "human/agent/weather_awareness.h"
#include "human/agent/weather_fetch.h"
#include "human/config.h"
#include "human/context/protective.h"
#include "human/core/string.h"
#include "human/feeds/awareness.h"
#include "human/feeds/processor.h"
#include "human/memory.h"
#include "human/memory/compression.h"
#include "human/memory/degradation.h"
#include "human/persona.h"
#include "human/platform.h"
#ifdef HU_ENABLE_SQLITE
#include "human/memory/superhuman.h"
#endif
#if defined(__APPLE__)
#include "human/platform/calendar.h"
#endif

#include "human/core/debug.h"
#include <stdio.h>
#include <string.h>

/* ── Contact activity LRU cache ─────────────────────────────────────── */

void hu_proactive_context_reset(hu_proactive_context_t *ctx) {
    if (!ctx)
        return;
    HU_ASSERT_NOT_REENTRANT(proactive_reset);
    memset(ctx->entries, 0, sizeof(ctx->entries));
    ctx->count = 0;
    ctx->seq = 0;
    HU_LEAVE_NOT_REENTRANT(proactive_reset);
}

bool hu_daemon_channel_list_has_name(const hu_service_channel_t *channels, size_t channel_count,
                                     const char *name) {
    if (!name || !name[0])
        return false;
    for (size_t i = 0; i < channel_count; i++) {
        if (!channels[i].channel || !channels[i].channel->vtable ||
            !channels[i].channel->vtable->name)
            continue;
        const char *n = channels[i].channel->vtable->name(channels[i].channel->ctx);
        if (n && strcmp(n, name) == 0)
            return true;
    }
    return false;
}

void hu_daemon_contact_activity_record(hu_proactive_context_t *ctx, const char *contact_id,
                                       const char *channel_name, const char *session_key) {
    if (!ctx || !contact_id || !contact_id[0] || !channel_name || !channel_name[0] ||
        !session_key || !session_key[0])
        return;
    HU_ASSERT_NOT_REENTRANT(proactive_record);

    size_t slot = (size_t)-1;
    for (size_t i = 0; i < ctx->count; i++) {
        if (strcmp(ctx->entries[i].contact_id, contact_id) == 0) {
            slot = i;
            break;
        }
    }

    if (slot == (size_t)-1) {
        if (ctx->count < HU_DAEMON_CONTACT_ACTIVITY_CAP) {
            slot = ctx->count++;
        } else {
            size_t lru = 0;
            for (size_t j = 1; j < HU_DAEMON_CONTACT_ACTIVITY_CAP; j++) {
                if (ctx->entries[j].lru_seq < ctx->entries[lru].lru_seq)
                    lru = j;
            }
            slot = lru;
        }
    }

    size_t cid_len = strlen(contact_id);
    if (cid_len >= sizeof(ctx->entries[slot].contact_id))
        cid_len = sizeof(ctx->entries[slot].contact_id) - 1;
    memcpy(ctx->entries[slot].contact_id, contact_id, cid_len);
    ctx->entries[slot].contact_id[cid_len] = '\0';

    size_t ch_len = strlen(channel_name);
    if (ch_len >= sizeof(ctx->entries[slot].last_channel))
        ch_len = sizeof(ctx->entries[slot].last_channel) - 1;
    memcpy(ctx->entries[slot].last_channel, channel_name, ch_len);
    ctx->entries[slot].last_channel[ch_len] = '\0';

    size_t sk_len = strlen(session_key);
    if (sk_len >= sizeof(ctx->entries[slot].last_session_key))
        sk_len = sizeof(ctx->entries[slot].last_session_key) - 1;
    memcpy(ctx->entries[slot].last_session_key, session_key, sk_len);
    ctx->entries[slot].last_session_key[sk_len] = '\0';

    ctx->entries[slot].last_activity = time(NULL);
    ctx->entries[slot].lru_seq = ++ctx->seq;
    HU_LEAVE_NOT_REENTRANT(proactive_record);
}

/* ── Proactive route parsing ────────────────────────────────────────── */

void hu_daemon_proactive_parse_route(const hu_contact_profile_t *cp, char *ch_buf,
                                     char *target_buf) {
    memset(ch_buf, 0, 64);
    memset(target_buf, 0, 128);
    const char *colon = strchr(cp->proactive_channel, ':');
    if (colon) {
        size_t ch_len = (size_t)(colon - cp->proactive_channel);
        if (ch_len < 64) {
            memcpy(ch_buf, cp->proactive_channel, ch_len);
            ch_buf[ch_len] = '\0';
        }
        size_t tgt_len = strlen(colon + 1);
        if (tgt_len >= 128)
            tgt_len = 127;
        memcpy(target_buf, colon + 1, tgt_len);
        target_buf[tgt_len] = '\0';
    } else {
        size_t plen = strlen(cp->proactive_channel);
        if (plen >= 64)
            plen = 63;
        memcpy(ch_buf, cp->proactive_channel, plen);
        ch_buf[plen] = '\0';
        size_t cid_len = strlen(cp->contact_id);
        if (cid_len >= 128)
            cid_len = 127;
        memcpy(target_buf, cp->contact_id, cid_len);
        target_buf[cid_len] = '\0';
    }
}

void hu_daemon_proactive_apply_route(hu_proactive_context_t *ctx, const char *contact_id,
                                     time_t now, const hu_service_channel_t *channels,
                                     size_t channel_count, char *ch_buf, char *target_buf,
                                     size_t *target_len) {
    if (!ctx)
        return;
    for (size_t i = 0; i < ctx->count; i++) {
        if (strcmp(ctx->entries[i].contact_id, contact_id) != 0)
            continue;
        if (ctx->entries[i].last_session_key[0] == '\0')
            return;
        if (difftime(now, ctx->entries[i].last_activity) > (double)HU_DAEMON_ACTIVITY_FRESH_SECS)
            return;
        if (!hu_daemon_channel_list_has_name(channels, channel_count, ctx->entries[i].last_channel))
            return;

        size_t ch_len = strlen(ctx->entries[i].last_channel);
        if (ch_len >= 64)
            ch_len = 63;
        memcpy(ch_buf, ctx->entries[i].last_channel, ch_len);
        ch_buf[ch_len] = '\0';

        size_t sk_len = strlen(ctx->entries[i].last_session_key);
        if (sk_len >= 128)
            sk_len = 127;
        memcpy(target_buf, ctx->entries[i].last_session_key, sk_len);
        target_buf[sk_len] = '\0';
        *target_len = sk_len;
        return;
    }
}

/* ── Memory callback context builder ───────────────────────────────── */

char *hu_daemon_build_callback_context(hu_allocator_t *alloc, hu_legacy_memory_t *memory,
                                       const char *session_id, size_t session_id_len,
                                       const char *msg, size_t msg_len, size_t *out_len,
                                       hu_agent_t *agent) {
    *out_len = 0;
    if (!memory || !memory->vtable || !memory->vtable->recall || !msg || msg_len == 0)
        return NULL;

    hu_memory_entry_t *entries = NULL;
    size_t count = 0;
    hu_error_t err = memory->vtable->recall(memory->ctx, alloc, msg, msg_len, 3, session_id,
                                            session_id_len, &entries, &count);
    if (err != HU_OK || !entries || count == 0)
        return NULL;

    time_t now = time(NULL);
    struct tm tm_buf;
    struct tm *lt = hu_platform_localtime_r(&now, &tm_buf);
    int hour_local = lt ? lt->tm_hour : 12;
    float deg_rate = 0.10f;
    if (agent && agent->persona && agent->persona->memory_degradation_rate > 0.f)
        deg_rate = agent->persona->memory_degradation_rate;

    char buf[2048];
    size_t pos = 0;
    int w = snprintf(buf, sizeof(buf), "\nCONTEXT FROM YOUR SHARED HISTORY:\n");
    if (w > 0 && (size_t)w < sizeof(buf))
        pos = (size_t)w;

    size_t usable = 0;
    for (size_t i = 0; i < count && i < 3; i++) {
        if (!entries[i].content || entries[i].content_len == 0)
            continue;
        if (agent &&
            !hu_protective_memory_ok(alloc, memory, session_id, session_id_len, entries[i].content,
                                     entries[i].content_len, 0.0f, hour_local))
            continue;
        const char *content = entries[i].content;
        size_t content_len = entries[i].content_len;
        char *degraded = NULL;
        size_t degraded_len = 0;
        uint32_t seed = (uint32_t)now * 1103515245u + 12345u + (uint32_t)i;
        degraded =
            hu_memory_degradation_apply(alloc, content, content_len, seed, deg_rate, &degraded_len);
        if (degraded && degraded_len > 0) {
            content = degraded;
            content_len = degraded_len;
        }
        size_t show = content_len;
        if (show > 200)
            show = 200;
        w = snprintf(buf + pos, sizeof(buf) - pos, "%.*s\n", (int)show, content);
        if (degraded)
            alloc->free(alloc->ctx, degraded, degraded_len + 1);
        if (w > 0 && pos + (size_t)w < sizeof(buf)) {
            pos += (size_t)w;
            usable++;
        }
    }

    for (size_t i = 0; i < count; i++)
        hu_memory_entry_free_fields(alloc, &entries[i]);
    alloc->free(alloc->ctx, entries, count * sizeof(hu_memory_entry_t));

    if (usable == 0)
        return NULL;

    w = snprintf(buf + pos, sizeof(buf) - pos,
                 "Use this knowledge naturally. Don't reference that you \"remember\" "
                 "things — you just KNOW them, the way you know things about people "
                 "you're close to.\n");
    if (w > 0 && pos + (size_t)w < sizeof(buf))
        pos += (size_t)w;

    char *result = (char *)alloc->alloc(alloc->ctx, pos + 1);
    if (!result)
        return NULL;
    memcpy(result, buf, pos);
    result[pos] = '\0';
    *out_len = pos;
    return result;
}

/* ── Proactive prompt builder ──────────────────────────────────────── */

char *hu_daemon_proactive_prompt_for_contact(hu_allocator_t *alloc, hu_agent_t *agent,
                                             hu_legacy_memory_t *memory, const hu_contact_profile_t *cp,
                                             size_t *out_len) {
    char *starter = NULL;
    size_t starter_len = 0;
    if (memory && cp->contact_id) {
        (void)hu_proactive_build_starter(alloc, memory, cp->contact_id, strlen(cp->contact_id),
                                         &starter, &starter_len);
    }

    /* Memory-informed topics: recall recent memories about the contact */
    char *mem_ctx = NULL;
    size_t mem_ctx_len = 0;
    if (memory && cp->contact_id) {
        mem_ctx =
            hu_daemon_build_callback_context(alloc, memory, cp->contact_id, strlen(cp->contact_id),
                                             "recent conversation topics", 24, &mem_ctx_len, agent);
    }

    /* Calendar awareness: inject today's events when calendar_enabled */
    char *calendar_ctx = NULL;
    size_t calendar_ctx_len = 0;
    if (agent && agent->persona && agent->persona->context_awareness.calendar_enabled) {
#if defined(__APPLE__)
        char *events_json = NULL;
        size_t events_len = 0;
        if (hu_calendar_macos_get_events(alloc, 24, &events_json, &events_len) == HU_OK &&
            events_json && events_len > 2) {
            size_t prefix_len = sizeof("Your calendar today: ") - 1;
            size_t suffix_len =
                sizeof(". Use for context (e.g. 'in meetings', 'had dentist appointment').") - 1;
            calendar_ctx_len = prefix_len + events_len + suffix_len;
            calendar_ctx = (char *)alloc->alloc(alloc->ctx, calendar_ctx_len + 1);
            if (calendar_ctx) {
                memcpy(calendar_ctx, "Your calendar today: ", prefix_len);
                memcpy(calendar_ctx + prefix_len, events_json, events_len);
                memcpy(calendar_ctx + prefix_len + events_len,
                       ". Use for context (e.g. 'in meetings', 'had dentist appointment').",
                       suffix_len + 1);
                alloc->free(alloc->ctx, events_json, events_len + 1);
            } else {
                alloc->free(alloc->ctx, events_json, events_len + 1);
                calendar_ctx_len = 0;
            }
        } else if (events_json) {
            alloc->free(alloc->ctx, events_json, events_len + 1);
        }
#endif
    }

    /* F51: Weather awareness — inject notable weather for proactive context */
    char *weather_ctx = NULL;
    size_t weather_ctx_len = 0;
    if (agent && agent->persona && agent->persona->location[0]) {
        hu_weather_context_t wx = {0};
        (void)hu_weather_fetch(alloc, agent->persona->location, strlen(agent->persona->location),
                               NULL, &wx);
        time_t now_ts = time(NULL);
        struct tm tm_buf;
        uint8_t bth_hour = 12;
        if (hu_platform_localtime_r(&now_ts, &tm_buf))
            bth_hour = (uint8_t)tm_buf.tm_hour;
        if (hu_weather_awareness_should_mention(&wx, bth_hour)) {
            char *wx_dir = NULL;
            size_t wx_len = 0;
            if (hu_weather_awareness_build_directive(alloc, &wx, bth_hour, &wx_dir, &wx_len) ==
                    HU_OK &&
                wx_dir && wx_len > 0) {
                weather_ctx = wx_dir;
                weather_ctx_len = wx_len;
            } else if (wx_dir)
                alloc->free(alloc->ctx, wx_dir, wx_len + 1);
        }
    }

#ifdef HU_ENABLE_SQLITE
    /* Recent feeds → natural bring-up hooks for this contact (high relevance only). */
    char *feed_aware_ctx = NULL;
    size_t feed_aware_ctx_len = 0;
    if (memory && agent && agent->persona) {
        sqlite3 *fdb = hu_sqlite_memory_get_db(memory);
        if (fdb) {
            int64_t since_feed = (int64_t)time(NULL) - (int64_t)172800;
            hu_feed_item_stored_t *stored = NULL;
            size_t scount = 0;
            if (hu_feed_processor_get_all_recent(alloc, fdb, since_feed, 32, &stored, &scount) ==
                    HU_OK &&
                stored && scount > 0) {
                hu_feed_item_t *fitems =
                    (hu_feed_item_t *)alloc->alloc(alloc->ctx, scount * sizeof(*fitems));
                if (fitems) {
                    memset(fitems, 0, scount * sizeof(*fitems));
                    for (size_t fi = 0; fi < scount; fi++)
                        hu_feed_awareness_item_from_stored(&stored[fi], &fitems[fi]);
                    hu_awareness_topic_t *topics = NULL;
                    size_t tcount = 0;
                    if (hu_feed_awareness_synthesize(alloc, fitems, scount, agent->persona, &topics,
                                                     &tcount) == HU_OK &&
                        topics && tcount > 0) {
                        size_t need = 96;
                        for (size_t ti = 0; ti < tcount; ti++) {
                            if (topics[ti].relevance < 0.65)
                                continue;
                            if (!hu_feed_awareness_should_share(&topics[ti], cp))
                                continue;
                            need += strlen(topics[ti].text) + strlen(topics[ti].source) + 64;
                        }
                        if (need > 96) {
                            char *abuf = (char *)alloc->alloc(alloc->ctx, need);
                            if (abuf) {
                                int n0 = snprintf(abuf, need,
                                                  "FEED AWARENESS — optional natural "
                                                  "bring-up (high relevance):\n");
                                size_t ap = (n0 > 0 && (size_t)n0 < need) ? (size_t)n0 : 0;
                                for (size_t ti = 0; ti < tcount; ti++) {
                                    if (topics[ti].relevance < 0.65)
                                        continue;
                                    if (!hu_feed_awareness_should_share(&topics[ti], cp))
                                        continue;
                                    int nw = snprintf(abuf + ap, need - ap, "- [%s | %.2f] %s\n",
                                                      topics[ti].source, topics[ti].relevance,
                                                      topics[ti].text);
                                    if (nw > 0 && (size_t)nw < need - ap)
                                        ap += (size_t)nw;
                                }
                                if (strstr(abuf, "- [") != NULL) {
                                    feed_aware_ctx = abuf;
                                    feed_aware_ctx_len = ap;
                                } else
                                    alloc->free(alloc->ctx, abuf, need);
                            }
                        }
                    }
                    hu_feed_awareness_topics_free(alloc, topics, tcount);
                    alloc->free(alloc->ctx, fitems, scount * sizeof(*fitems));
                }
                hu_feed_items_free(alloc, stored, scount);
            }
        }
    }
#endif /* HU_ENABLE_SQLITE feed awareness */

    static const char HU_DEFAULT_PROACTIVE_RULES[] =
        "\nRules: "
        "1. One short natural message (not 'hey how are you' — too generic). "
        "2. Reference something specific you know about them or ask about "
        "something from a previous conversation. "
        "3. Keep it under 10 words. "
        "4. If you have nothing specific, share something you saw/did "
        "that made you think of them. "
        "5. Reply SKIP if you genuinely have nothing natural to say.";
    const char *rules = (agent && agent->persona && agent->persona->proactive_rules)
                            ? agent->persona->proactive_rules
                            : HU_DEFAULT_PROACTIVE_RULES;
    size_t rules_len = (agent && agent->persona && agent->persona->proactive_rules)
                           ? strlen(rules)
                           : sizeof(HU_DEFAULT_PROACTIVE_RULES) - 1;

    char base_buf[256];
    int w = snprintf(base_buf, sizeof(base_buf), "You're initiating a casual check-in text to %s. ",
                     cp->name ? cp->name : "this person");
    size_t base_len = (w > 0 && (size_t)w < sizeof(base_buf)) ? (size_t)w : 0;

    size_t total = base_len + rules_len;
    if (starter && starter_len > 0)
        total += 2 + starter_len;
    if (mem_ctx && mem_ctx_len > 0)
        total += 2 + mem_ctx_len;
    if (weather_ctx && weather_ctx_len > 0)
        total += 2 + weather_ctx_len;
#ifdef HU_ENABLE_SQLITE
    if (feed_aware_ctx && feed_aware_ctx_len > 0)
        total += 2 + feed_aware_ctx_len;
#endif
    if (calendar_ctx && calendar_ctx_len > 0)
        total += 2 + calendar_ctx_len;

    char *result = (char *)alloc->alloc(alloc->ctx, total + 1);
    if (!result) {
        if (starter)
            alloc->free(alloc->ctx, starter, starter_len + 1);
        if (mem_ctx)
            alloc->free(alloc->ctx, mem_ctx, mem_ctx_len + 1);
        if (weather_ctx)
            alloc->free(alloc->ctx, weather_ctx, weather_ctx_len + 1);
#ifdef HU_ENABLE_SQLITE
        if (feed_aware_ctx)
            alloc->free(alloc->ctx, feed_aware_ctx, feed_aware_ctx_len + 1);
#endif
        if (calendar_ctx)
            alloc->free(alloc->ctx, calendar_ctx, calendar_ctx_len + 1);
        *out_len = 0;
        return NULL;
    }

    size_t pos = 0;
    memcpy(result, base_buf, base_len);
    pos = base_len;

    if (starter && starter_len > 0) {
        result[pos++] = '\n';
        result[pos++] = '\n';
        memcpy(result + pos, starter, starter_len);
        pos += starter_len;
        alloc->free(alloc->ctx, starter, starter_len + 1);
    }
    if (mem_ctx && mem_ctx_len > 0) {
        result[pos++] = '\n';
        result[pos++] = '\n';
        memcpy(result + pos, mem_ctx, mem_ctx_len);
        pos += mem_ctx_len;
        alloc->free(alloc->ctx, mem_ctx, mem_ctx_len + 1);
    }
    if (weather_ctx && weather_ctx_len > 0) {
        result[pos++] = '\n';
        result[pos++] = '\n';
        memcpy(result + pos, weather_ctx, weather_ctx_len);
        pos += weather_ctx_len;
        alloc->free(alloc->ctx, weather_ctx, weather_ctx_len + 1);
    }
#ifdef HU_ENABLE_SQLITE
    if (feed_aware_ctx && feed_aware_ctx_len > 0) {
        result[pos++] = '\n';
        result[pos++] = '\n';
        memcpy(result + pos, feed_aware_ctx, feed_aware_ctx_len);
        pos += feed_aware_ctx_len;
        alloc->free(alloc->ctx, feed_aware_ctx, feed_aware_ctx_len + 1);
    }
#endif
    if (calendar_ctx && calendar_ctx_len > 0) {
        result[pos++] = '\n';
        result[pos++] = '\n';
        memcpy(result + pos, calendar_ctx, calendar_ctx_len);
        pos += calendar_ctx_len;
        alloc->free(alloc->ctx, calendar_ctx, calendar_ctx_len + 1);
    }

    memcpy(result + pos, rules, rules_len);
    pos += rules_len;
    result[pos] = '\0';
    *out_len = pos;
    return result;
}

/* Test helpers removed — use hu_proactive_context_reset() and ctx->count directly. */
