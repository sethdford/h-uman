#ifndef HU_DAEMON_PERIPHERAL_GOV_H
#define HU_DAEMON_PERIPHERAL_GOV_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Peripheral-output governance, extracted from daemon.c (DDD Phase 2.4).
 *
 * Governs outbound VISUAL ATTACHMENT sends with a daily/weekly budget,
 * deliberately separate from the proactive check-in governor (gov_budget in
 * daemon/common.h) and the proactive throttle (which stays with the proactive
 * cluster in daemon_proactive.c). The budget state is a process-lifetime
 * singleton, lazily initialized on the first allow-check.
 *
 * The implementation is compiled only in non-test builds (#ifndef HU_IS_TEST),
 * matching the original guard in daemon.c; these declarations are harmless in
 * test builds because the (also-guarded) call sites never reference them there. */

/* True if the visual-attachment budget permits another send at now_ms.
 * Lazily initializes the governor on first call. */
bool hu_daemon_visual_attach_gov_allow(uint64_t now_ms);

/* Record that a visual attachment was sent at now_ms (debits the budget).
 * No-op if the governor has not been initialized via an allow-check yet. */
void hu_daemon_visual_attach_gov_after_send(uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* HU_DAEMON_PERIPHERAL_GOV_H */
