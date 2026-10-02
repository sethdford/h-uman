#ifndef HU_DAEMON_GRIEF_DECAY_H
#define HU_DAEMON_GRIEF_DECAY_H
/*
 * Time-decayed emotional-tone gate for proactive check-ins (DEF-10).
 *
 * Before: when the contact's LAST inbound classified heavy or grief
 * (hu_proactive_should_suppress_for_emotion), every check-in to them was
 * suppressed, with no time limit. One sad message silenced a contact for as
 * long as it stayed their last message: 153 suppressions in 13 days of prod
 * logs, all one contact, about 12 a day. A person checks in MORE after a hard
 * moment, gently, after a short pause.
 *
 * Gate: HU_GRIEF_DECAY=off|shadow|live (default OFF).
 *   off    — byte-identical to before: heavy/grief last inbound suppresses.
 *   shadow — same decision; one line per suppression logs whether the quiet
 *            window has passed:
 *              [grief_decay shadow] heavy=1 age_h=<h> quiet_h=<h> would_allow=<0|1>
 *   live   — suppression only inside the quiet window after the heavy
 *            message; after it the check-in is ELIGIBLE again and still goes
 *            through every other gate (24 h since last contact, governor,
 *            throttle, reachability, opt-out, the proposer).
 *
 * The window: $HU_GRIEF_DECAY_QUIET_HOURS, default
 * HU_GRIEF_DECAY_DEFAULT_QUIET_HOURS (24 h, "the next day"). No measurement
 * of Seth's own follow-up latency after a heavy message exists yet (the
 * emotion card measures his reply to distress, not the follow-up), so this is
 * a documented default.
 * TODO(learned-behaviour v2): read followup_delay_p50[heavy] from the v2
 * profile (static-rules inventory §5.2, follow-up group) per contact, clamped
 * to [1 h, 14 d], and fall back to this default only for new contacts.
 *
 * A timestamp that does not parse fails closed: the old suppression applies.
 * Logs carry the age, window and decision only — never text or contact ids.
 */
#include "human/core/gate_mode.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HU_GRIEF_DECAY_DEFAULT_QUIET_HOURS 24.0
#define HU_GRIEF_DECAY_MIN_QUIET_HOURS     1.0
#define HU_GRIEF_DECAY_MAX_QUIET_HOURS     336.0 /* 14 days */

/* The contact's most recent inbound message and when it arrived. */
typedef struct hu_grief_decay_inbound {
    char text[1024];
    size_t len;
    char ts[32]; /* "%Y-%m-%d %H:%M" local, as hu_channel_history_entry_t */
} hu_grief_decay_inbound_t;

/* Remember text (cut to 1023 bytes) and ts as the most recent inbound. */
void hu_grief_decay_note_inbound(hu_grief_decay_inbound_t *li, const char *text, size_t len,
                                 const char *ts);

/* $HU_GRIEF_DECAY=off|shadow|live, default OFF. */
hu_gate_mode_t hu_grief_decay_mode(void);

/* $HU_GRIEF_DECAY_QUIET_HOURS clamped to [MIN, MAX]; unparsable -> default. */
double hu_grief_decay_quiet_hours(void);

/* "%Y-%m-%d %H:%M" (local time) -> epoch seconds; -1 when it does not parse. */
int64_t hu_grief_decay_parse_ts(const char *ts);

/* Pure: true while `now` is inside the quiet window after heavy_ts (or when
 * heavy_ts < 0: unknown time fails closed to quiet). */
bool hu_grief_decay_in_quiet_window(int64_t heavy_ts, int64_t now, double quiet_hours);

/* The check-in gate at daemon.c's P6-3 site: true = suppress this check-in.
 * Not heavy/grief -> false in every mode. */
bool hu_grief_decay_suppress_checkin(const hu_grief_decay_inbound_t *li, int64_t now);

#endif /* HU_DAEMON_GRIEF_DECAY_H */
