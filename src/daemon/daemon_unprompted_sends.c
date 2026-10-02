/* src/daemon/daemon_unprompted_sends.c — F25 check-in, photo share and the
 * scheduled-queue delivery, carved from src/daemon.c (file-size ratchet) and
 * routed through the one unprompted gate stack (DEF-14, 2026-10-02). Before
 * this, all three skipped opt-out and the governor, and the photo share had
 * no guard at all. Contract: include/human/daemon/unprompted_sends.h. */
#include "human/agent/outbound_sanitize.h"
#include "human/agent/proactive.h"
#include "human/agent/validators/builtin.h"
#include "human/contact_send_recency.h"
#include "human/context/conversation.h"
#include "human/core/log.h"
#include "human/core/log_redact.h"
#include "human/core/paths.h"
#include "human/daemon/unprompted_gate.h"
#include "human/daemon/unprompted_sends.h"
#include "human/daemon_learning_tick.h"
#include "human/memory/emotional_moments.h"
#include "human/visual/content.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Log tag only (#xxxx): no handle, no display name (docs/guides/log-privacy.md). */
#define who_of(cp) HU_LOG_WHO_CSTR((cp)->contact_id)

/* One due moment → at most one send. Returns after the first channel whose
 * name matches the contact's route, as the inline loop did. */
static void f25_send_for_contact(hu_allocator_t *alloc, hu_agent_t *agent,
                                 hu_service_channel_t *channels, size_t channel_count,
                                 hu_proactive_context_t *pctx, const hu_contact_profile_t *cp,
                                 const hu_emotional_moment_t *m, int64_t now) {
    char ch_buf[64] = {0};
    char target_route_buf[128] = {0};
    hu_daemon_proactive_parse_route(cp, ch_buf, target_route_buf);
    size_t target_len = strlen(target_route_buf);
    hu_daemon_proactive_apply_route(pctx, cp->contact_id, (time_t)now, channels, channel_count,
                                    ch_buf, target_route_buf, &target_len);
    const char *target_part = target_route_buf;

    for (size_t c = 0; c < channel_count; c++) {
        hu_channel_t *ch = channels[c].channel;
        if (!ch || !ch->vtable || !ch->vtable->name)
            continue;
        const char *ch_name = ch->vtable->name(ch->ctx);
        if (!ch_name || strcmp(ch_name, ch_buf) != 0)
            continue;
        if (!ch->vtable->send)
            return;

        /* Outbound safety gate — see 2026-05-16 incident: m->topic can hold a
         * raw window of the user's own confession or the "(last: %lld)"
         * recall-format string. Drop the send, log without the body. */
        size_t topic_len = strnlen(m->topic, sizeof(m->topic));
        if (!hu_proactive_topic_is_safe(m->topic, topic_len)) {
            hu_log_info("human", agent->observer,
                        "F25 emotional check-in BLOCKED for %s (unsafe topic, %zu chars)",
                        who_of(cp), topic_len);
            (void)hu_emotional_moment_mark_followed_up(agent->memory, m->id);
            return;
        }
        /* FU-1: defer F25 if a reactive turn fired for this contact recently. */
        if (hu_daemon_proactive_should_defer(&agent->contact_send_recency, m->contact_id,
                                             strlen(m->contact_id), now)) {
            hu_log_info("human", agent->observer,
                        "F25 emotional check-in deferred for %s (reactive turn within %ds)",
                        who_of(cp), HU_DAEMON_REACTIVE_GATE_WINDOW_S);
            return;
        }

        char msg_buf[384];
        int w = snprintf(msg_buf, sizeof(msg_buf), "hey how are you doing with %s?", m->topic);
        if (w <= 0 || (size_t)w >= sizeof(msg_buf))
            return;
        size_t msg_len = (size_t)w;
        hu_unprompted_gate_t g;
        hu_daemon_unprompted_gate_init(&g, alloc, agent, ch_name, target_part, target_len, now);
        if (hu_unprompted_send_check(&g, cp->contact_id, HU_UNPROMPTED_F25, now, msg_buf, &msg_len,
                                     /*at_send=*/true) != HU_UNPROMPTED_ALLOW)
            return; /* the moment stays due; the stack logged why */
        if (ch->vtable->send(ch->ctx, target_part, target_len, msg_buf, msg_len, NULL, 0) ==
            HU_OK) {
            (void)hu_emotional_moment_mark_followed_up(agent->memory, m->id);
            hu_contact_send_recency_record(&agent->contact_send_recency, m->contact_id,
                                           strlen(m->contact_id), now, HU_SEND_PATH_PROACTIVE);
            (void)hu_daemon_proactive_outcome_record_send(agent->memory, ch_name, target_part,
                                                          target_len);
            hu_unprompted_record_sent(&g, cp->contact_id, HU_UNPROMPTED_F25, now);
            hu_log_info("human", agent->observer, "F25 emotional check-in sent to %s: %s",
                        who_of(cp), HU_LOG_TEXT_CSTR(msg_buf, 120));
        }
        return;
    }
}

