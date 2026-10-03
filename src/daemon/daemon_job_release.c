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
 *     hu_daemon_jobs_on_turn_error counts it and tells the owner instead.
 * Nothing is released on a tick whose poll failed (the caller may drop that
 * batch), or when chat.db cannot say whether the owner already answered.
 * The daemon's reply dedup (keyed on the batch's highest rowid) still applies
 * to the re-injected batch, as a second guard.
 *
 * Cost: the model probe runs only when a held row is due (LIVE) or a
 * would-hold is remembered (SHADOW), under a 2 s cap and a 60 s cache. */
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

#define HOLD_RELEASE_EVERY_S 30   /* the probe caches 60 s; no need to ask more */
#define HOLD_RELEASE_LEASE_S 120  /* claim -> done happens within this call */
#define HOLD_RELEASE_MAX     16   /* one poll batch */
#define HOLD_NOTICE_REARM_S  3600 /* expiry banner: once per outage, at most hourly */

static hu_job_hold_replied_fn g_replied_stub;
static int64_t g_last_check;
static int64_t g_expiry_notice_at; /* 0 = no banner this outage */

void hu_job_hold_release_reset(void) {
    g_replied_stub = NULL;
    g_last_check = 0;
    g_expiry_notice_at = 0;
}

void hu_daemon_job_hold_set_replied_fn_for_test(hu_job_hold_replied_fn fn) {
    g_replied_stub = fn;
}

/* One chat.db read for all n messages. Non-OK: could not tell. */
static hu_error_t owner_replied(void *channel_ctx, const hu_channel_loop_msg_t *msgs, size_t n,
                                bool *out_replied) {
    if (g_replied_stub)
        return g_replied_stub(channel_ctx, msgs, n, out_replied);
#ifdef HOLD_HAVE_CHATDB
    return hu_imessage_channel_replied_after_batch(channel_ctx, msgs, n, out_replied);
#else
    (void)channel_ctx;
    (void)msgs;
    memset(out_replied, 0, n * sizeof(*out_replied));
    return HU_OK;
#endif
}

