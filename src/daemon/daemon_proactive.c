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
#include "human/agent/governor.h"
#include "human/agent/proactive.h"
#include "human/agent/proactive_throttle.h"
#include "human/agent/weather_awareness.h"
#include "human/agent/weather_fetch.h"
#include "human/autoresponder.h"
#include "human/config.h"
#include "human/context/protective.h"
#include "human/context/self_awareness.h"
#include "human/core/string.h"
#include "human/daemon_learning_tick.h" /* hu_daemon_proactive_outcome_record_send */
#include "human/feeds/awareness.h"
#include "human/feeds/processor.h"
#include "human/memory.h"
#include "human/memory/compression.h"
#include "human/memory/degradation.h"
#include "human/memory/personal_model.h"
#include "human/persona.h"
#include "human/platform.h"
#ifdef HU_ENABLE_SQLITE
#include "human/memory/superhuman.h"
#endif
#if defined(__APPLE__)
#include "human/platform/calendar.h"
#endif

#include "human/core/debug.h"
#include "human/core/log.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* P2-5 (2026-05-16 incident): public outbound-safety predicate for memory
 * entries that hu_daemon_build_callback_context is about to inject into a
 * proactive prompt. The previous code memcpy'd raw entries[i].content
 * bytes — which let "I confessed something terrible" reach family
 * contacts via F25/F30 paths.
 *
 * A memory entry is UNSAFE if it contains:
 *   - first-person pronouns/contractions (i, i'm, i'll, my, me, mine)
 *   - confession verbs (confessed/admitted/lied/cheated/betrayed/secret)
 *   - bare emotion keywords (lonely/depressed/suicidal/anxious/...)
 *   - format-specifier or newline injection
 *
 * Made non-static so tests can pin the predicate directly. Local to this
 * TU rather than depending on the Phase 1 hu_proactive_topic_is_safe so
 * this commit doesn't depend on Phase 1's merge order. */
bool hu_daemon_callback_content_is_safe(const char *content, size_t content_len) {
    if (!content || content_len == 0)
        return false;
    char buf[256];
    size_t copy = content_len < sizeof(buf) - 1 ? content_len : sizeof(buf) - 1;
    for (size_t i = 0; i < copy; i++)
        buf[i] = (char)tolower((unsigned char)content[i]);
    buf[copy] = '\0';

    static const char *first_person_tokens[] = {
        " i ", " i'm", " i'll", "i'm ", "i'll ", " my ", " me ", " mine ", "myself", NULL,
    };
    for (const char **p = first_person_tokens; *p; p++)
        if (strstr(buf, *p))
            return false;
    if (copy >= 2 && buf[0] == 'i' && (buf[1] == ' ' || buf[1] == '\''))
        return false;

    static const char *confession_verbs[] = {
        "confessed", "admitted", "lied to", "cheated on", "betrayed", "secret", "regret", NULL,
    };
    for (const char **p = confession_verbs; *p; p++)
        if (strstr(buf, *p))
            return false;

    static const char *charged_keywords[] = {
        "lonely",     "depressed", "suicidal",  "scared",    "terrible",
        "dying",      "crying",    "anxious",   "exhausted", "burnt out",
        "burned out", "broken",    "miserable", "hopeless",  NULL,
    };
    for (const char **p = charged_keywords; *p; p++)
        if (strstr(buf, *p))
            return false;

    if (strchr(buf, '%') || strchr(buf, '\n') || strchr(buf, '\r'))
        return false;

    return true;
}

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
        /* P2-5 (2026-05-16): skip entries whose raw content is unsafe to
         * inject into an outbound prompt. The previous code memcpy'd the
         * raw bytes regardless of content, which let "I confessed
         * something terrible" reach a family contact via this path. */
        if (!hu_daemon_callback_content_is_safe(entries[i].content, entries[i].content_len))
            continue;
        /* P2-11 (2026-05-16): memory degradation is a UX-of-recall concept
         * (mimics human forgetting in interactive recall). The previous
         * code applied it to content about to be injected into an OUTBOUND
         * proactive prompt — corrupting the text the LLM would then send
         * verbatim. Disable degradation on this path. The seed/rate locals
         * remain for the future-when-recall-display-is-separate flow. */
        (void)deg_rate;
        const char *content = entries[i].content;
        size_t content_len = entries[i].content_len;
        size_t show = content_len;
        if (show > 200)
            show = 200;
        w = snprintf(buf + pos, sizeof(buf) - pos, "%.*s\n", (int)show, content);
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

