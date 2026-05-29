/* Peripheral-output governance — DDD Phase 2.4 extraction from daemon.c.
 *
 * Owns the outbound visual-attachment send budget. This is a pure structural
 * move: the logic, the budget config, and the lazy-init behavior are unchanged
 * from the original daemon.c definitions. Compiled only in non-test builds,
 * matching the original `#ifndef HU_IS_TEST` guard. */
#ifndef HU_IS_TEST

#include "human/agent/governor.h"
#include "human/daemon/peripheral_gov.h"

/* Proactive-style budget for outbound visual attachments (separate from check-in governor). */
static hu_proactive_budget_t hu_daemon_visual_attach_gov;
static bool hu_daemon_visual_attach_gov_init;

bool hu_daemon_visual_attach_gov_allow(uint64_t now_ms) {
    if (!hu_daemon_visual_attach_gov_init) {
        hu_proactive_budget_config_t cfg = {
            .daily_max = 4,
            .weekly_max = 14,
            .relationship_multiplier = 1.0,
            .cool_off_after_unanswered = 255,
            .cool_off_hours = 0,
        };
        hu_governor_init(&cfg, &hu_daemon_visual_attach_gov);
        hu_daemon_visual_attach_gov_init = true;
    }
    return hu_governor_has_budget(&hu_daemon_visual_attach_gov, now_ms);
}

void hu_daemon_visual_attach_gov_after_send(uint64_t now_ms) {
    if (hu_daemon_visual_attach_gov_init)
        (void)hu_governor_record_sent(&hu_daemon_visual_attach_gov, now_ms);
}

#endif /* !HU_IS_TEST */
