/**
 * daemon_reactive_context.c — per-batch context loading for the reactive reply
 * path (slice A of the hu_service_run batch-reply carve-out).
 *
 * Pure move of src/daemon.c lines 5606-6036 as of origin/main 8ec08adab, see
 * docs/plans/2026-09-02-daemon-batch-reply-carveout.md. The body below is the
 * original text: inputs are aliased under their historical names at the top and
 * the outputs are written back to the turn context at the bottom, every #if
 * gate kept where it was. The only textual change is the expansion of the
 * daemon.c-private `daemon_contact_activity_record` macro, whose g_proactive_ctx
 * now arrives as rt->proactive_ctx.
 */
#include "human/daemon/reactive_turn.h"

#include "human/agent.h"
#include "human/channel.h"
#include "human/config.h"
#include "human/core/log.h"
#include "human/core/string.h"
#include "human/daemon.h"
#include "human/daemon/message_router.h"
#include "human/daemon_comfort_summary.h"
#include "human/daemon_proactive.h"
#include "human/memory.h"
#include "human/persona.h"
#include "human/persona/auto_profile.h"
/* comfort_patterns.c is compiled on every variant (no-sqlite stubs return
 * HU_ERR_NOT_SUPPORTED) and hu_comfort_pattern_record is called outside the
 * SQLITE guard below, so its header must be visible unconditionally — the
 * cross-arm64 (-DHU_ENABLE_SQLITE=OFF) build failed with an implicit
 * declaration when it sat inside the #ifdef. */
#include "human/memory/comfort_patterns.h"
#ifdef HU_ENABLE_SQLITE
#include "human/memory/contact_graph.h"
#endif

#include <stdint.h>
#include <stdio.h>
#include <string.h>