#ifdef HU_ENABLE_SQLITE
/* Sprint 59 Phase C (2026-05-26 Annie/Mindy/Betty incident) — per-contact
 * feed-item scope. Replaces the previous use of
 * hu_feed_processor_get_all_recent in the FEED AWARENESS block below; that
 * call returned items from every contact, which combined with the
 * proactive prompt being keyed by the recipient's contact_id allowed one
 * contact's feed topic to bleed into another contact's emotional_moments
 * row (see header for the full incident chain). */
hu_error_t hu_daemon_proactive_get_contact_feed_items(hu_allocator_t *alloc, sqlite3 *db,
                                                      const hu_contact_profile_t *cp, size_t limit,
                                                      hu_feed_item_stored_t **out,
                                                      size_t *out_count) {
    if (!alloc || !db || !cp || !cp->contact_id || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    return hu_feed_processor_get_for_contact(alloc, db, cp->contact_id, strlen(cp->contact_id),
                                             limit, out, out_count);
}
#endif

/* ── Proactive prompt builder ──────────────────────────────────────── */

char *hu_daemon_proactive_prompt_for_contact(hu_allocator_t *alloc, hu_agent_t *agent,
                                             hu_legacy_memory_t *memory,
                                             const hu_contact_profile_t *cp, size_t *out_len) {
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
            hu_feed_item_stored_t *stored = NULL;
            size_t scount = 0;
            /* Sprint 59 Phase C: scope to this contact's feed items only.
             * Previous get_all_recent variant returned items from every
             * contact, which let one contact's "lonely" feed topic seed
             * emotional_moments rows for unrelated contacts via the
             * proactive recipient's session_id (Annie/Mindy/Betty). */
            if (hu_daemon_proactive_get_contact_feed_items(alloc, fdb, cp, 32, &stored, &scount) ==
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

    /* 2026-05-16 P1-8: self-awareness directive (topic-repeat suppression). */
    char *self_aware_ctx = NULL;
    size_t self_aware_ctx_len = 0;
#ifdef HU_ENABLE_SQLITE
    if (memory && cp->contact_id) {
        (void)hu_self_awareness_build_directive_from_memory(
            alloc, memory, cp->contact_id, strlen(cp->contact_id), (int64_t)time(NULL),
            &self_aware_ctx, &self_aware_ctx_len);
    }
#endif

    /* P6-1: per-channel overlay directives — mirror the reactive path in
     * src/agent/agent_stream.c so proactive prompts carry the same
     * formality/length/emoji/pragmatic guidance the LLM gets when it
     * responds to an inbound message.  Channel name derived from
     * cp->proactive_channel ("imessage:+1234567890" → "imessage"). */
    char *overlay_ctx = NULL;
    size_t overlay_ctx_len = 0;
    if (agent && agent->persona && cp->proactive_channel) {
        const char *ch_name = cp->proactive_channel;
        size_t ch_name_len = strlen(ch_name);
        const char *colon = strchr(ch_name, ':');
        if (colon)
            ch_name_len = (size_t)(colon - ch_name);
        if (ch_name_len > 0) {
            const hu_persona_overlay_t *ov =
                hu_persona_find_overlay(agent->persona, ch_name, ch_name_len);
            if (ov) {
                char obuf[1024];
                size_t opos = 0;
                int n = snprintf(obuf + opos, sizeof(obuf) - opos, "\nChannel style:");
                if (n > 0 && opos + (size_t)n < sizeof(obuf))
                    opos += (size_t)n;
                if (ov->formality) {
                    n = snprintf(obuf + opos, sizeof(obuf) - opos, " %s.", ov->formality);
                    if (n > 0 && opos + (size_t)n < sizeof(obuf))
                        opos += (size_t)n;
                }
                if (ov->avg_length) {
                    n = snprintf(obuf + opos, sizeof(obuf) - opos, " Length: %s.", ov->avg_length);
                    if (n > 0 && opos + (size_t)n < sizeof(obuf))
                        opos += (size_t)n;
                }
                if (ov->emoji_usage) {
                    n = snprintf(obuf + opos, sizeof(obuf) - opos, " Emoji: %s.", ov->emoji_usage);
                    if (n > 0 && opos + (size_t)n < sizeof(obuf))
                        opos += (size_t)n;
                }
                if (ov->directness) {
                    n = snprintf(obuf + opos, sizeof(obuf) - opos, " Directness: %s.",
                                 ov->directness);
                    if (n > 0 && opos + (size_t)n < sizeof(obuf))
                        opos += (size_t)n;
                }
                if (ov->face_saving) {
                    n = snprintf(obuf + opos, sizeof(obuf) - opos, " Face-saving: %s.",
                                 ov->face_saving);
                    if (n > 0 && opos + (size_t)n < sizeof(obuf))
                        opos += (size_t)n;
                }
                if (ov->disagreement_style) {
                    n = snprintf(obuf + opos, sizeof(obuf) - opos, " Disagreement: %s.",
                                 ov->disagreement_style);
                    if (n > 0 && opos + (size_t)n < sizeof(obuf))
                        opos += (size_t)n;
                }
                for (size_t i = 0; i < ov->style_notes_count; i++) {
                    if (ov->style_notes[i]) {
                        n = snprintf(obuf + opos, sizeof(obuf) - opos, " %s.", ov->style_notes[i]);
                        if (n > 0 && opos + (size_t)n < sizeof(obuf))
                            opos += (size_t)n;
                    }
                }
                if (opos > 0) {
                    overlay_ctx = (char *)alloc->alloc(alloc->ctx, opos + 1);
                    if (overlay_ctx) {
                        memcpy(overlay_ctx, obuf, opos);
                        overlay_ctx[opos] = '\0';
                        overlay_ctx_len = opos;
                    }
                }
            }
        }
    }

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

    /* P6-2: relationship_type + dunbar_layer give the LLM the register
     * it should write in — texting a sister (dunbar 1) is not the same
     * as texting a coworker (dunbar 3). Format only the fields present;
     * fall through to the prior generic shape if neither is set. */
    char base_buf[512];
    int w;
    if (cp->relationship_type && cp->relationship_type[0] && cp->dunbar_layer &&
        cp->dunbar_layer[0]) {
        w = snprintf(base_buf, sizeof(base_buf),
                     "You're initiating a casual check-in text to %s. "
                     "You are texting your %s (dunbar layer %s). ",
                     cp->name ? cp->name : "this person", cp->relationship_type, cp->dunbar_layer);
    } else if (cp->relationship_type && cp->relationship_type[0]) {
        w = snprintf(base_buf, sizeof(base_buf),
                     "You're initiating a casual check-in text to %s. "
                     "You are texting your %s. ",
                     cp->name ? cp->name : "this person", cp->relationship_type);
    } else if (cp->dunbar_layer && cp->dunbar_layer[0]) {
        w = snprintf(base_buf, sizeof(base_buf),
                     "You're initiating a casual check-in text to %s "
                     "(dunbar layer %s). ",
                     cp->name ? cp->name : "this person", cp->dunbar_layer);
    } else {
        w = snprintf(base_buf, sizeof(base_buf), "You're initiating a casual check-in text to %s. ",
                     cp->name ? cp->name : "this person");
    }
    size_t base_len = (w > 0 && (size_t)w < sizeof(base_buf)) ? (size_t)w : 0;

    /* P6-5: shared absolute-rules block — same source of truth as the
     * reactive path (src/agent/agent_stream.c). Last-position weight. */
    char absolute_rules_buf[2048];
    size_t absolute_rules_len = 0;
    if (hu_persona_build_absolute_rules(agent ? agent->persona : NULL, absolute_rules_buf,
                                        sizeof(absolute_rules_buf), &absolute_rules_len) != HU_OK)
        absolute_rules_len = 0;

    size_t total = base_len + rules_len;
    if (self_aware_ctx && self_aware_ctx_len > 0)
        total += self_aware_ctx_len + 2; /* prepended with trailing "\n\n" */
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
    if (overlay_ctx && overlay_ctx_len > 0)
        total += 2 + overlay_ctx_len;
    if (absolute_rules_len > 0)
        total += absolute_rules_len;

    char *result = (char *)alloc->alloc(alloc->ctx, total + 1);
    if (!result) {
        if (self_aware_ctx)
            alloc->free(alloc->ctx, self_aware_ctx, self_aware_ctx_len + 1);
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
        if (overlay_ctx)
            alloc->free(alloc->ctx, overlay_ctx, overlay_ctx_len + 1);
        *out_len = 0;
        return NULL;
    }

    size_t pos = 0;
    /* 2026-05-16 P1-8: prepend self-awareness directive. */
    if (self_aware_ctx && self_aware_ctx_len > 0) {
        memcpy(result + pos, self_aware_ctx, self_aware_ctx_len);
        pos += self_aware_ctx_len;
        result[pos++] = '\n';
        result[pos++] = '\n';
        alloc->free(alloc->ctx, self_aware_ctx, self_aware_ctx_len + 1);
    }
    memcpy(result + pos, base_buf, base_len);
    pos += base_len;

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
    if (overlay_ctx && overlay_ctx_len > 0) {
        result[pos++] = '\n';
        result[pos++] = '\n';
        memcpy(result + pos, overlay_ctx, overlay_ctx_len);
        pos += overlay_ctx_len;
        alloc->free(alloc->ctx, overlay_ctx, overlay_ctx_len + 1);
    }

    memcpy(result + pos, rules, rules_len);
    pos += rules_len;
    if (absolute_rules_len > 0) {
        memcpy(result + pos, absolute_rules_buf, absolute_rules_len);
        pos += absolute_rules_len;
    }
    result[pos] = '\0';
    *out_len = pos;
    return result;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Follow-up watcher flush (US-48-3) — generate and send a follow-up draft
 * ────────────────────────────────────────────────────────────────────────── */

hu_error_t hu_daemon_follow_up_flush_for_contact(hu_allocator_t *alloc, struct hu_agent *agent,
                                                 const char *contact_handle, struct hu_config *cfg,
                                                 hu_service_channel_t *channels,
                                                 size_t channel_count,
                                                 struct hu_proactive_throttle *throttle) {
    if (!alloc || !agent || !contact_handle || !contact_handle[0] || !cfg)
        return HU_ERR_INVALID_ARGUMENT;
    if (!channels || channel_count == 0 || !throttle)
        return HU_ERR_INVALID_ARGUMENT;

    hu_log_info("follow_up_watcher", NULL, "follow-up flush initiated for contact %s",
                contact_handle);

    /* Step 1: Load per-contact personal model (US-48-2).
     * The contact_handle is typically an iMessage handle like "+15551234567" or
     * "alice@example.com". Construct the model db path using workspace_dir. */
    hu_personal_model_t contact_model;
    memset(&contact_model, 0, sizeof(contact_model));

    char db_path[512] = {0};
    if (cfg->workspace_dir) {
        snprintf(db_path, sizeof(db_path), "%s/models/per_contact", cfg->workspace_dir);
    } else {
        /* Fallback: use ~/.human */
        const char *home = getenv("HOME");
        snprintf(db_path, sizeof(db_path), "%s/.human/models/per_contact", home ? home : "/tmp");
    }

    hu_error_t pm_err = hu_personal_model_load_for_contact(&contact_model, contact_handle, db_path);
    if (pm_err != HU_OK) {
        hu_log_warn(
            "follow_up_watcher", NULL,
            "failed to load personal model for contact %s (err=%d); proceeding with empty model",
            contact_handle, pm_err);
        /* Non-fatal: proceed with empty model. The autoresponder will generate a generic response.
         */
    }

    /* Step 2: Build autoresponder prompt for the follow-up.
     * Use a simple incoming message to trigger the follow-up template. */
    const char *follow_up_trigger = "It's been a while since we last talked.";
    char prompt_buf[4096];
    size_t prompt_len = hu_autoresponder_build_prompt(
        NULL, /* autoresponder_config — NULL means skip the DND/allowlist checks */
        contact_handle, "imessage", follow_up_trigger, NULL, /* persona_summary=NULL for now */
        &contact_model, (int64_t)time(NULL), prompt_buf, sizeof(prompt_buf));

    if (prompt_len == 0 || prompt_len >= sizeof(prompt_buf)) {
        hu_log_warn("follow_up_watcher", NULL,
                    "failed to build autoresponder prompt for contact %s", contact_handle);
        return HU_ERR_INVALID_ARGUMENT;
    }

    hu_log_info("follow_up_watcher", NULL, "built follow-up prompt (%zu bytes) for contact %s",
                prompt_len, contact_handle);

    /* Step 3: Send via iMessage.
     * Find the iMessage channel in the service channels array and call its send vtable. */
    hu_error_t send_err = HU_ERR_NOT_FOUND;
    for (size_t i = 0; i < channel_count; i++) {
        hu_service_channel_t *ch = &channels[i];
        if (!ch->channel || !ch->channel->vtable || !ch->channel->vtable->name)
            continue;
        const char *ch_name = ch->channel->vtable->name(ch->channel->ctx);
        if (ch_name && strcmp(ch_name, "imessage") == 0) {
            /* Found iMessage channel. Send the follow-up prompt. */
            send_err =
                ch->channel->vtable->send(ch->channel->ctx, contact_handle, strlen(contact_handle),
                                          prompt_buf, prompt_len, NULL, /* media */
                                          0);                           /* media_count */
            if (send_err == HU_OK) {
                hu_log_info("follow_up_watcher", NULL, "follow-up sent via iMessage to contact %s",
                            contact_handle);
            } else {
                hu_log_warn("follow_up_watcher", NULL,
                            "iMessage send failed for contact %s (err=%d)", contact_handle,
                            send_err);
            }
            break;
        }
    }
    if (send_err == HU_ERR_NOT_FOUND) {
        hu_log_warn("follow_up_watcher", NULL,
                    "iMessage channel not found in service channels; cannot send follow-up");
    }

    /* Step 4: Record throttle event (for rate-limiting).
     * Track the send so we don't violate the follow-up throttle limits. */
    int64_t now_unix = time(NULL);
    uint64_t now_ms = (uint64_t)now_unix * 1000;
    bool allowed = hu_proactive_throttle_record_send(throttle, contact_handle, "follow_up", now_ms);
    if (!allowed) {
        hu_log_warn("follow_up_watcher", NULL, "throttle blocked follow-up send for contact %s",
                    contact_handle);
    } else {
        hu_log_info("follow_up_watcher", NULL, "throttle recorded follow-up send for contact %s",
                    contact_handle);
    }

    return HU_OK;
}

/* Test helpers removed — use hu_proactive_context_reset() and ctx->count directly. */

/* Sprint 41 (2026-05-26 Jordan incident) — quiet-hour gate predicate for
 * the proactive-send block in daemon.c. Extracted as a pure predicate
 * (see ~/.claude/rules/security-predicate-extraction.md) so the truth
 * table can be locked by unit tests without spinning a daemon, and so
 * the daemon-side wire-up reduces to a single condition + log.
 *
 * Contract:
 *   - Returns true  → daemon MUST skip the proactive send this tick
 *                     because the recipient is inside an autoresponder
 *                     DND window.
 *   - Returns false → no quiet-hour gate (either cfg unset OR window
 *                     not active OR cfg disabled).
 *
 * NULL cfg means "operator has not configured quiet hours" — that is a
 * config decision, not a daemon bug. Returning false here preserves the
 * pre-change behavior for operators who deliberately disabled the
 * autoresponder. */
bool hu_daemon_proactive_should_skip_for_quiet_hours(const hu_autoresponder_config_t *ar_cfg,
                                                     int64_t now_unix, int32_t tz_offset_seconds) {
    /* NULL or operator-disabled cfg: never gate. Mirrors what
     * daemon_autoresponder_config() already returns NULL for in
     * production — we re-check here so unit tests and any future
     * caller cannot pass a disabled cfg by accident and have the
     * predicate quietly suppress sends. */
    if (!ar_cfg || !ar_cfg->enabled)
        return false;
    return hu_autoresponder_in_dnd_window(ar_cfg, now_unix, tz_offset_seconds);
}

bool hu_daemon_proactive_should_skip_for_budget(hu_proactive_budget_t *budget, uint64_t now_ms) {
    /* NULL budget: operator opted out of budget enforcement entirely.
     * Mirrors init_proposer's NULL-budget semantics at init_proposer.c:91. */
    if (!budget)
        return false;
    return !hu_governor_has_budget(budget, now_ms);
}

/* Send a proactive check-in and record it, ONLY if the channel accepted it.
 *
 * Extracted from daemon.c (which is at its size ratchet) together with the fix
 * for a discarded return value. Previously the send's hu_error_t was thrown
 * away, so a FAILED send still logged "proactive check-in sent", recorded
 * send-recency (which suppresses later REACTIVE replies to that contact), fed
 * hu_daemon_proactive_outcome_record_send (training the humanization bandit on a
 * message nobody received), and charged the governor budget (throttling real
 * sends). One unchecked return corrupted four downstream systems.
 *
 * Measured 2026-08-02: 12 "proactive check-in sent" lines in the daemon log and
 * ZERO matching rows in chat.db.
 *
 * Returns true only when the message was actually accepted for delivery. */
bool hu_daemon_proactive_send_and_record(struct hu_agent *agent, hu_channel_t *channel,
                                         const struct hu_contact_profile *cp, const char *ch_name,
                                         const char *target, size_t target_len, const char *message,
                                         size_t message_len, int64_t now,
                                         hu_proactive_budget_t *gov_budget) {
    if (!channel || !channel->vtable || !channel->vtable->send || !cp)
        return false;

    const char *who = cp->name ? cp->name : cp->contact_id;
    hu_error_t send_rc =
        channel->vtable->send(channel->ctx, target, target_len, message, message_len, NULL, 0);
    if (send_rc != HU_OK) {
        hu_log_warn("human", agent ? agent->observer : NULL,
                    "proactive check-in to %s FAILED (err=%d), nothing delivered; "
                    "skipping recency/outcome/governor bookkeeping",
                    who, (int)send_rc);
        return false;
    }

    /* FU-1: record proactive send so reactive deferral works. */
    if (agent)
        hu_contact_send_recency_record(&agent->contact_send_recency, cp->contact_id,
                                       strlen(cp->contact_id), now, HU_SEND_PATH_PROACTIVE);
    (void)hu_daemon_proactive_outcome_record_send(agent ? agent->memory : NULL, ch_name, target,
                                                  target_len);
    hu_log_info("human", agent ? agent->observer : NULL, "proactive check-in sent to %s: %.*s", who,
                (int)message_len, message ? message : "");
    if (gov_budget)
        hu_governor_record_sent(gov_budget, (uint64_t)time(NULL) * 1000ULL);
    return true;
}
