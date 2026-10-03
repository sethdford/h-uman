#ifndef HU_DAEMON_CRON_H
#define HU_DAEMON_CRON_H

#include "core/allocator.h"
#include "core/error.h"
#include <stdbool.h>
#include <stddef.h>

/**
 * Internal cron helpers extracted from daemon.c.
 * Public API (hu_cron_schedule_matches, hu_service_run_agent_cron) is in daemon.h.
 */

/* Match a single cron atom (e.g. "5", "1-10", "star/5", "1-10/3") against a value. */
bool hu_cron_atom_matches(const char *atom, size_t len, int value);

/* Match a cron field (comma-separated atoms, or "*") against a value. */
bool hu_cron_field_matches(const char *field, int value);

/* Run system crontab tick: load crontab, execute matching jobs.
 * Called once per minute from the service loop. */
void hu_daemon_cron_tick(hu_allocator_t *alloc);

#include "core/gate_mode.h"

/* Proactive check-ins: daily agent jobs registered at startup as "proactive:<name>"
 * that write to a contact. Gate HU_PROACTIVE_CHECKINS = off | shadow | live (default
 * off): off skips the job, shadow writes and logs the message without sending it, live
 * sends it. Live is gated on the owner reviewing shadow check-ins. */
bool hu_cron_job_is_proactive_checkin(const char *job_name);

hu_gate_mode_t hu_proactive_checkin_mode(void);

/* Builds the "channel:handle" target. A persona contact's proactive channel may already
 * name the handle ("imessage:+15550001111"); then it is used as-is. Otherwise
 * contact_id is appended. Returns snprintf-shaped length; -1 with buf[0]=0 on bad input
 * or truncation. */
int hu_proactive_checkin_target(char *buf, size_t cap, const char *channel, const char *contact_id);

#endif /* HU_DAEMON_CRON_H */
