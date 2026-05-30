/* iMessage reply-route dispatcher — DDD Phase 2.5 extraction from daemon.c.
 *
 * hu_daemon_dispatch_imessage_reply routes an outbound iMessage reply through
 * the reply-style predicate (Phase A) to choose threaded / flat / tapback based
 * on reply-style facts. Pure structural move — behavior unchanged. The public
 * declaration stays in human/daemon.h, so external callers are unaffected.
 *
 * Also hosts the cross-channel context formatters
 * (hu_daemon_cross_channel_* / hu_daemon_cross_ctx_append_line) — a sibling
 * inter-channel concern, declared in human/daemon/message_router.h and compiled
 * only under SQLite + non-test builds. */
/* strptime() is an XSI/GNU extension; glibc (Linux / nix) declares it only
 * when _GNU_SOURCE is defined before any include. macOS declares it
 * unconditionally, so this define is a harmless no-op there. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "human/agent.h"
#include "human/channel.h"
#include "human/channels/imessage_action.h"
#include "human/channels/imessage_action_facts.h"
#include "human/channels/imessage_reply.h"
#include "human/config.h"
#include "human/core/log.h"
#include "human/core/time.h"
#include "human/daemon.h"
#include "human/daemon/message_router.h"
#include "human/persona/pacing.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

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

    /* Dispatch by style. `actual_style` tracks what was ACTUALLY sent (which
     * can differ from the chosen `style` — e.g. a THREADED attempt that the AX
     * Return silently committed flat). Telemetry records the actual outcome,
     * not the intent. */
    hu_error_t err = HU_OK;
    const char *tier_used = "flat_fallback";
    hu_reply_style_t actual_style = style;
    switch (style) {
    case HU_REPLY_STYLE_THREADED: {
        bool threaded_attempted = (ch->vtable->reply && parent_msg_guid && parent_guid_len > 0);
        if (threaded_attempted) {
            err = ch->vtable->reply(ch->ctx, target, target_len, parent_msg_guid, parent_guid_len,
                                    body, body_len);
            if (err == HU_OK) {
                /* The reply returned OK, but on macOS 26+ the AX value-inject +
                 * Return path can commit a FLAT message even on success. Read
                 * the verified-threaded result back from the reply layer rather
                 * than assuming THREADED on any HU_OK. (When vtable->reply is a
                 * mock in tests, the getter stays false — inert, no test pins
                 * it.) */
                bool verified = hu_imessage_reply_last_verified_threaded();
                if (verified) {
                    tier_used = "threaded";
                    actual_style = HU_REPLY_STYLE_THREADED;
                    hu_log_info("human", agent ? agent->observer : NULL,
                                "imessage_dispatch: threaded reply sent (native thread verified)");
                } else {
                    tier_used = "threaded_flat";
                    actual_style = HU_REPLY_STYLE_FLAT;
                    hu_log_info(
                        "human", agent ? agent->observer : NULL,
                        "imessage_dispatch: reply sent but not natively threaded (flat commit)");
                }
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
                    actual_style = HU_REPLY_STYLE_FLAT;
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
                    actual_style = HU_REPLY_STYLE_FLAT;
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
        log_entry.style_chosen = actual_style;
        log_entry.send_result = (int)err;
        log_entry.tier_used = tier_used;
        uint64_t pace_end = hu_time_get_current_ms();
        log_entry.elapsed_ms = (int)(pace_end - pace_start);
        (void)hu_imessage_action_log_jsonl(&log_entry);
    }

    return err;
}

/* ── Cross-channel context formatters (DDD Phase 2.5 follow-on) ──────────────
 * Build the "cross-channel awareness" context lines for proactive prompts.
 * SQLite + non-test only, matching the original daemon.c guard. */
#if defined(HU_ENABLE_SQLITE) && !defined(HU_IS_TEST)
void hu_daemon_cross_channel_format_when(char *out, size_t out_sz, const char *ts) {
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

void hu_daemon_cross_channel_platform_label(const char *plat, char *out, size_t out_sz) {
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

bool hu_daemon_cross_ctx_append_line(hu_allocator_t *alloc, char **buf, size_t *buf_len,
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