void hu_daemon_f25_checkins_tick(hu_allocator_t *alloc, hu_agent_t *agent,
                                 hu_service_channel_t *channels, size_t channel_count,
                                 hu_proactive_context_t *pctx, int64_t now) {
    if (!alloc || !agent || !agent->persona || !agent->memory || !channels)
        return;
    hu_emotional_moment_t *due = NULL;
    size_t due_count = 0;
    if (hu_emotional_moment_get_due(alloc, agent->memory, now, &due, &due_count) != HU_OK || !due)
        return;
    for (size_t d = 0; d < due_count; d++) {
        const hu_emotional_moment_t *m = &due[d];
        for (size_t i = 0; i < agent->persona->contacts_count; i++) {
            const hu_contact_profile_t *cp = &agent->persona->contacts[i];
            if (!cp->proactive_checkin || !cp->proactive_channel || !cp->contact_id)
                continue;
            /* Strict contact_id equality only (2026-05-16 cross-contact
             * routing incident; pinned by tests/test_proactive.c). */
            if (!hu_proactive_contact_matches_moment(cp->contact_id, m->contact_id))
                continue;
            f25_send_for_contact(alloc, agent, channels, channel_count, pctx, cp, m, now);
            break;
        }
    }
    alloc->free(alloc->ctx, due, due_count * sizeof(hu_emotional_moment_t));
}

bool hu_daemon_photo_share_send(hu_allocator_t *alloc, hu_agent_t *agent, hu_channel_t *channel,
                                const hu_contact_profile_t *cp, const char *ch_name,
                                const char *target, size_t target_len, const char *const *media,
                                size_t media_count, int64_t now) {
    if (!agent || !channel || !channel->vtable || !channel->vtable->send || !cp ||
        !cp->contact_id || !media || media_count == 0)
        return false;
    /* FU-1: defer the album if a reactive turn fired recently. */
    if (hu_daemon_proactive_should_defer(&agent->contact_send_recency, cp->contact_id,
                                         strlen(cp->contact_id), now)) {
        hu_log_info("human", agent->observer,
                    "proactive photo album deferred for %s (reactive turn within %ds)", who_of(cp),
                    HU_DAEMON_REACTIVE_GATE_WINDOW_S);
        return false;
    }
    hu_unprompted_gate_t g;
    hu_daemon_unprompted_gate_init(&g, alloc, agent, ch_name, target, target_len, now);
    if (hu_unprompted_send_check(&g, cp->contact_id, HU_UNPROMPTED_PHOTO, now, NULL, NULL,
                                 /*at_send=*/true) != HU_UNPROMPTED_ALLOW)
        return false;
    hu_error_t err = HU_ERR_IO;
    if ((err = channel->vtable->send(channel->ctx, target, target_len, "", 0, media,
                                     media_count)) != HU_OK) {
        hu_log_warn("human", agent->observer, "proactive photo album FAILED (err=%d)", (int)err);
        return false;
    }
    hu_contact_send_recency_record(&agent->contact_send_recency, cp->contact_id,
                                   strlen(cp->contact_id), now, HU_SEND_PATH_PHOTO);
    hu_unprompted_record_sent(&g, cp->contact_id, HU_UNPROMPTED_PHOTO, now);
    hu_log_info("human", agent->observer, "proactive photo album: %zu photos shared", media_count);
    return true;
}

