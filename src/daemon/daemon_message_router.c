/* iMessage reply-route dispatcher — DDD Phase 2.5 extraction from daemon.c.
 *
 * hu_daemon_dispatch_imessage_reply routes an outbound iMessage reply through
 * the reply-style predicate (Phase A) to choose threaded / flat / tapback based
 * on reply-style facts. Pure structural move — behavior unchanged. The public
 * declaration stays in human/daemon.h, so external callers are unaffected.
 *
 * Scope note: the cross-channel context formatters (cross_channel_format_when /
 * _platform_label / daemon_cross_ctx_append_line) are a separate concern and
 * remain in daemon.c for a later slice. */
#include "human/agent.h"
#include "human/channel.h"
#include "human/channels/imessage_action.h"
#include "human/channels/imessage_action_facts.h"
#include "human/config.h"
#include "human/core/log.h"
#include "human/core/time.h"
#include "human/daemon.h"
#include "human/persona/pacing.h"

/* Dispatcher: route iMessage reply through predicate (Phase A) to choose
 * between threaded / flat / tapback based on reply style facts. */
hu_error_t hu_daemon_dispatch_imessage_reply(
    struct hu_channel *ch, const struct hu_persona *persona, const struct hu_agent *agent,
    const struct hu_config *config, const char *target, size_t target_len,
    const char *parent_msg_guid, size_t parent_guid_len, const char *body, size_t body_len,
    const struct hu_conversation_snapshot *snapshot, int64_t inferred_message_id_for_react) {
    if (!ch || !ch->vtable || !target || !body) {
        return HU_ERR_INVALID_ARGUMENT;
    }

    /* Check the action-surface-v2 config gate (A5).  If disabled, do a flat
     * send and return predictably. Emit one-shot log on first disable. */
    bool asv2_enabled = (config && config->channels.imessage.action_surface_v2.enabled);
    if (!asv2_enabled) {
        static bool warned_once = false;
        if (!warned_once) {
            warned_once = true;
            hu_log_info("human", agent ? agent->observer : NULL,
                        "action_surface_v2 disabled (iMessage.action_surface_v2.enabled=false); "
                        "set true in config to enable threaded replies / tapback");
        }
        if (ch->vtable->send) {
            return ch->vtable->send(ch->ctx, target, target_len, body, body_len, NULL, 0);
        }
        return HU_ERR_NOT_SUPPORTED;
    }

    /* Build facts from snapshot + persona. */
    hu_reply_style_facts_t facts;
    hu_imessage_build_reply_facts((const hu_conversation_snapshot_t *)snapshot, persona, &facts);

    /* Pick style via predicate with seeded RNG. Mixing inferred_message_id
     * lets tests pin a deterministic seed by varying that parameter; in
     * production it's the inbound message rowid which varies naturally. */
    uint64_t rng_seed = (uint64_t)time(NULL) * 1000ULL + (uint64_t)target_len +
                        (uint64_t)inferred_message_id_for_react;
    hu_reply_style_t style = hu_imessage_choose_reply_style(&facts, rng_seed);

    /* Pacing (C5) — start. */
    uint64_t pace_start = 0;
    hu_persona_pace_reply_start(&pace_start);

    /* Dispatch by style. */
    hu_error_t err = HU_OK;
    const char *tier_used = "flat_fallback";
    switch (style) {
    case HU_REPLY_STYLE_THREADED: {
        bool threaded_attempted = (ch->vtable->reply && parent_msg_guid && parent_guid_len > 0);
        if (threaded_attempted) {
            err = ch->vtable->reply(ch->ctx, target, target_len, parent_msg_guid, parent_guid_len,
                                    body, body_len);
            if (err == HU_OK) {
                tier_used = "threaded";
                hu_log_info("human", agent ? agent->observer : NULL,
                            "imessage_dispatch: threaded reply sent");
            }
        }
        /* Fall through to flat send if (a) reply slot was NULL or
         * parent_msg_guid wasn't provided (threaded_attempted==false), or
         * (b) the reply attempt returned non-OK. Always-do-something
         * contract — never silently no-op. */
        if (!threaded_attempted || err != HU_OK) {
            if (ch->vtable->send) {
                err = ch->vtable->send(ch->ctx, target, target_len, body, body_len, NULL, 0);
                if (err == HU_OK) {
                    tier_used = "flat_fallback";
                    hu_log_info("human", agent ? agent->observer : NULL,
                                "imessage_dispatch: threaded unavailable, flat fallback");
                }
            }
        }
        break;
    }

    case HU_REPLY_STYLE_FLAT:
        if (ch->vtable->send) {
            err = ch->vtable->send(ch->ctx, target, target_len, body, body_len, NULL, 0);
            if (err == HU_OK) {
                tier_used = "flat";
                hu_log_info("human", agent ? agent->observer : NULL,
                            "imessage_dispatch: flat send");
            }
        }
        break;

    case HU_REPLY_STYLE_TAPBACK:
        if (ch->vtable->react_emoji) {
            const char *emoji = "👍"; /* universal-positive default */
            err = ch->vtable->react_emoji(ch->ctx, target, target_len,
                                          inferred_message_id_for_react, emoji, strlen(emoji));
            if (err == HU_OK) {
                tier_used = "tapback";
                hu_log_info("human", agent ? agent->observer : NULL,
                            "imessage_dispatch: tapback emoji sent");
            }
        }
        if (err != HU_OK || !ch->vtable->react_emoji) {
            if (ch->vtable->send) {
                err = ch->vtable->send(ch->ctx, target, target_len, body, body_len, NULL, 0);
                if (err == HU_OK) {
                    tier_used = "flat_fallback";
                    hu_log_info("human", agent ? agent->observer : NULL,
                                "imessage_dispatch: tapback unavailable, flat fallback");
                }
            }
        }
        break;

    case HU_REPLY_STYLE_TAPBACK_PLUS_FLAT:
        /* Both: tapback first (best-effort), then text. */
        if (ch->vtable->react_emoji) {
            const char *emoji = "❤️"; /* heart for emotional acknowledgment */
            (void)ch->vtable->react_emoji(ch->ctx, target, target_len,
                                          inferred_message_id_for_react, emoji, strlen(emoji));
        }
        if (ch->vtable->send) {
            err = ch->vtable->send(ch->ctx, target, target_len, body, body_len, NULL, 0);
            if (err == HU_OK) {
                tier_used = "tapback_plus_flat";
                hu_log_info("human", agent ? agent->observer : NULL,
                            "imessage_dispatch: tapback + flat");
            }
        }
        break;
    }

    /* Pacing — finish (sleep if elapsed < persona.min_reply_delay_ms * 1.2). */
    if (persona) {
        hu_persona_pace_reply_finish(persona, pace_start);
    }

    /* Telemetry: log the action-surface decision + outcome. */
    if (config) {
        hu_imessage_action_log_t log_entry = {0};
        log_entry.ts_unix = (int64_t)time(NULL);
        /* target_chat_id_hash: for tests, we pass target directly; in production,
         * the caller would hash it for privacy. */
        log_entry.target_chat_id_hash = target;
        log_entry.facts = facts;
        log_entry.style_chosen = style;
        log_entry.send_result = (int)err;
        log_entry.tier_used = tier_used;
        uint64_t pace_end = hu_time_get_current_ms();
        log_entry.elapsed_ms = (int)(pace_end - pace_start);
        (void)hu_imessage_action_log_jsonl(&log_entry);
    }

    return err;
}
