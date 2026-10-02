/* Undelivered-send recorder. Contract: include/human/daemon/send_failure.h. */
#include "human/daemon/send_failure.h"

#ifdef HU_ENABLE_SQLITE

#include "human/core/log.h"
#include "human/daemon/owner_notify.h"
#include "human/memory/outbound_sends_repo.h"
#include "human/memory/proactive_decisions_repo.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/* A split reply fails bubble by bubble, seconds apart: one banner per
 * contact per window, every failure still recorded. */
#define HU_SEND_FAILURE_NOTIFY_WINDOW_S 600
#define HU_SEND_FAILURE_NOTIFY_SLOTS    16

static atomic_uint_fast64_t s_total;
static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
    char handle[64];
    int64_t at;
} s_notified[HU_SEND_FAILURE_NOTIFY_SLOTS];

uint64_t hu_daemon_send_failure_total(void) {
    return (uint64_t)atomic_load(&s_total);
}

#ifdef HU_IS_TEST
void hu_daemon_send_failure_test_reset(void) {
    pthread_mutex_lock(&s_mu);
    memset(s_notified, 0, sizeof(s_notified));
    pthread_mutex_unlock(&s_mu);
}
#endif

/* True when this contact has not been notified within the window; claims the
 * slot (oldest slot is reused). */
static bool claim_notify(const char *handle, int64_t now) {
    bool claim = true;
    pthread_mutex_lock(&s_mu);
    size_t slot = 0;
    for (size_t i = 0; i < HU_SEND_FAILURE_NOTIFY_SLOTS; i++) {
        if (strcmp(s_notified[i].handle, handle) == 0) {
            slot = i;
            claim = now - s_notified[i].at >= HU_SEND_FAILURE_NOTIFY_WINDOW_S;
            goto done;
        }
        if (s_notified[i].at < s_notified[slot].at)
            slot = i;
    }
    snprintf(s_notified[slot].handle, sizeof(s_notified[slot].handle), "%s", handle);
done:
    if (claim)
        s_notified[slot].at = now;
    pthread_mutex_unlock(&s_mu);
    return claim;
}

void hu_daemon_send_failure_record(sqlite3 *db, const hu_imessage_send_failed_event_t *ev,
                                   int64_t now) {
    if (!ev || !ev->handle || ev->handle_len == 0)
        return;
    char contact[64];
    size_t hl = ev->handle_len < sizeof(contact) - 1 ? ev->handle_len : sizeof(contact) - 1;
    memcpy(contact, ev->handle, hl);
    contact[hl] = '\0';
    const char *svc = ev->chat_service ? ev->chat_service : "unknown";

    uint64_t total = (uint64_t)atomic_fetch_add(&s_total, 1) + 1;
    /* Aggregate only: service and a count, never the text or the handle. */
    hu_log_warn("imessage", NULL,
                "[send] UNDELIVERED: every path failed (chat_service=%s, %zu B); recorded as "
                "send_failed, owner notified once per 10 min (failures this process: %llu)",
                svc, ev->text_len, (unsigned long long)total);

    if (db) {
        /* message_ref is a bounded prefix, the same as a proactive send's. */
        char ref[65];
        size_t rl = 0;
        if (ev->text && ev->text_len > 0) {
            rl = ev->text_len < sizeof(ref) - 1 ? ev->text_len : sizeof(ref) - 1;
            memcpy(ref, ev->text, rl);
        }
        ref[rl] = '\0';
        hu_error_t err = hu_proactive_decisions_repo_record(
            db, now, contact, HU_PROACTIVE_TRIGGER_OUTBOUND_SEND, HU_PROACTIVE_DECISION_SEND,
            "send_failed", 0, rl ? ref : NULL);
        if (err != HU_OK)
            hu_log_warn("imessage", NULL, "send_failed row not written (err=%d)", (int)err);
    }

    if (claim_notify(contact, now)) {
        /* Names who (last four characters) and the service; never the text. */
        const char *tail = hl > 4 ? contact + hl - 4 : contact;
        char body[200];
        snprintf(body, sizeof(body),
                 "h-uman could not deliver a message to ...%s (%s). Nothing went out - text "
                 "them yourself.",
                 tail, svc);
        (void)hu_owner_notify_local(body);
    }
}

bool hu_daemon_send_failure_last_undelivered(sqlite3 *db, const char *contact, int64_t *out_ts) {
    if (!db || !contact || !out_ts)
        return false;
    int64_t fail_ts = 0;
    bool have_fail = false;
    if (hu_proactive_decisions_repo_last_send_failure_ts(db, contact, &fail_ts, &have_fail) !=
            HU_OK ||
        !have_fail)
        return false;
    int64_t sent_ms = 0;
    bool have_sent = false;
    if (hu_outbound_sends_repo_last_sent_ms(db, contact, &sent_ms, &have_sent) != HU_OK)
        return false;
    if (have_sent && sent_ms / 1000 >= fail_ts)
        return false;
    *out_ts = fail_ts;
    return true;
}

#endif /* HU_ENABLE_SQLITE */
