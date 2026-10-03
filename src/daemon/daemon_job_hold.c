/* Inbound hold while the model is down: the HOLD/ONE_OFF decision and the
 * daemon's failed-turn hook. Contract: include/human/daemon/job_hold.h.
 * The release side (poll wrapper) is daemon_job_release.c. */
#include "daemon_job_hold_internal.h"
#include "human/daemon/job_hold.h"

#include "human/agent.h"
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/log.h"
#include "human/core/log_redact.h"
#include "human/core/time.h"
#include "human/daemon.h"
#include "human/ml/mlx_admin.h"
#include <stdio.h>
#include <string.h>

/* Single daemon thread: written by the start hook and the poll loop only. */
static hu_daemon_job_hold_metrics_t g_metrics;
static hu_job_hold_shadow_entry_t g_shadow[HU_JOB_HOLD_SHADOW_SLOTS];
static size_t g_shadow_next;
static int64_t g_test_now;
#ifdef HU_ENABLE_SQLITE
static sqlite3 *g_db;
#endif

hu_job_turn_verdict_t hu_job_hold_decide(hu_error_t turn_err, hu_job_probe_t probe) {
    return hu_agent_error_is_transport(turn_err) && probe == HU_JOB_PROBE_DOWN
               ? HU_JOB_TURN_HOLD
               : HU_JOB_TURN_ONE_OFF;
}

/* Payload v1, little-endian, no NULs:
 *   u8 version | u8 flags | i64 message_id | i64 timestamp_sec |
 *   u8 guid_len | u8 reply_to_len | u8 chat_id_len | u16 content_len |
 *   guid | reply_to_guid | chat_id | content
 * flags: 1 is_group, 2 has_attachment, 4 has_video, 8 was_edited, 16 was_unsent. */
#define HOLD_PAYLOAD_VERSION 1
#define HOLD_HEADER_LEN      23

static void put_i64(unsigned char *p, int64_t v) {
    uint64_t u = (uint64_t)v;
    for (int i = 0; i < 8; i++)
        p[i] = (unsigned char)(u >> (8 * i));
}

