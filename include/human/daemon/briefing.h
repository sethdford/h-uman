#ifndef HU_DAEMON_BRIEFING_H
#define HU_DAEMON_BRIEFING_H

/* Morning briefing (life-admin slice 2, docs/plans/2026-09-30-life-admin/).
 *
 * One message to the owner each morning: today's calendar, reminders due
 * today, commitments due (theirs to you and yours to them, each quoting its
 * source), important dates in the next week, and the weather. A day with
 * none of the first four sends nothing; weather alone is not worth a text.
 *
 * Gate: HU_BRIEFING off | shadow | live (default off). Shadow composes the
 * briefing and writes it to <state dir>/briefings/<YYYY-MM-DD>.txt instead of
 * sending it. Going live is gated on a measurement: the owner reads and rates
 * a week of those shadow files. Only the owner is ever texted.
 *
 * Send time: HU_BRIEFING_HOUR (5..11, default 8) local. If the daemon was
 * down at that hour the briefing still goes out until noon, then the day is
 * skipped rather than sent late. At most one per local day, durably. */

#include "human/memory/reminder_repo.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HU_BRIEFING_MAX_ITEMS 8

typedef struct hu_briefing_event {
    char name[128];
    int hour; /* 0..23 local; -1 for all-day events or an unreadable time */
    int minute;
    bool all_day;
} hu_briefing_event_t;

typedef struct hu_briefing_commitment {
    char what[256];    /* the words that were said */
    char who_name[64]; /* the other person */
    bool mine;         /* the owner promised it; false: they promised the owner */
    bool overdue;      /* deadline before today */
} hu_briefing_commitment_t;

typedef struct hu_briefing_date {
    char label[128]; /* "Mom's birthday" */
    int days_away;   /* 0 = today .. 7 */
} hu_briefing_date_t;

typedef struct hu_briefing_inputs {
    hu_briefing_event_t events[HU_BRIEFING_MAX_ITEMS];
    size_t events_n;
    size_t events_more; /* today's events beyond the ones listed */
    hu_reminder_t reminders[HU_BRIEFING_MAX_ITEMS];
    size_t reminders_n;
    hu_briefing_commitment_t commitments[HU_BRIEFING_MAX_ITEMS];
    size_t commitments_n;
    hu_briefing_date_t dates[HU_BRIEFING_MAX_ITEMS];
    size_t dates_n;
    bool have_weather;
    int temp_f;
    char condition[64];
} hu_briefing_inputs_t;

/* Compose the briefing for the local day containing `now`. Pure. Returns
 * bytes written, or 0 (and out[0] = '\0') when there is nothing worth
 * sending or it would not fit in `cap`. */
size_t hu_briefing_compose(const hu_briefing_inputs_t *in, int64_t now, char *out, size_t cap);

/* Parse the calendar helper's JSON array into events, all-day ones first,
 * then by start time. Times come from the numeric "h"/"m" fields; output
 * without them falls back to the "H:MM[:SS] AM|PM" or "HH:MM[:SS]" part of
 * the "start" date string, else hour = -1. Returns false (and *out_n = 0)
 * when the JSON does not parse. */
bool hu_briefing_parse_calendar(const char *json, size_t len, hu_briefing_event_t *out, size_t cap,
                                size_t *out_n, size_t *out_more);

/* Whether local hour `hour` is inside the send window for `send_hour`. */
bool hu_briefing_in_window(int hour, int send_hour);

struct hu_agent;
struct hu_service_channel;

/* Call once per service-loop iteration; paces itself (one check a minute). */
void hu_briefing_tick(struct hu_agent *agent, struct hu_service_channel *channels,
                      size_t channel_count, int64_t now);

#endif /* HU_DAEMON_BRIEFING_H */
