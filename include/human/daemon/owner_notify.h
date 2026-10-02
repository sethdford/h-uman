#ifndef HU_DAEMON_OWNER_NOTIFY_H
#define HU_DAEMON_OWNER_NOTIFY_H

/*
 * Tell the owner something locally — a Notification Center banner on macOS.
 * Shared by the hurt-signal hand-off and the undelivered-send alert, so both
 * go through one osascript invocation that passes the body as a run-handler
 * argument (a contact name or handle can never be interpreted as script).
 *
 * The body must never quote a message: name who/what, not what was said.
 * Test builds record the request instead of spawning anything.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Best-effort; returns false when the banner could not be shown (logged). */
bool hu_owner_notify_local(const char *body);

#ifdef HU_IS_TEST
unsigned hu_owner_notify_test_count(void);
const char *hu_owner_notify_test_last_body(void);
void hu_owner_notify_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* HU_DAEMON_OWNER_NOTIFY_H */
