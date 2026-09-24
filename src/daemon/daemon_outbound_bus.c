/* src/daemon/daemon_outbound_bus.c — outbound bus bridge for the service loop.
 *
 * Carved verbatim out of src/daemon.c (E2, 2026-09-20). See the header for the
 * contract. The bridge was HU_IS_TEST-gated inside daemon.c only because it was
 * static there; here it compiles in every build so tests can pin the UTF-8
 * clamp, the chunk/final routing and the iMessage deferral without a daemon. */
#include "human/daemon_outbound_bus.h"

#include "human/agent/output_validator_chain.h"
#include "human/agent/validators/builtin.h"
#include "human/channel.h"
#include "human/channels/channel_embed.h"
#include "human/context/conversation.h"
#include "human/observability/validator_telemetry.h"

#include <string.h>

size_t hu_daemon_outbound_utf8_safe_truncate(const char *buf, size_t len) {
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
        /* A complete trailing sequence is kept whole; an incomplete one loses
         * its lead byte too. Before 2026-09-20 the complete case returned the
         * index just past the lead, leaving a dangling lead byte (invalid
         * UTF-8) whenever the clamp landed exactly on a character end. */
        pos = (pos - 1 + seq_len <= len) ? pos - 1 + seq_len : pos - 1;
    }
    return pos;
}

void hu_daemon_outbound_bus_set_message(hu_bus_event_t *bev, const char *data, size_t len) {
    if (!data || len == 0) {
        bev->message[0] = '\0';
        return;
    }
    size_t copy_len = len < HU_BUS_MSG_LEN - 1 ? len : HU_BUS_MSG_LEN - 1;
    if (copy_len < len)
        copy_len = hu_daemon_outbound_utf8_safe_truncate(data, copy_len);
    memcpy(bev->message, data, copy_len);
    bev->message[copy_len] = '\0';
}

hu_service_channel_t *hu_daemon_outbound_find_channel(hu_service_channel_t *channels, size_t count,
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
void hu_daemon_outbound_stream_event_cb(const hu_agent_stream_event_t *event, void *ctx) {
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
        hu_daemon_outbound_bus_set_message(&ev, event->data, event->data_len);
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
        hu_daemon_outbound_bus_set_message(&ev, event->data, event->data_len);
        break;
    case HU_AGENT_STREAM_TOOL_START:
        ev.type = HU_BUS_TOOL_CALL;
        hu_daemon_outbound_bus_set_message(&ev, event->tool_name, event->tool_name_len);
        break;
    case HU_AGENT_STREAM_TOOL_ARGS:
        ev.type = HU_BUS_TOOL_CALL;
        hu_daemon_outbound_bus_set_message(&ev, event->data, event->data_len);
        break;
    case HU_AGENT_STREAM_TOOL_RESULT:
        ev.type = HU_BUS_TOOL_CALL_RESULT;
        hu_daemon_outbound_bus_set_message(&ev, event->data, event->data_len);
        break;
    }
    hu_bus_publish(sc->bus, &ev);
}

bool hu_daemon_outbound_bus_cb(hu_bus_event_type_t type, const hu_bus_event_t *ev, void *user_ctx) {
    hu_daemon_out_bus_bridge_t *br = (hu_daemon_out_bus_bridge_t *)user_ctx;
    if (!br || !ev)
        return true;
    if (type != HU_BUS_MESSAGE_CHUNK && type != HU_BUS_MESSAGE_SENT)
        return true;

    hu_service_channel_t *sch =
        hu_daemon_outbound_find_channel(br->channels, br->channel_count, ev->channel);
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

    /* iMessage owns its final delivery in the post-turn action-surface
     * dispatcher (threaded reply via Cmd-R / Show-Menu, with a documented
     * flat fallback). iMessage exposes no send_event, so a generic bus
     * delivery here would fall back to a FLAT vtable->send — stripping all
     * threading intent — and then set text_delivered_via_bus=true, which
     * makes the post-turn guard skip the dispatcher entirely. Every reply
     * would land as a free-floating new message instead of a native thread
     * reply. Defer: return without sending so text_delivered_via_bus stays
     * false and the dispatcher region owns delivery. */
    if (sch->channel->vtable->name) {
        const char *cn = sch->channel->vtable->name(sch->channel->ctx);
        if (cn && strcmp(cn, "imessage") == 0)
            return true;
    }

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