void hu_daemon_photo_share_tick(hu_allocator_t *alloc, hu_agent_t *agent, hu_channel_t *channel,
                                const hu_contact_profile_t *cp, const char *target,
                                size_t target_len, const char *combined, size_t combined_len,
                                int64_t now) {
#if defined(HU_ENABLE_SQLITE) && !defined(HU_IS_TEST)
    if (!channel || !channel->vtable || !channel->vtable->send || !channel->vtable->name ||
        combined_len == 0)
        return;
    static uint64_t last_photo_scan_ms;
    uint64_t pnow_ms = (uint64_t)now * 1000ULL;
    if (pnow_ms - last_photo_scan_ms <= 3600000) /* max once per hour */
        return;
    last_photo_scan_ms = pnow_ms;
    char photos_db[512];
    if (hu_visual_apple_photos_db_path(photos_db, sizeof(photos_db)) == 0)
        return;
    hu_visual_entry_t *photos = NULL;
    size_t photo_count = 0;
    if (hu_visual_scan_apple_photos(alloc, photos_db, 3, &photos, &photo_count, 5) != HU_OK ||
        !photos || photo_count == 0)
        return;
    /* Top N shareable photos (album mode: up to 3). */
    size_t cand_idx[3];
    double cand_conf[3];
    size_t cand_count = 0;
    for (size_t pi = 0; pi < photo_count; pi++) {
        bool should_share = false;
        double conf = 0.0;
        hu_visual_should_share(&photos[pi], combined, combined_len, &should_share, &conf);
        if (!should_share || conf < 0.3)
            continue;
        if (cand_count < 3) {
            cand_idx[cand_count] = pi;
            cand_conf[cand_count++] = conf;
            continue;
        }
        size_t worst = 0;
        for (size_t ci = 1; ci < 3; ci++)
            if (cand_conf[ci] < cand_conf[worst])
                worst = ci;
        if (conf > cand_conf[worst]) {
            cand_idx[worst] = pi;
            cand_conf[worst] = conf;
        }
    }
    const char *media[3];
    size_t media_count = 0;
    for (size_t ci = 0; ci < cand_count; ci++)
        if (photos[cand_idx[ci]].path[0])
            media[media_count++] = photos[cand_idx[ci]].path;
    if (media_count > 0)
        (void)hu_daemon_photo_share_send(alloc, agent, channel, cp,
                                         channel->vtable->name(channel->ctx), target, target_len,
                                         media, media_count, now);
    hu_visual_entries_free(alloc, photos, photo_count);
#else
    (void)alloc;
    (void)agent;
    (void)channel;
    (void)cp;
    (void)target;
    (void)target_len;
    (void)combined;
    (void)combined_len;
    (void)now;
#endif
}

