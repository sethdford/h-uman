/**
 * daemon_reactive_prompt.c — prompt-building phases of the reactive reply path
 * (slice B of the hu_service_run batch-reply carve-out).
 *
 * Pure move of src/daemon.c lines 5644-6974 as they stood after slice A
 * (27f5de110), see docs/plans/2026-09-02-daemon-batch-reply-carveout.md: the
 * Phase 6 prefix builder (life sim, mood, ToM, anticipatory, self-awareness,
 * life chapter, social graph, timezone, humor, inner thoughts, anti-sycophancy,
 * repair, evolved opinions, feeds, visual, relationship dynamics), then
 * hu_conversation_build_awareness, the prefix merge that produces convo_ctx,
 * and the F21 topic-switch consolidation. The body is the original text with
 * inputs aliased under their historical names; the only textual edits are the
 * five sites where two hu_service_run statics (inner_thought_store,
 * repair_signal) are now reached through pointers instead of by name; the
 * drift_check_counter static moved here with its only user.
 * Every #if gate is kept; the whole region sat inside #ifndef HU_IS_TEST.
 */
#include "human/daemon/reactive_turn.h"

#include "human/agent.h"
#include "human/config.h"
#include "human/core/gate_mode.h"
#include "human/core/log.h"
#include "human/core/string.h"
#include "human/daemon/agent_facade.h"
#include "human/daemon/context_facade.h"
#include "human/daemon/director.h"
#include "human/daemon/feeds_facade.h"
#include "human/daemon/intelligence_facade.h"
#include "human/daemon/memory_facade.h"
#include "human/daemon/persona_facade.h"
#include "human/daemon_maintenance.h"
#include "human/humanness.h"
#include "human/memory/opinion_challenge.h"
#include "human/platform.h"
#include "human/visual/content.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

