/* Inbound hold, release side: the poll wrapper that hands held messages back
 * to the normal turn once the model is healthy, cancels the ones the owner
 * already answered, and expires the ones held too long.
 * Contract: include/human/daemon/job_hold.h. Hold side: daemon_job_hold.c.
 *
 * At most once. A held row is claimed (lease), then committed to `sending`
 * (hu_job_queue_repo_mark_sending), then copied into the poll batch, then
 * finished `done` — all inside one hu_daemon_jobs_poll call, before the turn
 * that will answer it runs. So:
 *   - crash before mark_sending: no turn saw the message; the lease expires
 *     and the row is claimed again (safe: nothing was sent);
 *   - crash after mark_sending (with or without the finish): the row is
 *     `sending` and the next start moves it to `unknown`, never re-claimed —
 *     the message may go unanswered, it is never answered twice;
 *   - a re-injected message whose turn fails again is not held again: its
 *     key hold:<chat_id>:<rowid> already exists, enqueue is INSERT OR IGNORE.
 * The daemon's reply dedup (keyed on the batch's highest rowid) still applies
 * to the re-injected batch, as a second guard. */
#include "daemon_job_hold_internal.h"
#include "human/daemon/job_hold.h"

#include "human/agent.h"
#include "human/core/log.h"
#include "human/daemon.h"
#include "human/daemon/owner_notify.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(HU_HAS_IMESSAGE) && defined(HU_ENABLE_SQLITE) && !defined(HU_IS_TEST) && \
    defined(__APPLE__) && defined(__MACH__)
#include "human/channels/imessage.h"
#define HOLD_HAVE_CHATDB 1
#endif

#define HOLD_RELEASE_EVERY_S 30  /* the probe caches 60 s; no need to ask more */
#define HOLD_RELEASE_LEASE_S 120 /* claim -> done happens within this call */
#define HOLD_RELEASE_MAX     16  /* one poll batch */

static hu_job_hold_replied_fn g_replied_stub;
static int64_t g_last_check;

void hu_job_hold_release_reset(void) {
    g_replied_stub = NULL;
    g_last_check = 0;
}

void hu_daemon_job_hold_set_replied_fn_for_test(hu_job_hold_replied_fn fn) {
    g_replied_stub = fn;
}

static bool owner_replied(void *channel_ctx, const char *chat_id, const char *handle,
                          int64_t rowid) {
    if (g_replied_stub)
        return g_replied_stub(channel_ctx, chat_id, handle, rowid);
#ifdef HOLD_HAVE_CHATDB
    return hu_imessage_channel_replied_after(channel_ctx, chat_id, handle, rowid);
#else
    (void)channel_ctx;
    (void)chat_id;
    (void)handle;
    (void)rowid;
    return false;
#endif
}

/* SHADOW: what LIVE would have done with the in-memory would-holds. */
static void release_shadow(const struct hu_service_channel *ch, const struct hu_config *config,
                           hu_observer_t *obs, int64_t now) {
    hu_job_hold_shadow_entry_t *ring = hu_job_hold_shadow_ring();
    hu_daemon_job_hold_metrics_t *m = hu_job_hold_metrics_mut();
    size_t live = 0, expired = 0;
    for (size_t i = 0; i < HU_JOB_HOLD_SHADOW_SLOTS; i++) {
        if (!ring[i].used)
            continue;
        if (now - ring[i].held_at > HU_JOB_HOLD_MAX_AGE_S) {
            ring[i].used = false;
            expired++;
        } else {
            live++;
        }
    }
    if (expired) {
        m->shadow_would_expire += expired;
        hu_log_info("jobq", obs, "[jobq] shadow would expire n=%zu", expired);
    }
    if (!live || hu_job_hold_probe(config) != HU_JOB_PROBE_UP)
        return;
    size_t release = 0, cancel = 0;
    int64_t age_max = 0;
    for (size_t i = 0; i < HU_JOB_HOLD_SHADOW_SLOTS; i++) {
        hu_job_hold_shadow_entry_t *e = &ring[i];
        if (!e->used)
            continue;
        e->used = false;
        if (owner_replied(ch->channel_ctx, e->chat_id, e->handle, e->rowid)) {
            cancel++;
            continue;
        }
        release++;
        if (now - e->held_at > age_max)
            age_max = now - e->held_at;
    }
    m->shadow_would_release += release;
    m->shadow_would_cancel += cancel;
    hu_log_info("jobq", obs, "[jobq] shadow would release n=%zu age_max=%llds", release,
                (long long)age_max);
    if (cancel)
        hu_log_info("jobq", obs, "[jobq] shadow would cancel n=%zu", cancel);
}

#ifdef HU_ENABLE_SQLITE
#include "human/memory/job_queue_repo.h"

static int cmp_rowid(const void *a, const void *b) {
    int64_t x = ((const hu_channel_loop_msg_t *)a)->message_id;
    int64_t y = ((const hu_channel_loop_msg_t *)b)->message_id;
    return (x > y) - (x < y);
}

/* Expire first, so a message held past the cap is never released. */
static void expire_live(sqlite3 *db, hu_observer_t *obs, int64_t now) {
    int64_t n = 0;
    hu_error_t err = hu_job_queue_repo_expire_older_than(db, HU_JOB_KIND_INBOUND_HOLD,
                                                         now - HU_JOB_HOLD_MAX_AGE_S, now, &n);
    if (err != HU_OK || n <= 0)
        return;
    hu_daemon_job_hold_metrics_t *m = hu_job_hold_metrics_mut();
    m->expired += (uint64_t)n;
    m->expiry_notices++;
    hu_log_warn("jobq", obs, "[jobq live] expired n=%lld (held > %d s); owner notified",
                (long long)n, HU_JOB_HOLD_MAX_AGE_S);
    /* Counts only: never who wrote or what they said. */
    char body[200];
    snprintf(body, sizeof(body),
             "h-uman held %lld message%s while the local model was down and could not answer "
             "within 3 hours. Check Messages and reply yourself.",
             (long long)n, n == 1 ? "" : "s");
    (void)hu_owner_notify_local(body);
}