void hu_daemon_reactive_context_load(hu_allocator_t *alloc, hu_agent_t *agent,
                                     const hu_config_t *config, hu_service_channel_t *channels,
                                     size_t channel_count, hu_reactive_turn_ctx_t *rt) {
    /* Historical names for the moved body (see file comment). */
    hu_service_channel_t *ch = rt->ch;
    const char *batch_key = rt->batch_key;
    size_t key_len = rt->key_len;
    const char *combined = rt->combined;
    size_t combined_len = rt->combined_len;
    bool llm_decides = rt->llm_decides;
    hu_daemon_comfort_pending_t *comfort_pending = rt->comfort_pending;
    /* Only the HU_IS_TEST-gated tail reads these. */
    (void)channels;
    (void)channel_count;
    (void)combined;
    (void)combined_len;
    (void)llm_decides;
    (void)comfort_pending;

    hu_agent_clear_history(agent);

    /* Set active channel for per-channel persona overlays */
    if (ch->channel->vtable->name) {
        agent->active_channel = ch->channel->vtable->name(ch->channel->ctx);
        agent->active_channel_len = agent->active_channel ? strlen(agent->active_channel) : 0;
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
            persona_override = hu_config_persona_for_channel(config, agent->active_channel);
        if (persona_override && persona_override[0]) {
            const char *current = agent->persona_name ? agent->persona_name : "";
            if (strcmp(persona_override, current) != 0) {
                hu_error_t perr =
                    hu_agent_set_persona(agent, persona_override, strlen(persona_override));
                if (perr != HU_OK) {
#ifndef HU_IS_TEST
                    hu_log_error("human", agent ? agent->observer : NULL,
                                 "warning: failed to switch persona to '%s'", persona_override);
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
        if (agent->session_store->vtable->load_messages(agent->session_store->ctx, alloc, batch_key,
                                                        key_len, &entries, &entry_count) == HU_OK &&
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
                        alloc->ctx, agent->history, agent->history_cap * sizeof(hu_owned_message_t),
                        new_cap * sizeof(hu_owned_message_t));
                    if (!arr)
                        break;
                    agent->history = arr;
                    agent->history_cap = new_cap;
                }
                hu_owned_message_t *hm = &agent->history[agent->history_count];
                memset(hm, 0, sizeof(*hm));
                hm->role = role;
                hm->content = hu_strndup(agent->alloc, entries[e].content, entries[e].content_len);
                hm->content_len = entries[e].content_len;
                if (hm->content)
                    agent->history_count++;
            }
            for (size_t e = 0; e < entry_count; e++) {
                if (entries[e].role)
                    alloc->free(alloc->ctx, (void *)entries[e].role, entries[e].role_len + 1);
                if (entries[e].content)
                    alloc->free(alloc->ctx, (void *)entries[e].content, entries[e].content_len + 1);
            }
            alloc->free(alloc->ctx, entries, entry_count * sizeof(hu_message_entry_t));
        }
    }

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
    /* 1. Per-contact profile via persona struct */
    if (agent->persona) {
        const hu_contact_profile_t *cp =
            hu_persona_find_contact(agent->persona, batch_key, key_len);
        if (cp) {
            contact_for_tapback = cp;
            if (ch->channel->vtable->name && cp->contact_id) {
                const char *act_ch = ch->channel->vtable->name(ch->channel->ctx);
                if (act_ch) {
                    hu_daemon_contact_activity_record(rt->proactive_ctx, cp->contact_id, act_ch,
                                                      batch_key);
#ifdef HU_ENABLE_SQLITE
                    /* Link this platform handle to canonical contact ID */
                    if (agent->memory) {
                        sqlite3 *link_db = hu_sqlite_memory_get_db(agent->memory);
                        if (link_db) {
                            hu_contact_graph_link(link_db, cp->contact_id, act_ch, batch_key,
                                                  cp->name ? cp->name : "", 1.0);
                        }
                    }
#endif
                }
            }
            hu_contact_profile_build_context(alloc, cp, &contact_ctx, &contact_ctx_len);

            size_t iw_len = 0;
            char *iw_ctx =
                llm_decides ? NULL
                            : hu_persona_build_inner_world_context(alloc, agent->persona,
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
                    int w = snprintf(profile_buf + pb_n, sizeof(profile_buf) - (size_t)pb_n,
                                     "Contact formality: %s. ", auto_ov.formality);
                    if (w > 0 && (size_t)pb_n + (size_t)w < sizeof(profile_buf))
                        pb_n += w;
                }
                if (auto_ov.avg_length) {
                    int w = snprintf(profile_buf + pb_n, sizeof(profile_buf) - (size_t)pb_n,
                                     "Avg message length: %s. ", auto_ov.avg_length);
                    if (w > 0 && (size_t)pb_n + (size_t)w < sizeof(profile_buf))
                        pb_n += w;
                }
                if (auto_ov.emoji_usage) {
                    int w = snprintf(profile_buf + pb_n, sizeof(profile_buf) - (size_t)pb_n,
                                     "Emoji usage: %s. ", auto_ov.emoji_usage);
                    if (w > 0 && (size_t)pb_n + (size_t)w < sizeof(profile_buf))
                        pb_n += w;
                }
                if (auto_ov.style_notes) {
                    for (size_t sn = 0; sn < auto_ov.style_notes_count && (size_t)pb_n < 900;
                         sn++) {
                        if (auto_ov.style_notes[sn]) {
                            int w = snprintf(profile_buf + pb_n, sizeof(profile_buf) - (size_t)pb_n,
                                             "%s ", auto_ov.style_notes[sn]);
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
                                memcpy(merged + contact_ctx_len + 1, note, (size_t)pb_n);
                                merged[total] = '\0';
                                alloc->free(alloc->ctx, contact_ctx, contact_ctx_len + 1);
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
        ch->channel->vtable->load_conversation_history(ch->channel->ctx, alloc, batch_key, key_len,
                                                       25, &history_entries, &history_count);
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
                float eng = hu_daemon_score_comfort_engagement(combined, combined_len);
                (void)hu_comfort_pattern_record(
                    agent->memory, batch_key, key_len, comfort_pending[cp_i].emotion,
                    strlen(comfort_pending[cp_i].emotion), comfort_pending[cp_i].response_type,
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
    if (agent->memory && ch->channel->vtable->name && batch_key && key_len > 0 && key_len < 512 &&
        !llm_decides) {
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
                    if (hu_contact_graph_list(alloc, cg_db, canonical, &idents, &id_count) ==
                            HU_OK &&
                        idents && id_count > 0) {
                        for (size_t ci = 0; ci < channel_count; ci++) {
                            if (&channels[ci] == ch)
                                continue;
                            hu_channel_t *och = channels[ci].channel;
                            if (!och || !och->vtable || !och->vtable->load_conversation_history ||
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
                                och->ctx, alloc, ohandle, strlen(ohandle), 5, &oent, &onc);
                            if (oh == HU_OK && oent && onc > 0) {
                                size_t start = onc > 5 ? onc - 5 : 0;
                                char plabel[64];
                                hu_daemon_cross_channel_platform_label(oname, plabel,
                                                                       sizeof(plabel));
                                for (size_t ei = start; ei < onc; ei++) {
                                    char when[48];
                                    hu_daemon_cross_channel_format_when(when, sizeof(when),
                                                                        oent[ei].timestamp);
                                    const char *role = oent[ei].from_me ? " (you)" : "";
                                    char line[768];
                                    int lw = snprintf(line, sizeof(line), "[From %s, %s]%s %s",
                                                      plabel, when, role, oent[ei].text);
                                    if (lw > 0 && (size_t)lw < sizeof(line)) {
                                        (void)hu_daemon_cross_ctx_append_line(
                                            alloc, &cross_channel_ctx, &cross_channel_ctx_len, line,
                                            (size_t)lw);
                                    }
                                }
                                alloc->free(alloc->ctx, oent,
                                            onc * sizeof(hu_channel_history_entry_t));
                            }
                        }
                        alloc->free(alloc->ctx, idents, id_count * sizeof(hu_contact_identity_t));
                    }
                }
            }
        }
    }
#endif /* HU_ENABLE_SQLITE && !HU_IS_TEST */

    rt->ctx_entries = ctx_entries;
    rt->ctx_count = ctx_count;
#endif /* HU_IS_TEST */

    rt->contact_ctx = contact_ctx;
    rt->contact_ctx_len = contact_ctx_len;
    rt->convo_ctx = convo_ctx;
    rt->convo_ctx_len = convo_ctx_len;
    rt->history_entries = history_entries;
    rt->history_count = history_count;
#if defined(HU_ENABLE_SQLITE) && !defined(HU_IS_TEST)
    rt->cross_channel_ctx = cross_channel_ctx;
    rt->cross_channel_ctx_len = cross_channel_ctx_len;
#endif
    rt->contact_for_tapback = contact_for_tapback;
}