void hu_daemon_reactive_prompt_build(hu_allocator_t *alloc, hu_agent_t *agent,
                                     const hu_config_t *config, hu_reactive_turn_ctx_t *rt) {
#ifndef HU_IS_TEST
    /* Historical names for the moved body (see file comment). */
    const char *batch_key = rt->batch_key;
    size_t key_len = rt->key_len;
    const char *combined = rt->combined;
    size_t combined_len = rt->combined_len;
    bool llm_decides = rt->llm_decides;
    char *convo_ctx = rt->convo_ctx;
    size_t convo_ctx_len = rt->convo_ctx_len;
    hu_channel_history_entry_t *history_entries = rt->history_entries;
    size_t history_count = rt->history_count;
    const hu_channel_history_entry_t *ctx_entries = rt->ctx_entries;
    size_t ctx_count = rt->ctx_count;
    hu_inner_thought_store_t *inner_thought_store = rt->inner_thought_store;
    bool inner_thought_store_ok = rt->inner_thought_store_ok;
    uint32_t daemon_turn_counter = rt->daemon_turn_counter;
    hu_repair_signal_t *repair_signal = rt->repair_signal;
#if defined(HU_ENABLE_SQLITE)
    /* Phase 4: Style drift check counter (only with persona + sqlite). Was a
     * function-scope static of hu_service_run; this region is its only user. */
    static unsigned drift_check_counter = 0;
#endif
    /* Some of these are read only inside feature-gated phases. */
    (void)config;
    (void)combined;
    (void)combined_len;
    (void)history_entries;
    (void)history_count;
    (void)inner_thought_store;
    (void)inner_thought_store_ok;
    (void)daemon_turn_counter;
    (void)repair_signal;

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
            size_t ls_len = 0;
            char *ls_ctx =
                hu_life_sim_build_context_now(alloc, &agent->persona->daily_routine, &ls_len);
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
                hu_theory_of_mind_deviation_t dev =
                    hu_theory_of_mind_detect_deviation(&tom_baseline, ctx_entries, ctx_count);
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
                    char *tom_inf = hu_theory_of_mind_build_inference(alloc, contact_name, name_len,
                                                                      NULL, 0, &dev, &tom_len);
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
            if (hu_anticipatory_predict_with_provider(alloc, agent->memory, &agent->provider,
                                                      agent->model_name, agent->model_name_len,
                                                      batch_key, key_len, (int64_t)time(NULL),
                                                      &preds, &pred_count) == HU_OK &&
                preds && pred_count > 0) {
                const char *cname = (cp_p6 && cp_p6->name) ? cp_p6->name : batch_key;
                size_t cname_len = (cp_p6 && cp_p6->name) ? strlen(cp_p6->name) : key_len;
                size_t ant_len = 0;
                char *ant_dir = hu_anticipatory_build_directive(alloc, preds, pred_count, cname,
                                                                cname_len, &ant_len);
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
            if (hu_self_awareness_build_directive_from_memory(alloc, agent->memory, batch_key,
                                                              key_len, (int64_t)time(NULL), &sa_dir,
                                                              &sa_len) == HU_OK &&
                sa_dir && sa_len > 0) {
                PHASE6_APPEND(sa_dir, sa_len);
            }
            char *rec_dir = NULL;
            size_t rec_len = 0;
            if (hu_self_awareness_build_reciprocity_directive(
                    alloc, agent->memory, batch_key, key_len, &rec_dir, &rec_len) == HU_OK &&
                rec_dir && rec_len > 0) {
                PHASE6_APPEND(rec_dir, rec_len);
            }
        }
#endif

        /* 6. Life chapter (F66) */
#ifdef HU_ENABLE_SQLITE
        if (agent->memory) {
            hu_life_chapter_t lc = {0};
            if (hu_life_chapter_get_active(alloc, agent->memory, &lc) == HU_OK && lc.theme[0]) {
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
            if (hu_social_graph_get(alloc, agent->memory, batch_key, key_len, &rels, &rel_count) ==
                    HU_OK &&
                rels && rel_count > 0) {
                const char *cname = (cp_p6 && cp_p6->name) ? cp_p6->name : batch_key;
                size_t cname_len = (cp_p6 && cp_p6->name) ? strlen(cp_p6->name) : key_len;
                size_t sg_len = 0;
                char *sg_dir = hu_social_graph_build_directive(alloc, cname, cname_len, rels,
                                                               rel_count, &sg_len);
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
                if (hu_timezone_build_directive(alloc, &tz, cname_tz, cname_tz_len, &tz_dir,
                                                &tz_len) == HU_OK &&
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
                    ? hu_daemon_detect_emotion(alloc, agent, history_entries, history_count)
                    : (hu_emotional_state_t){0};
            bool playful =
                (combined_len > 0) && (strstr(combined, "lol") || strstr(combined, "haha") ||
                                       strstr(combined, "😂") || strstr(combined, "😄"));
            if (playful && !emo_humor.concerning) {
                /* Check timing appropriateness (Phase 3) */
                time_t hum_now = time(NULL);
                struct tm hum_tm;
                int hum_hour = 12;
                if (hu_platform_localtime_r(&hum_now, &hum_tm))
                    hum_hour = hum_tm.tm_hour;
                float hum_valence = emo_humor.valence;
                const char *rel_stage =
                    (cp_p6 && cp_p6->relationship_stage) ? cp_p6->relationship_stage : "friend";
                hu_humor_timing_result_t timing =
                    hu_humor_check_timing(hum_hour, hum_valence, false, rel_stage);

                if (timing.allowed) {
                    const char *dom =
                        emo_humor.dominant_emotion ? emo_humor.dominant_emotion : "neutral";
                    size_t hum_len = 0;
                    char *hum_dir = hu_humor_build_persona_directive(
                        alloc, &agent->persona->humor, dom, strlen(dom), true, &hum_len);
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
                if (hu_weather_awareness_build_directive(alloc, &wx, wx_hour, &wx_dir, &wx_len) ==
                        HU_OK &&
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
            size_t hint_len =
                combined_len < sizeof(topic_hint) - 1 ? combined_len : sizeof(topic_hint) - 1;
            memcpy(topic_hint, combined, hint_len);
            topic_hint[hint_len] = '\0';
            uint64_t now_ms = (uint64_t)time(NULL) * 1000ULL;
            size_t surfaced_count = hu_inner_thought_surface(
                inner_thought_store, batch_key, key_len, topic_hint, hint_len, now_ms,
                surfaced_thoughts, HU_INNER_THOUGHT_MAX_SURFACE);
            if (surfaced_count > 0) {
                /* Build a directive from surfaced thoughts */
                char thought_dir[512];
                int tdir_len = 0;
                for (size_t si = 0; si < surfaced_count && si < 3; si++) {
                    const hu_inner_thought_t *th = surfaced_thoughts[si];
                    if (th->thought_text && th->thought_text_len > 0) {
                        int wrote =
                            snprintf(thought_dir + tdir_len, sizeof(thought_dir) - (size_t)tdir_len,
                                     "%s[Inner thought: %.*s]", tdir_len > 0 ? " " : "",
                                     (int)th->thought_text_len, th->thought_text);
                        if (wrote > 0 && (size_t)wrote < sizeof(thought_dir) - (size_t)tdir_len)
                            tdir_len += wrote;
                    }
                }
                if (tdir_len > 0) {
                    size_t td_alloc_len = (size_t)tdir_len;
                    char *td_copy = (char *)alloc->realloc(alloc->ctx, NULL, 0, td_alloc_len + 1);
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
                    for (size_t di = 0; di < agent->persona->important_dates_count && de_count < 16;
                         di++) {
                        const hu_important_date_t *id = &agent->persona->important_dates[di];
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
                            date_entries, de_count, cur_year, cur_month, cur_day, 7, ann_buf, 8);
                }

                /* Detect life transitions from recent messages */
                hu_life_transition_t transition = HU_TRANSITION_NONE;
                if (combined_len > 0) {
                    hu_temporal_message_t tmsg = {.text = combined, .text_len = combined_len};
                    transition = hu_temporal_detect_life_transition(&tmsg, 1);
                }

                /* Build temporal context directive */
                char *temp_dir = NULL;
                size_t temp_dir_len = 0;
                if (hu_temporal_build_context(alloc, cur_month, cur_day, ann_buf, ann_count,
                                              transition, &temp_dir, &temp_dir_len) == HU_OK &&
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
                size_t syc_topic_len =
                    combined_len < sizeof(syc_topic) - 1 ? combined_len : sizeof(syc_topic) - 1;
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
                char *cp_dir = hu_opinion_contrarian_prompt(alloc, syc_topic, syc_topic_len,
                                                            daemon_turn_counter, &cp_len);
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
                    snprintf(drift_bl.common_phrases, sizeof(drift_bl.common_phrases), "%s",
                             agent->persona->signature_phrases[0]);
            }
            if (hu_style_drift_check(agent->memory, alloc, &drift_bl, &drift_res) == HU_OK &&
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
        if (repair_signal->should_acknowledge && repair_signal->directive[0] != '\0') {
            size_t rlen = strlen(repair_signal->directive);
            char *rc = (char *)alloc->alloc(alloc->ctx, rlen + 1);
            if (rc) {
                memcpy(rc, repair_signal->directive, rlen + 1);
                PHASE6_APPEND(rc, rlen);
            }
            memset(repair_signal, 0, sizeof(*repair_signal));
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
                    if (hu_prospective_check_triggers(alloc, db, "keyword", combined, combined_len,
                                                      batch_key, key_len, now_ts, &prosp_entries,
                                                      &prosp_count) == HU_OK &&
                        prosp_entries && prosp_count > 0) {
                        char prosp_buf[1024];
                        size_t prosp_pos = 0;
                        int n = snprintf(prosp_buf, sizeof(prosp_buf),
                                         "[PROSPECTIVE MEMORY: Remember to: ");
                        if (n > 0 && (size_t)n < sizeof(prosp_buf))
                            prosp_pos = (size_t)n;
                        for (size_t pi = 0;
                             pi < prosp_count && pi < 3 && prosp_pos < sizeof(prosp_buf) - 64;
                             pi++) {
                            if (pi > 0) {
                                prosp_buf[prosp_pos++] = ' ';
                                prosp_buf[prosp_pos++] = '|';
                                prosp_buf[prosp_pos++] = ' ';
                            }
                            int w = snprintf(prosp_buf + prosp_pos, sizeof(prosp_buf) - prosp_pos,
                                             "%s (triggered by: %s)", prosp_entries[pi].action,
                                             prosp_entries[pi].trigger_value);
                            if (w > 0 && prosp_pos + (size_t)w < sizeof(prosp_buf))
                                prosp_pos += (size_t)w;
                        }
                        if (prosp_pos + 2 < sizeof(prosp_buf)) {
                            prosp_buf[prosp_pos++] = ']';
                            prosp_buf[prosp_pos] = '\0';
                            char *prosp_str = (char *)alloc->alloc(alloc->ctx, prosp_pos + 1);
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
                    if (hu_emotional_residue_get_active(alloc, db, batch_key, key_len, now_ts,
                                                        &residues, &res_count) == HU_OK &&
                        residues && res_count > 0) {
                        size_t dir_len = 0;
                        char *dir = hu_emotional_residue_build_directive(alloc, residues, res_count,
                                                                         &dir_len);
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
                    if (hu_emotional_residue_get_active(alloc, db, batch_key, key_len, now_ts,
                                                        &carry_res, &carry_count) == HU_OK &&
                        carry_res && carry_count > 0) {
                        double *valences =
                            (double *)alloc->alloc(alloc->ctx, carry_count * sizeof(double));
                        double *intensities =
                            (double *)alloc->alloc(alloc->ctx, carry_count * sizeof(double));
                        int64_t *timestamps =
                            (int64_t *)alloc->alloc(alloc->ctx, carry_count * sizeof(int64_t));
                        if (valences && intensities && timestamps) {
                            for (size_t cr = 0; cr < carry_count; cr++) {
                                valences[cr] = carry_res[cr].valence;
                                intensities[cr] = carry_res[cr].intensity;
                                timestamps[cr] = carry_res[cr].created_at;
                            }
                            hu_residue_carryover_t carryover = {0};
                            if (hu_residue_carryover_compute(valences, intensities, timestamps,
                                                             carry_count, now_ts,
                                                             &carryover) == HU_OK) {
                                size_t carry_dir_len = 0;
                                char *carry_dir = hu_residue_carryover_build_directive(
                                    alloc, &carryover, &carry_dir_len);
                                if (carry_dir && carry_dir_len > 0)
                                    PHASE6_APPEND(carry_dir, carry_dir_len);
                                else if (carry_dir)
                                    alloc->free(alloc->ctx, carry_dir, carry_dir_len + 1);
                            }
                        }
                        if (valences)
                            alloc->free(alloc->ctx, valences, carry_count * sizeof(double));
                        if (intensities)
                            alloc->free(alloc->ctx, intensities, carry_count * sizeof(double));
                        if (timestamps)
                            alloc->free(alloc->ctx, timestamps, carry_count * sizeof(int64_t));
                        alloc->free(alloc->ctx, carry_res,
                                    carry_count * sizeof(hu_emotional_residue_t));
                    }
                }

                /* 10c. Evolving opinions — inject developed perspectives */
                {
                    hu_evolved_opinions_ensure_table(db);
                    hu_evolved_opinion_t *evo_opinions = NULL;
                    size_t evo_count = 0;
                    if (hu_evolved_opinions_get(alloc, db, 0.4, 5, &evo_opinions, &evo_count) ==
                            HU_OK &&
                        evo_opinions && evo_count > 0) {
                        size_t op_dir_len = 0;
                        char *op_dir = hu_evolved_opinion_build_directive(
                            alloc, evo_opinions, evo_count, 0.4, &op_dir_len);
                        if (op_dir && op_dir_len > 0)
                            PHASE6_APPEND(op_dir, op_dir_len);
                        else if (op_dir)
                            alloc->free(alloc->ctx, op_dir, op_dir_len + 1);

                        /* Roadmap #14: stances persist under pushback. When the
                         * inbound challenges a held opinion, direct the model to
                         * hold its position. Gated HU_OPINION_HOLD=off|shadow|live,
                         * default OFF; do not flip to default-ON without a stance-
                         * retention measurement (feature-gate-requires-measurement).
                         * Shadow logs would-fire without injecting. */
                        {
                            hu_gate_mode_t hold_mode =
                                hu_gate_mode_from_env("HU_OPINION_HOLD", HU_GATE_OFF);
                            if (hold_mode != HU_GATE_OFF && combined_len > 0) {
                                for (size_t oi = 0; oi < evo_count; oi++) {
                                    char *hold_dir = NULL;
                                    size_t hold_len = 0;
                                    bool hold_would = false;
                                    if (hu_opinion_challenge_directive(
                                            alloc, hold_mode, combined, combined_len,
                                            evo_opinions[oi].topic, evo_opinions[oi].topic_len,
                                            evo_opinions[oi].stance, evo_opinions[oi].stance_len,
                                            &hold_dir, &hold_len, &hold_would) != HU_OK)
                                        continue;
                                    if (!hold_would)
                                        continue;
                                    hu_log_info("opinion_hold", agent ? agent->observer : NULL,
                                                "%s: inbound challenges stance [%.*s]",
                                                hold_mode == HU_GATE_LIVE ? "live" : "shadow",
                                                (int)evo_opinions[oi].topic_len,
                                                evo_opinions[oi].topic);
                                    if (hold_dir && hold_len > 0)
                                        PHASE6_APPEND(hold_dir, hold_len);
                                    break; /* one hold directive max per turn */
                                }
                            }
                        }
                        hu_evolved_opinions_free(alloc, evo_opinions, evo_count);
                    }
                }

                /* 11. Episodic context — last 5 episodes for this contact */
                {
                    hu_episode_sqlite_t *episodes = NULL;
                    size_t ep_count = 0;
                    if (hu_episode_get_by_contact(alloc, db, batch_key, key_len, 5, 0, &episodes,
                                                  &ep_count) == HU_OK &&
                        episodes && ep_count > 0) {
                        char ep_buf[4096];
                        size_t ep_pos = 0;
                        static const char ep_hdr[] =
                            "[SHARED HISTORY with this person — reference specific "
                            "details when relevant, not generic empathy: ";
                        int n = snprintf(ep_buf, sizeof(ep_buf), "%s", ep_hdr);
                        if (n > 0 && (size_t)n < sizeof(ep_buf))
                            ep_pos = (size_t)n;
                        for (size_t ei = 0; ei < ep_count && ep_pos < sizeof(ep_buf) - 64; ei++) {
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
                            (void)hu_episode_reinforce(db, episodes[ei].id, (int64_t)time(NULL));
                        hu_episode_free(alloc, episodes, ep_count);
                    }
                }
            }
        }
#endif

        /* F148 (Pillar 29): On-device message classification */
        if (batch_key && key_len > 0) {
            double cls_conf = 0.0;
            hu_classify_result_t cls = hu_classify_message(batch_key, key_len, &cls_conf);
            if (cls_conf > 0.5) {
                char *cls_dir = NULL;
                size_t cls_len = 0;
                if (hu_classifier_build_prompt(alloc, cls, cls_conf, &cls_dir, &cls_len) == HU_OK &&
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
                    if (sqlite3_prepare_v2(kdb, ksql, (int)ksql_len, &kstmt, NULL) == SQLITE_OK) {
                        hu_knowledge_entry_t kentries[8];
                        size_t kcount = 0;
                        while (sqlite3_step(kstmt) == SQLITE_ROW && kcount < 8) {
                            hu_knowledge_entry_t *ke = &kentries[kcount];
                            memset(ke, 0, sizeof(*ke));
                            const char *kt = (const char *)sqlite3_column_text(kstmt, 0);
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
                            if (hu_knowledge_build_summary(alloc, kentries, kcount, batch_key,
                                                           key_len, NULL, 0, &ksummary) == HU_OK) {
                                char *kprompt = NULL;
                                size_t kprompt_len = 0;
                                if (hu_knowledge_summary_to_prompt(alloc, &ksummary, &kprompt,
                                                                   &kprompt_len) == HU_OK &&
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
                if (hu_compression_query_sql(batch_key, key_len, csql, sizeof(csql), &csql_len) ==
                    HU_OK) {
                    sqlite3_stmt *cstmt = NULL;
                    if (sqlite3_prepare_v2(cdb, csql, (int)csql_len, &cstmt, NULL) == SQLITE_OK) {
                        hu_shared_ref_t crefs[8];
                        size_t ccount = 0;
                        while (sqlite3_step(cstmt) == SQLITE_ROW && ccount < 8) {
                            hu_shared_ref_t *cr = &crefs[ccount];
                            memset(cr, 0, sizeof(*cr));
                            const char *ck = (const char *)sqlite3_column_text(cstmt, 0);
                            if (ck) {
                                cr->compressed_form_len = (size_t)sqlite3_column_bytes(cstmt, 0);
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
                            if (hu_compression_build_prompt(alloc, crefs, ccount, &cprompt,
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
                    hu_skill_match_triggers(alloc, skill_db, batch_key, key_len, NULL, 0, NULL, 0,
                                            0.5, &matched, &matched_count);
                if (skill_err == HU_OK && matched && matched_count > 0) {
                    size_t max_skills = matched_count > 3 ? 3 : matched_count;
                    for (size_t si = 0; si < max_skills; si++) {
                        /* Inject skill strategy as directive */
                        size_t strat_len = matched[si].strategy_len > 0
                                               ? matched[si].strategy_len
                                               : strlen(matched[si].strategy);
                        if (strat_len > 0 && strat_len < 500) {
                            hu_log_info("human", agent ? agent->observer : NULL, "skill: %s",
                                        matched[si].name);
                            char skill_buf[512];
                            int sb =
                                snprintf(skill_buf, sizeof(skill_buf), "[SKILL %s]: %.*s",
                                         matched[si].name, (int)strat_len, matched[si].strategy);
                            if (sb > 0 && (size_t)sb < sizeof(skill_buf)) {
                                char *skill_str = (char *)alloc->alloc(alloc->ctx, (size_t)sb + 1);
                                if (skill_str) {
                                    memcpy(skill_str, skill_buf, (size_t)sb + 1);
                                    PHASE6_APPEND(skill_str, (size_t)sb);
                                }
                            }
                        }
                        /* Record attempt and update success rate */
                        int64_t attempt_id = 0;
                        hu_skill_record_attempt(skill_db, matched[si].id, batch_key, key_len,
                                                (int64_t)time(NULL), NULL, 0, NULL, 0, NULL, 0,
                                                &attempt_id);
                        (void)hu_skill_update_success_rate(skill_db, matched[si].id, 1, 0);
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
            hu_cognitive_load_state_t cog = hu_cognitive_load_calculate(&cog_cfg, 0, t_p9);
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
                hu_log_info("human", agent ? agent->observer : NULL, "Phase 9: physical state: %s",
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
                const char *complaint =
                    hu_mundane_complaint_prompt(lt_p9->tm_hour, lt_p9->tm_wday, phys, NULL);
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
            uint32_t auth_seed = (uint32_t)((uint64_t)time(NULL) ^ (uintptr_t)batch_key);
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
                hu_authentic_build_directive(alloc, behavior, NULL, 0, &auth_dir, &auth_len);
                if (auth_dir && auth_len > 0)
                    PHASE6_APPEND(auth_dir, auth_len);
                else if (auth_dir)
                    alloc->free(alloc->ctx, auth_dir, auth_len + 1);
            }
        }

        /* 10. Cognitive load (F102) */
        {
            double load =
                hu_cognitive_compute_load(ctx_count > 0 ? (uint32_t)ctx_count : 1, 1, false);
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
                hu_rel_dynamics_build_prompt(alloc, &rel_vel, &drift, &repair, &rel_dir, &rel_len);
                if (rel_dir && rel_len > 0)
                    PHASE6_APPEND(rel_dir, rel_len);
                else if (rel_dir)
                    alloc->free(alloc->ctx, rel_dir, rel_len + 1);
            }
        }

        /* F65 (opinion injection from the legacy `opinions` table) was removed
         * 2026-07-18: the table had no production writer and the query keyed
         * topic=<contact key>, so the block always injected nothing. Held
         * opinions reach the prompt via the evolved_opinions block (10c),
         * which also carries the HU_OPINION_HOLD stance-hold directive. */

        /* F83-F93: Feed context — inject recent relevant feed items */
#ifdef HU_ENABLE_SQLITE
        if (agent->memory && batch_key && key_len > 0) {
            hu_feed_item_stored_t *fitems = NULL;
            size_t fcount = 0;
            sqlite3 *fdb = hu_sqlite_memory_get_db(agent->memory);
            if (fdb &&
                hu_feed_processor_get_for_contact(alloc, fdb, batch_key, key_len, 5, &fitems,
                                                  &fcount) == HU_OK &&
                fitems && fcount > 0) {
                hu_feed_item_t *conv_items =
                    (hu_feed_item_t *)alloc->alloc(alloc->ctx, fcount * sizeof(hu_feed_item_t));
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
                    alloc->free(alloc->ctx, conv_items, fcount * sizeof(hu_feed_item_t));
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
                if (hu_visual_match_for_contact(alloc, vdb, batch_key, key_len, combined,
                                                combined_len, &ventries, &vcount) == HU_OK &&
                    ventries && vcount > 0) {
                    bool should_share = false;
                    double vconf = 0.0;
                    hu_visual_should_share(&ventries[0], combined, combined_len, &should_share,
                                           &vconf);
                    if (should_share) {
                        hu_visual_candidate_t vc = {0};
                        vc.path = ventries[0].path;
                        vc.path_len = strlen(ventries[0].path);
                        vc.description = ventries[0].description;
                        vc.description_len = strlen(ventries[0].description);
                        vc.relevance_score = vconf;
                        char *vprompt = NULL;
                        size_t vprompt_len = 0;
                        if (hu_visual_build_prompt(alloc, &vc, 1, &vprompt, &vprompt_len) ==
                                HU_OK &&
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
                                                        sizeof(fwd_sql), &fwd_sql_len) == HU_OK) {
                    sqlite3_stmt *fwd_stmt = NULL;
                    if (sqlite3_prepare_v2(fwd_db, fwd_sql, (int)fwd_sql_len, &fwd_stmt, NULL) ==
                        SQLITE_OK) {
                        if (sqlite3_step(fwd_stmt) == SQLITE_ROW) {
                            const char *fc = (const char *)sqlite3_column_text(fwd_stmt, 1);
                            const char *fs = (const char *)sqlite3_column_text(fwd_stmt, 2);
                            if (fc && fs) {
                                size_t fc_len = (size_t)sqlite3_column_bytes(fwd_stmt, 1);
                                char fwd_buf[512];
                                int fw = snprintf(fwd_buf, sizeof(fwd_buf),
                                                  "[SHAREABLE CONTENT from %s]: %.*s — "
                                                  "Share this naturally if relevant.",
                                                  fs, (int)(fc_len > 300 ? 300 : fc_len), fc);
                                if (fw > 0 && (size_t)fw < sizeof(fwd_buf)) {
                                    char *fwd_str =
                                        (char *)alloc->alloc(alloc->ctx, (size_t)fw + 1);
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
                     ni < agent->persona->context_awareness.news_topics_count && ni < 3; ni++) {
                    const char *topic = agent->persona->context_awareness.news_topics[ni];
                    if (!topic || !topic[0])
                        continue;
                    size_t tlen = strlen(topic);
                    char ev_sql[512];
                    size_t ev_sql_len = 0;
                    if (hu_events_create_table_sql(ev_sql, sizeof(ev_sql), &ev_sql_len) != HU_OK)
                        break;
                    /* Query events by topic */
                    char eq_sql[512];
                    size_t eq_len = 0;
                    int qn = snprintf(eq_sql, sizeof(eq_sql),
                                      "SELECT topic, summary, source FROM current_events "
                                      "WHERE topic LIKE '%%%.*s%%' ORDER BY published_at "
                                      "DESC LIMIT 3",
                                      (int)tlen, topic);
                    if (qn <= 0 || (size_t)qn >= sizeof(eq_sql))
                        continue;
                    eq_len = (size_t)qn;
                    sqlite3_stmt *ev_stmt = NULL;
                    if (sqlite3_prepare_v2(ev_db, eq_sql, (int)eq_len, &ev_stmt, NULL) ==
                        SQLITE_OK) {
                        if (sqlite3_step(ev_stmt) == SQLITE_ROW) {
                            const char *summary = (const char *)sqlite3_column_text(ev_stmt, 1);
                            if (summary) {
                                size_t slen = (size_t)sqlite3_column_bytes(ev_stmt, 1);
                                char ev_buf[384];
                                int ew = snprintf(ev_buf, sizeof(ev_buf),
                                                  "[CURRENT EVENT — %.*s]: %.*s", (int)tlen, topic,
                                                  (int)(slen > 250 ? 250 : slen), summary);
                                if (ew > 0 && (size_t)ew < sizeof(ev_buf)) {
                                    char *ev_str = (char *)alloc->alloc(alloc->ctx, (size_t)ew + 1);
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
            alloc, ctx_entries, ctx_count, (agent && agent->persona) ? agent->persona : NULL,
            &convo_ctx_len);

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
            if (hu_conversation_detect_topic_change(ctx_entries, ctx_count, topic_before,
                                                    sizeof(topic_before), topic_after,
                                                    sizeof(topic_after))) {
                size_t tb_len = strlen(topic_before);
                if (tb_len > 0)
                    (void)hu_superhuman_avoidance_record(agent->memory, batch_key, key_len,
                                                         topic_before, tb_len, true);

                hu_consolidation_set_topic_switch(true);

                /* Topic-switch consolidation (EdgeClaw-inspired):
                 * trigger memory consolidation on topic change
                 * with debounce to avoid excessive consolidation. */
                {
                    int64_t tc_now = (int64_t)time(NULL);
                    if (hu_consolidation_should_run(&topic_consolidation_debounce, tc_now)) {
                        hu_consolidation_config_t tc_cfg =
                            hu_daemon_consolidation_config(config, agent);
                        if (hu_memory_consolidate(alloc, agent->memory, &tc_cfg) == HU_OK) {
                            hu_consolidation_debounce_reset(&topic_consolidation_debounce, tc_now);
                            hu_log_info("human", agent ? agent->observer : NULL,
                                        "topic-switch consolidation: '%s' -> '%s'", topic_before,
                                        topic_after);
                        }
                    }
                }
            }
        }
    }

    rt->convo_ctx = convo_ctx;
    rt->convo_ctx_len = convo_ctx_len;
#else
    (void)alloc;
    (void)agent;
    (void)config;
    (void)rt;
#endif /* HU_IS_TEST */
}
