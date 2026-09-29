/* Send-provenance observer registry. See
 * include/human/channels/imessage_send_observer.h for the contract. */
#include "human/channels/imessage_send_observer.h"

static hu_imessage_send_observer_fn g_observer = NULL;
static void *g_observer_user = NULL;

void hu_imessage_send_observer_set(hu_imessage_send_observer_fn fn, void *user) {
    g_observer = fn;
    g_observer_user = fn ? user : NULL;
}

bool hu_imessage_send_observer_active(void) {
    return g_observer != NULL;
}

void hu_imessage_send_observer_notify(const hu_imessage_sent_event_t *ev) {
    if (!g_observer || !ev || !ev->handle || ev->handle_len == 0)
        return;
    g_observer(g_observer_user, ev);
}