/* SHADOW: what LIVE would have done with the in-memory would-holds. */
static void release_shadow(const struct hu_service_channel *ch, const struct hu_config *config,
                           hu_allocator_t *alloc, hu_observer_t *obs, int64_t now) {
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
    if (!live || hu_job_hold_probe(config, false) != HU_JOB_PROBE_UP)
        return;
    hu_channel_loop_msg_t *q =
        (hu_channel_loop_msg_t *)alloc->alloc(alloc->ctx, live * sizeof(hu_channel_loop_msg_t));
    bool *replied = (bool *)alloc->alloc(alloc->ctx, live * sizeof(bool));
    size_t slot[HU_JOB_HOLD_SHADOW_SLOTS];
    size_t n = 0;
    for (size_t i = 0; q && replied && i < HU_JOB_HOLD_SHADOW_SLOTS && n < live; i++) {
        if (!ring[i].used)
            continue;
        memset(&q[n], 0, sizeof(q[n]));
        q[n].message_id = ring[i].rowid;
        memcpy(q[n].session_key, ring[i].handle, sizeof(ring[i].handle));
        memcpy(q[n].chat_id, ring[i].chat_id, sizeof(ring[i].chat_id));
        slot[n++] = i;
    }
    if (n == live && owner_replied(ch->channel_ctx, q, n, replied) == HU_OK) {
        size_t release = 0, cancel = 0;
        int64_t age_max = 0;
        for (size_t k = 0; k < n; k++) {
            hu_job_hold_shadow_entry_t *e = &ring[slot[k]];
            e->used = false;
            if (replied[k]) {
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
    } else {
        hu_log_info("jobq", obs, "[jobq] shadow would keep n=%zu held (chat.db unavailable)", live);
    }
    if (q)
        alloc->free(alloc->ctx, q, live * sizeof(hu_channel_loop_msg_t));
    if (replied)
        alloc->free(alloc->ctx, replied, live * sizeof(bool));
}

#ifdef HU_ENABLE_SQLITE
#include "human/memory/job_queue_repo.h"

static int cmp_rowid(const void *a, const void *b) {
    int64_t x = ((const hu_channel_loop_msg_t *)a)->message_id;
    int64_t y = ((const hu_channel_loop_msg_t *)b)->message_id;
    return (x > y) - (x < y);
}

/* Expire first, so a message held past the cap is never released. One banner
 * per outage (re-armed by a release, or after an hour): holds expire on
 * staggered ticks, and a banner per tick would be one per message. */
static void expire_live(sqlite3 *db, hu_observer_t *obs, int64_t now) {
    int64_t n = 0;
    hu_error_t err = hu_job_queue_repo_expire_older_than(db, HU_JOB_KIND_INBOUND_HOLD,
                                                         now - HU_JOB_HOLD_MAX_AGE_S, now, &n);
    if (err != HU_OK || n <= 0)
        return;
    hu_daemon_job_hold_metrics_t *m = hu_job_hold_metrics_mut();
    m->expired += (uint64_t)n;
    bool notify = g_expiry_notice_at == 0 || now - g_expiry_notice_at >= HOLD_NOTICE_REARM_S;
    hu_log_warn("jobq", obs, "[jobq live] expired n=%lld (held > %d s)%s", (long long)n,
                HU_JOB_HOLD_MAX_AGE_S, notify ? "; owner notified" : "");
    if (!notify)
        return;
    g_expiry_notice_at = now;
    m->expiry_notices++;
    /* Counts only: never who wrote or what they said. */
    char body[200];
    snprintf(body, sizeof(body),
             "h-uman held %lld message%s while the local model was down and could not answer "
             "within 3 hours. Check Messages and reply yourself.",
             (long long)n, n == 1 ? "" : "s");
    (void)hu_owner_notify_local(body);
}

/* chat.db unreadable: give every claimed row back (no attempt counted). */
static void defer_all(sqlite3 *db, hu_observer_t *obs, const hu_job_t *jobs, size_t n,
                      int64_t now) {
    for (size_t i = 0; i < n; i++)
        (void)hu_job_queue_repo_release(db, jobs[i].id, jobs[i].lease_until, 0, now);
    hu_job_hold_metrics_mut()->release_deferred++;
    hu_log_warn("jobq", obs, "[jobq live] chat.db unavailable; n=%zu stay held", n);
}

/* One decoded row the owner has not answered: commit it to `sending`, then
 * `done`. True when the message may be handed to a turn. */
static bool commit_release(sqlite3 *db, const hu_job_t *j, int64_t now) {
    hu_daemon_job_hold_metrics_t *m = hu_job_hold_metrics_mut();
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

typedef struct release_buf {
    hu_job_t *jobs;
    hu_channel_loop_msg_t *held;
    bool *replied;
    size_t cap;
} release_buf_t;

static void release_buf_free(hu_allocator_t *alloc, release_buf_t *b) {
    if (b->jobs)
        alloc->free(alloc->ctx, b->jobs, b->cap * sizeof(hu_job_t));
    if (b->held)
        alloc->free(alloc->ctx, b->held, b->cap * sizeof(hu_channel_loop_msg_t));
    if (b->replied)
        alloc->free(alloc->ctx, b->replied, b->cap * sizeof(bool));
}

/* Claim, decode, ask chat.db once, then cancel or commit each row. Returns
 * how many messages b->held[0..) now holds for the turn. */
static size_t claim_and_check(sqlite3 *db, const struct hu_service_channel *ch, hu_observer_t *obs,
                              release_buf_t *b, int64_t now, int64_t *age_max, size_t *canceled,
                              size_t *claimed) {
    size_t n = 0, nd = 0, nh = 0;
    if (hu_job_queue_repo_claim_due(db, HU_JOB_KIND_INBOUND_HOLD, now, HOLD_RELEASE_LEASE_S,
                                    b->jobs, b->cap, &n) != HU_OK)
        return 0;
    *claimed = n;
    hu_daemon_job_hold_metrics_t *m = hu_job_hold_metrics_mut();
    for (size_t i = 0; i < n; i++) { /* decode; compact b->jobs to the decodable */
        if (hu_job_hold_decode(b->jobs[i].payload, b->jobs[i].payload_len, b->jobs[i].contact,
                               &b->held[nd]) != HU_OK) {
            m->release_failures++;
            (void)hu_job_queue_repo_finish(db, b->jobs[i].id, HU_JOB_STATE_FAILED,
                                           b->jobs[i].lease_until, "undecodable payload", now);
            continue;
        }
        if (nd != i)
            b->jobs[nd] = b->jobs[i];
        nd++;
    }
    if (nd == 0)
        return 0;
    if (owner_replied(ch->channel_ctx, b->held, nd, b->replied) != HU_OK) {
        defer_all(db, obs, b->jobs, nd, now);
        return 0;
    }
    for (size_t i = 0; i < nd; i++) {
        const hu_job_t *j = &b->jobs[i];
        if (b->replied[i]) {
            (*canceled)++;
            m->canceled++;
            (void)hu_job_queue_repo_finish(db, j->id, HU_JOB_STATE_CANCELED, j->lease_until,
                                           "owner replied", now);
            continue;
        }
        if (!commit_release(db, j, now))
            continue;
        if (now - j->created_at > *age_max)
            *age_max = now - j->created_at;
        if (nh != i)
            b->held[nh] = b->held[i];
        hu_job_hold_note_released(b->held[nh].message_id);
        nh++;
    }
    return nh;
}

static void release_live(sqlite3 *db, const struct hu_service_channel *ch,
                         const struct hu_config *config, hu_allocator_t *alloc, hu_observer_t *obs,
                         hu_channel_loop_msg_t *msgs, size_t max_msgs, size_t *count, int64_t now) {
    expire_live(db, obs, now);
    size_t space = max_msgs > *count ? max_msgs - *count : 0;
    int64_t due = 0;
    /* Probe only when there is something to release: a hung server costs
     * nothing on the (usual) ticks with an empty queue. */
    if (space == 0 ||
        hu_job_queue_repo_count_due(db, HU_JOB_KIND_INBOUND_HOLD, now, &due) != HU_OK || due == 0 ||
        hu_job_hold_probe(config, false) != HU_JOB_PROBE_UP)
        return;
    release_buf_t b = {.cap = space < HOLD_RELEASE_MAX ? space : HOLD_RELEASE_MAX};
    b.jobs = (hu_job_t *)alloc->alloc(alloc->ctx, b.cap * sizeof(hu_job_t));
    b.held =
        (hu_channel_loop_msg_t *)alloc->alloc(alloc->ctx, b.cap * sizeof(hu_channel_loop_msg_t));
    b.replied = (bool *)alloc->alloc(alloc->ctx, b.cap * sizeof(bool));
    size_t nh = 0, canceled = 0, claimed = 0;
    int64_t age_max = 0;
    if (b.jobs && b.held && b.replied)
        nh = claim_and_check(db, ch, obs, &b, now, &age_max, &canceled, &claimed);
    if (nh > 0) {
        /* Oldest first, ahead of what this poll returned: a sender's held and
         * new messages stay consecutive, so they batch into one turn. */
        qsort(b.held, nh, sizeof(*b.held), cmp_rowid);
        memmove(msgs + nh, msgs, *count * sizeof(*msgs));
        memcpy(msgs, b.held, nh * sizeof(*msgs));
        *count += nh;
        g_expiry_notice_at = 0; /* the outage is over: re-arm the expiry banner */
    }
    if (claimed > 0)
        hu_log_info("jobq", obs, "[jobq live] released n=%zu age_max=%llds canceled=%zu", nh,
                    (long long)age_max, canceled);
    release_buf_free(alloc, &b);
}
#endif /* HU_ENABLE_SQLITE */

hu_error_t hu_daemon_jobs_poll(struct hu_service_channel *ch, hu_allocator_t *alloc,
                               struct hu_agent *agent, const struct hu_config *config,
                               hu_channel_loop_msg_t *msgs, size_t max_msgs, size_t *out_count) {
    hu_error_t err = ch->poll_fn(ch->channel_ctx, alloc, msgs, max_msgs, out_count);
    hu_gate_mode_t mode = hu_daemon_job_hold_mode();
    /* A failed poll may be dropped by the caller: never put released (already
     * `done`) messages into it. */
    if (mode == HU_GATE_OFF || err != HU_OK || !out_count || !alloc || !hu_job_hold_is_imessage(ch))
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
    release_shadow(ch, config, alloc, obs, now);
    return err;
}