hu_error_t hu_job_hold_encode(const hu_channel_loop_msg_t *m, unsigned char *buf, size_t cap,
                              size_t *out_len) {
    if (!m || !buf || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    size_t gl = strnlen(m->guid, sizeof(m->guid) - 1);
    size_t rl = strnlen(m->reply_to_guid, sizeof(m->reply_to_guid) - 1);
    size_t cl = strnlen(m->chat_id, sizeof(m->chat_id) - 1);
    size_t tl = strnlen(m->content, sizeof(m->content) - 1);
    size_t total = HOLD_HEADER_LEN + gl + rl + cl + tl;
    if (total > cap)
        return HU_ERR_INVALID_ARGUMENT;
    unsigned char flags = (unsigned char)((m->is_group ? 1 : 0) | (m->has_attachment ? 2 : 0) |
                                          (m->has_video ? 4 : 0) | (m->was_edited ? 8 : 0) |
                                          (m->was_unsent ? 16 : 0));
    buf[0] = HOLD_PAYLOAD_VERSION;
    buf[1] = flags;
    put_i64(buf + 2, m->message_id);
    put_i64(buf + 10, m->timestamp_sec);
    buf[18] = (unsigned char)gl;
    buf[19] = (unsigned char)rl;
    buf[20] = (unsigned char)cl;
    buf[21] = (unsigned char)(tl & 0xff);
    buf[22] = (unsigned char)(tl >> 8);
    unsigned char *p = buf + HOLD_HEADER_LEN;
    memcpy(p, m->guid, gl);
    p += gl;
    memcpy(p, m->reply_to_guid, rl);
    p += rl;
    memcpy(p, m->chat_id, cl);
    p += cl;
    memcpy(p, m->content, tl);
    *out_len = total;
    return HU_OK;
}

hu_gate_mode_t hu_daemon_job_hold_mode(void) {
    return g_metrics.mode;
}

void hu_daemon_job_hold_metrics(hu_daemon_job_hold_metrics_t *out) {
    if (out)
        *out = g_metrics;
}

void hu_daemon_job_hold_reset_for_test(void) {
    memset(&g_metrics, 0, sizeof(g_metrics));
    memset(g_shadow, 0, sizeof(g_shadow));
    g_shadow_next = 0;
    g_test_now = 0;
#ifdef HU_ENABLE_SQLITE
    g_db = NULL;
#endif
}

void hu_daemon_job_hold_set_now_for_test(int64_t now) {
    g_test_now = now;
}

int64_t hu_job_hold_now(void) {
    return g_test_now ? g_test_now : hu_time_wall_ms() / 1000;
}

hu_job_probe_t hu_job_hold_probe(const struct hu_config *config) {
    const char *url = config ? hu_config_get_provider_base_url(config, "mlx_local") : NULL;
    if (!url || !url[0])
        return HU_JOB_PROBE_UNKNOWN;
    hu_allocator_t alloc = hu_system_allocator();
    return hu_mlx_admin_probe_health(&alloc, url, strlen(url)) ? HU_JOB_PROBE_UP
                                                               : HU_JOB_PROBE_DOWN;
}

bool hu_job_hold_is_imessage(const struct hu_service_channel *ch) {
    if (!ch || !ch->channel || !ch->channel->vtable || !ch->channel->vtable->name)
        return false;
    const char *name = ch->channel->vtable->name(ch->channel->ctx);
    return name && strcmp(name, HU_JOB_HOLD_CHANNEL) == 0;
}

/* SHADOW: remember the batch (dedup by rowid; a full ring drops the oldest). */
static void shadow_remember(const hu_channel_loop_msg_t *msgs, size_t bs, size_t be, int64_t now) {
    for (size_t i = bs; i <= be; i++) {
        const hu_channel_loop_msg_t *m = &msgs[i];
        bool seen = false;
        for (size_t s = 0; s < HU_JOB_HOLD_SHADOW_SLOTS && !seen; s++)
            seen = g_shadow[s].used && g_shadow[s].rowid == m->message_id;
        if (seen || m->message_id <= 0)
            continue;
        hu_job_hold_shadow_entry_t *e = &g_shadow[g_shadow_next++ % HU_JOB_HOLD_SHADOW_SLOTS];
        memset(e, 0, sizeof(*e));
        e->used = true;
        e->rowid = m->message_id;
        e->held_at = now;
        memcpy(e->handle, m->session_key, sizeof(e->handle) - 1);
        memcpy(e->chat_id, m->chat_id, sizeof(e->chat_id) - 1);
    }
}

#ifdef HU_ENABLE_SQLITE
#include "human/memory/job_queue_repo.h"

void hu_daemon_job_hold_configure(hu_gate_mode_t mode, sqlite3 *db) {
    g_metrics.mode = mode;
    g_db = mode == HU_GATE_LIVE ? db : NULL;
    if (mode == HU_GATE_LIVE && !db)
        g_metrics.mode = HU_GATE_SHADOW;
}

/* LIVE: one inbound_hold job per message, key hold:<chat_id>:<rowid>. */
static void hold_live(hu_observer_t *obs, const hu_channel_loop_msg_t *msgs, size_t bs, size_t be,
                      hu_error_t err, int64_t now) {
    size_t held = 0, dup = 0, too_big = 0, skipped = 0;
    unsigned char payload[HU_JOB_PAYLOAD_MAX];
    for (size_t i = bs; i <= be; i++) {
        const hu_channel_loop_msg_t *m = &msgs[i];
        size_t len = 0;
        char key[HU_JOB_KEY_MAX];
        int kn = snprintf(key, sizeof(key), "hold:%s:%lld", m->chat_id, (long long)m->message_id);
        if (m->message_id <= 0 || kn <= 0 || (size_t)kn >= sizeof(key)) {
            skipped++;
            continue;
        }
        if (hu_job_hold_encode(m, payload, sizeof(payload), &len) != HU_OK) {
            too_big++;
            continue;
        }
        hu_job_spec_t spec = {
            .kind = HU_JOB_KIND_INBOUND_HOLD,
            .payload = payload,
            .payload_len = len,
            .contact = m->session_key,
            .channel = HU_JOB_HOLD_CHANNEL,
            .due_at = now,
            .idempotency_key = key,
        };
        bool inserted = false;
        if (hu_job_queue_repo_enqueue(g_db, &spec, now, NULL, &inserted) != HU_OK)
            skipped++;
        else if (inserted)
            held++;
        else
            dup++; /* already held (or already re-injected once): never twice */
    }
    g_metrics.held += held;
    g_metrics.hold_duplicates += dup;
    g_metrics.hold_skipped += too_big + skipped;
    /* Counts and the error enum only: never text, handles or keys. */
    hu_log_info("jobq", obs, "[jobq live] held n=%zu dup=%zu skipped=%zu err=%d probe=down", held,
                dup, skipped, (int)err);
    if (too_big)
        hu_log_warn("jobq", obs,
                    "[jobq live] not held: %zu message(s) over the %d-byte payload cap; they get "
                    "today's behaviour (no reply)",
                    too_big, HU_JOB_PAYLOAD_MAX);
}
#endif /* HU_ENABLE_SQLITE */

void hu_daemon_jobs_on_turn_error(struct hu_agent *agent, const struct hu_config *config,
                                  const struct hu_service_channel *ch,
                                  const hu_channel_loop_msg_t *msgs, size_t batch_start,
                                  size_t batch_end, hu_error_t err) {
    hu_observer_t *obs = agent ? agent->observer : NULL;
    const char *who = msgs ? msgs[batch_start].session_key : "";
    /* Today's line, byte for byte (it was inline in hu_service_run). */
    hu_log_error("human", obs, "agent turn failed for %s: %s", HU_LOG_WHO(who, strlen(who)),
                 hu_error_string(err));
    if (g_metrics.mode == HU_GATE_OFF || !msgs || batch_end < batch_start ||
        !hu_agent_error_is_transport(err) || !hu_job_hold_is_imessage(ch))
        return;
    hu_job_probe_t probe = hu_job_hold_probe(config);
    if (hu_job_hold_decide(err, probe) != HU_JOB_TURN_HOLD) {
        g_metrics.transport_one_offs++;
        hu_log_info("jobq", obs, "[jobq] one-off err=%d probe=%s", (int)err,
                    probe == HU_JOB_PROBE_UP ? "up" : "unknown");
        return;
    }
    size_t n = batch_end - batch_start + 1;
    int64_t now = hu_job_hold_now();
#ifdef HU_ENABLE_SQLITE
    if (g_metrics.mode == HU_GATE_LIVE && g_db) {
        hold_live(obs, msgs, batch_start, batch_end, err, now);
        return;
    }
#endif
    g_metrics.shadow_would_hold += n;
    shadow_remember(msgs, batch_start, batch_end, now);
    hu_log_info("jobq", obs, "[jobq] shadow would hold n=%zu err=%d probe=down", n, (int)err);
}