bool hu_daemon_sched_deliver(hu_allocator_t *alloc, hu_agent_t *agent, hu_channel_t *channel,
                             const char *ch_name, const char *contact, char *msg, size_t msg_len,
                             size_t msg_cap, uint8_t kind, int64_t now) {
    if (!channel || !contact || !msg)
        return false;
    /* FU-1: defer scheduled delivery if the reactive turn fired recently. */
    if (agent && hu_daemon_proactive_should_defer(&agent->contact_send_recency, contact,
                                                  strlen(contact), now)) {
        hu_log_info("human", agent->observer,
                    "scheduled message deferred for %s (reactive turn within %ds)",
                    HU_LOG_WHO_CSTR(contact), HU_DAEMON_REACTIVE_GATE_WINDOW_S);
        return false;
    }
    hu_validator_chain_apply_default_in_place(alloc, agent ? agent->observer : NULL, NULL, 0,
                                              "scheduled send", msg, &msg_len, msg_cap);
    if (msg_len == 0)
        return false;
    msg_len = hu_conversation_vary_complexity(msg, msg_len, (uint32_t)now);
    if (msg_len > 1 && msg[0] >= 'A' && msg[0] <= 'Z' && msg[1] >= 'a' && msg[1] <= 'z' &&
        msg[0] != 'I')
        msg[0] = (char)(msg[0] + 32);
    if (msg_len > 1 && msg[msg_len - 1] == '.') {
        msg[msg_len - 1] = '\0';
        msg_len--;
    }
    hu_unprompted_gate_t g;
    memset(&g, 0, sizeof(g));
    if (kind != HU_UNPROMPTED_NONE) {
        /* Unprompted (the bump): the whole stack, sanitizer and moderation
         * included, at the moment of sending — consent and caps may have
         * changed since it was scheduled. */
        hu_daemon_unprompted_gate_init(&g, alloc, agent, ch_name, contact, strlen(contact), now);
        if (hu_unprompted_send_check(&g, contact, (hu_unprompted_kind_t)kind, now, msg, &msg_len,
                                     /*at_send=*/true) != HU_UNPROMPTED_ALLOW)
            return false;
    } else {
        /* Sprint 59 outbound safety — owner-scheduled sends get the same
         * sanitizer as proactive (cross-contact bleed, metadata leak). */
        size_t san_len = msg_len;
        const char *san_reason = NULL;
        if (!hu_outbound_sanitize(msg, &san_len, &san_reason)) {
            hu_log_warn("human", agent ? agent->observer : NULL,
                        "scheduled send to %s REJECTED by outbound pipeline: %s "
                        "(would have sent: %s)",
                        HU_LOG_WHO_CSTR(contact), san_reason ? san_reason : "unknown",
                        HU_LOG_TEXT(msg, msg_len, 80));
            return false;
        }
        msg_len = san_len;
    }
    bool sent = hu_daemon_sched_send_and_log(agent, channel, ch_name, contact, msg, msg_len);
    if (sent && kind != HU_UNPROMPTED_NONE)
        hu_unprompted_record_sent(&g, contact, (hu_unprompted_kind_t)kind, now);
    return true;
}

/* Persist scheduled.json after a slot changes. A failed save leaves memory and
 * disk disagreeing and the stale file replays on restart (the 2026-07-27
 * sched-send incident class), so the failure is logged rather than dropped. */
static void sched_persist(hu_agent_t *agent, const char *what) {
    char sp[512];
    int sn = hu_paths_state(sp, sizeof(sp), "scheduled.json");
    if (sn <= 0 || (size_t)sn >= sizeof(sp))
        return;
    hu_error_t se = hu_conversation_sched_save(sp, (size_t)sn);
    if (se != HU_OK)
        hu_log_error("human", agent ? agent->observer : NULL,
                     "scheduled.json not persisted after %s (%d)", what, (int)se);
}

void hu_daemon_sched_deliver_due(hu_allocator_t *alloc, hu_agent_t *agent,
                                 hu_service_channel_t *channels, size_t channel_count,
                                 int64_t now) {
    if (!channels)
        return;
    /* Re-sync from disk when the file changed — `human schedule add` writes
     * from a separate process (2026-07-27). One stat() per pass. */
    char sp[512];
    int sn = hu_paths_state(sp, sizeof(sp), "scheduled.json");
    if (sn > 0 && (size_t)sn < sizeof(sp))
        hu_conversation_sched_reload_if_changed(sp, (size_t)sn);
    uint64_t now_ms = (uint64_t)now * 1000ULL;
    for (size_t sc = 0; sc < channel_count; sc++) {
        hu_channel_t *ch = channels[sc].channel;
        if (!ch || !ch->vtable || !ch->vtable->send || !ch->vtable->name)
            continue;
        const char *ch_name = ch->vtable->name(ch->ctx);
        if (!ch_name)
            continue;
        char contact[128], channel[32], msg[512];
        uint8_t kind = 0; /* non-zero = unprompted (the bump): full gate stack */
        size_t len = hu_conversation_flush_scheduled_kind(now_ms, ch_name, strlen(ch_name), contact,
                                                          sizeof(contact), channel, sizeof(channel),
                                                          msg, sizeof(msg), &kind);
        if (len > 0 && hu_daemon_sched_deliver(alloc, agent, ch, ch_name, contact, msg, len,
                                               sizeof(msg), kind, now))
            sched_persist(agent, "send");
    }
}