/* One claimed row: cancel, fail, or commit it to `sending`, copy it into
 * *out and finish it `done`. True when *out holds a message to inject. */
static bool release_one(sqlite3 *db, const struct hu_service_channel *ch, const hu_job_t *j,
                        int64_t now, hu_channel_loop_msg_t *out, size_t *canceled) {
    hu_daemon_job_hold_metrics_t *m = hu_job_hold_metrics_mut();
    if (hu_job_hold_decode(j->payload, j->payload_len, j->contact, out) != HU_OK) {
        m->release_failures++;
        (void)hu_job_queue_repo_finish(db, j->id, HU_JOB_STATE_FAILED, j->lease_until,
                                       "undecodable payload", now);
        return false;
    }
    if (owner_replied(ch->channel_ctx, out->chat_id, out->session_key, out->message_id)) {
        (*canceled)++;
        m->canceled++;
        (void)hu_job_queue_repo_finish(db, j->id, HU_JOB_STATE_CANCELED, j->lease_until,
                                       "owner replied", now);
        return false;
    }
    /* The at-most-once fence: committed before any turn can see the message.
     * Refused (lease lost, or a transaction open on the shared connection):
     * leave the row claimed; the lease expires and a later tick retries. */
    if (hu_job_queue_repo_mark_sending(db, j->id, j->lease_until, now) != HU_OK) {
        m->release_failures++;
        return false;
    }
    /* A failed finish leaves the row `sending` -> `unknown` at the next
     * start: never re-claimed, so still at most once. */
    if (hu_job_queue_repo_finish(db, j->id, HU_JOB_STATE_DONE, j->lease_until, NULL, now) != HU_OK)
        m->release_failures++;
    m->released++;
    return true;
}

static void release_live(sqlite3 *db, const struct hu_service_channel *ch,
                         const struct hu_config *config, hu_allocator_t *alloc, hu_observer_t *obs,
                         hu_channel_loop_msg_t *msgs, size_t max_msgs, size_t *count, int64_t now) {
    expire_live(db, obs, now);
    size_t space = max_msgs > *count ? max_msgs - *count : 0;
    if (space == 0 || hu_job_hold_probe(config) != HU_JOB_PROBE_UP)
        return;
    size_t cap = space < HOLD_RELEASE_MAX ? space : HOLD_RELEASE_MAX;
    hu_job_t *jobs = (hu_job_t *)alloc->alloc(alloc->ctx, cap * sizeof(hu_job_t));
    hu_channel_loop_msg_t *held =
        (hu_channel_loop_msg_t *)alloc->alloc(alloc->ctx, cap * sizeof(hu_channel_loop_msg_t));
    size_t n = 0, nh = 0, canceled = 0;
    int64_t age_max = 0;
    if (jobs && held &&
        hu_job_queue_repo_claim_due(db, HU_JOB_KIND_INBOUND_HOLD, now, HOLD_RELEASE_LEASE_S, jobs,
                                    cap, &n) == HU_OK) {
        for (size_t i = 0; i < n; i++) {
            if (!release_one(db, ch, &jobs[i], now, &held[nh], &canceled))
                continue;
            if (now - jobs[i].created_at > age_max)
                age_max = now - jobs[i].created_at;
            nh++;
        }
    }
    if (nh > 0) {
        /* Oldest first, ahead of what this poll returned: a sender's held and
         * new messages stay consecutive, so they batch into one turn. */
        qsort(held, nh, sizeof(*held), cmp_rowid);
        memmove(msgs + nh, msgs, *count * sizeof(*msgs));
        memcpy(msgs, held, nh * sizeof(*msgs));
        *count += nh;
    }
    if (n > 0)
        hu_log_info("jobq", obs, "[jobq live] released n=%zu age_max=%llds canceled=%zu", nh,
                    (long long)age_max, canceled);
    if (jobs)
        alloc->free(alloc->ctx, jobs, cap * sizeof(hu_job_t));
    if (held)
        alloc->free(alloc->ctx, held, cap * sizeof(hu_channel_loop_msg_t));
}
#endif /* HU_ENABLE_SQLITE */

hu_error_t hu_daemon_jobs_poll(struct hu_service_channel *ch, hu_allocator_t *alloc,
                               struct hu_agent *agent, const struct hu_config *config,
                               hu_channel_loop_msg_t *msgs, size_t max_msgs, size_t *out_count) {
    hu_error_t err = ch->poll_fn(ch->channel_ctx, alloc, msgs, max_msgs, out_count);
    hu_gate_mode_t mode = hu_daemon_job_hold_mode();
    if (mode == HU_GATE_OFF || !out_count || !alloc || !hu_job_hold_is_imessage(ch))
        return err;
    int64_t now = hu_job_hold_now();
    if (g_last_check && now - g_last_check < HOLD_RELEASE_EVERY_S)
        return err;
    g_last_check = now;
    hu_observer_t *obs = agent ? agent->observer : NULL;
#ifdef HU_ENABLE_SQLITE
    sqlite3 *db = hu_job_hold_db();
    if (mode == HU_GATE_LIVE && db) {
        release_live(db, ch, config, alloc, obs, msgs, max_msgs, out_count, now);
        return err;
    }
#endif
    release_shadow(ch, config, obs, now);
    return err;
}
