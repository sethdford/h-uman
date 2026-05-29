/* daemon_identity.c — Per-contact trust state.
 *
 * Phase 2 DDD bounded-context split: this single-responsibility translation
 * unit owns the thread-safe, LRU-evicted per-contact trust table that used to
 * live inline in daemon.c. The public contract (hu_daemon_get_trust_state /
 * _set_trust_state / _trust_count / _trust_reset) is declared in
 * human/daemon.h; this file is the only definition site. Pure structural move
 * — behavior is identical to the prior inline implementation.
 */

#include "human/core/error.h"
#include "human/daemon.h"             /* hu_daemon_contact_trust_t + public decls */
#include "human/intelligence/trust.h" /* hu_trust_init, hu_trust_state_t */

#include <pthread.h>
#include <string.h>

/* TRUST-006: Per-contact trust state tracking
 * Expanded from 256 to 4096 with LRU eviction and mutex for thread safety. */
#define HU_DAEMON_TRUST_CAP 4096

static hu_daemon_contact_trust_t g_contact_trust[HU_DAEMON_TRUST_CAP];
static size_t g_contact_trust_count;

#if !defined(_WIN32) && !defined(__CYGWIN__)
static pthread_mutex_t g_trust_mutex = PTHREAD_MUTEX_INITIALIZER;
#define TRUST_LOCK()   pthread_mutex_lock(&g_trust_mutex)
#define TRUST_UNLOCK() pthread_mutex_unlock(&g_trust_mutex)
#else
#define TRUST_LOCK()   ((void)0)
#define TRUST_UNLOCK() ((void)0)
#endif

static hu_error_t trust_find_or_create_slot(const char *contact_id, size_t cid_len,
                                            size_t *slot_out) {
    if (!contact_id || cid_len == 0 || !slot_out)
        return HU_ERR_INVALID_ARGUMENT;

    for (size_t i = 0; i < g_contact_trust_count; i++) {
        if (strlen(g_contact_trust[i].contact_id) == cid_len &&
            memcmp(g_contact_trust[i].contact_id, contact_id, cid_len) == 0) {
            *slot_out = i;
            return HU_OK;
        }
    }

    size_t slot;
    if (g_contact_trust_count < HU_DAEMON_TRUST_CAP) {
        slot = g_contact_trust_count++;
    } else {
        slot = 0;
        int64_t oldest = g_contact_trust[0].state.last_updated_at;
        for (size_t i = 1; i < g_contact_trust_count; i++) {
            if (g_contact_trust[i].state.last_updated_at < oldest) {
                oldest = g_contact_trust[i].state.last_updated_at;
                slot = i;
            }
        }
    }

    size_t copy_len = cid_len;
    if (copy_len >= sizeof(g_contact_trust[slot].contact_id))
        copy_len = sizeof(g_contact_trust[slot].contact_id) - 1;
    memcpy(g_contact_trust[slot].contact_id, contact_id, copy_len);
    g_contact_trust[slot].contact_id[copy_len] = '\0';
    hu_trust_init(&g_contact_trust[slot].state);
    *slot_out = slot;
    return HU_OK;
}

hu_error_t hu_daemon_get_trust_state(const char *contact_id, size_t cid_len,
                                     hu_trust_state_t *out) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    TRUST_LOCK();
    size_t slot;
    hu_error_t err = trust_find_or_create_slot(contact_id, cid_len, &slot);
    if (err == HU_OK)
        *out = g_contact_trust[slot].state;
    TRUST_UNLOCK();
    return err;
}

hu_error_t hu_daemon_set_trust_state(const char *contact_id, size_t cid_len,
                                     const hu_trust_state_t *state) {
    if (!state)
        return HU_ERR_INVALID_ARGUMENT;
    TRUST_LOCK();
    size_t slot;
    hu_error_t err = trust_find_or_create_slot(contact_id, cid_len, &slot);
    if (err == HU_OK)
        g_contact_trust[slot].state = *state;
    TRUST_UNLOCK();
    return err;
}

#ifdef HU_IS_TEST
size_t hu_daemon_trust_count(void) {
    return g_contact_trust_count;
}
void hu_daemon_trust_reset(void) {
    TRUST_LOCK();
    g_contact_trust_count = 0;
    memset(g_contact_trust, 0, sizeof(g_contact_trust));
    TRUST_UNLOCK();
}
#endif
