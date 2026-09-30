#ifndef HU_DAEMON_REMINDERS_H
#define HU_DAEMON_REMINDERS_H

/* Owner reminders (life-admin slice 1, docs/plans/2026-09-30-life-admin/).
 *
 * The owner texts the daemon from their own number ("remind me to call mom
 * at 5"), the daemon files it and answers with a one-line ack, and at the due
 * time sends the reminder back to the same thread. Gate: HU_REMINDERS
 * off | shadow | live (default off). Shadow parses the owner's messages and
 * logs what it would have saved; nothing is stored, acked or sent.
 *
 * Going live is gated on a measurement, not on these tests: a week of shadow
 * log on the owner's real messages with no chat misread as a command (a
 * false positive swallows a message the model should have answered). Only
 * the owner is ever texted, so the contact-facing blind A/B does not apply.
 *
 * Parsing is deterministic on purpose: a reminder that fires at the wrong
 * time is worse than none, and every phrasing below is pinned by
 * tests/test_reminders_parse.c. */

#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum hu_reminder_cmd_kind {
    HU_REMINDER_CMD_NONE = 0,    /* not a reminder command */
    HU_REMINDER_CMD_ADD,         /* "remind me to X at 5pm" — what + when */
    HU_REMINDER_CMD_ADD_NO_TIME, /* "remind me to X" — ask when */
    HU_REMINDER_CMD_LIST,        /* "what are my reminders" */
    HU_REMINDER_CMD_DONE,        /* "done" — only meaningful right after a delivery */
    HU_REMINDER_CMD_SNOOZE,      /* "snooze", "snooze 30m" — same */
    HU_REMINDER_CMD_WHEN,        /* "tomorrow at 9" — answers an ADD_NO_TIME */
} hu_reminder_cmd_kind_t;

/* When, in the owner's local calendar. Resolved to an instant by
 * hu_reminder_resolve so daylight-saving changes are handled by the C
 * library rather than by adding a fixed offset. */
typedef struct hu_reminder_when {
    int64_t relative_s;   /* > 0: "in 20 minutes" — now + relative_s, nothing else used */
    int day_offset;       /* 0 = today, 1 = tomorrow, ... (ignored when weekday >= 0) */
    int weekday;          /* 0 = Sunday .. 6; -1 = not a weekday phrase */
    int hour;             /* 0..23; -1 = not given */
    int minute;           /* 0..59 */
    bool time_is_default; /* no clock time was said; hour came from a default */
    bool half_unknown;    /* "at 7" with no am/pm and no morning/evening word:
                           * resolved to the next future of 7:00 and 19:00 */
} hu_reminder_when_t;

#define HU_REMINDER_WHAT_MAX 256

typedef struct hu_reminder_cmd {
    hu_reminder_cmd_kind_t kind;
    char what[HU_REMINDER_WHAT_MAX]; /* the task, as the owner said it */
    hu_reminder_when_t when;         /* ADD, WHEN, and a SNOOZE that named a time */
    int64_t snooze_s;                /* SNOOZE without a time (default 15 minutes) */
} hu_reminder_cmd_t;

/* Parse one owner message. Returns true (and fills *out) for any reminder
 * command, false (kind NONE) otherwise. Pure: no clock, no I/O. */
bool hu_reminder_parse(const char *text, size_t len, hu_reminder_cmd_t *out);

/* The instant `when` names, relative to `now` in the process's local time
 * zone. A clock time today that has already passed rolls to tomorrow; a
 * weekday that is today but already past rolls to next week. Returns 0 when
 * `when` is unusable. */
int64_t hu_reminder_resolve(const hu_reminder_when_t *when, int64_t now);

/* "Fri 5:00 PM", "tomorrow 9:00 AM", "today 5:00 PM" — how an ack or a list
 * names a due time, relative to `now`. Returns bytes written (0 on error). */
size_t hu_reminder_format_due(int64_t due, int64_t now, char *buf, size_t cap);

/* ── Daemon side (live only under HU_REMINDERS) ─────────────────────────── */

struct hu_agent;
struct hu_service_channel;

/* An owner message that is a reminder command: act on it and write the reply
 * to `reply`. Returns true when the message was consumed (the caller sends
 * `reply` and skips the normal turn); false for anything else, including
 * every message while the gate is off or shadow. `owner` is the sender
 * handle, `channel_name` the channel it arrived on. */
bool hu_reminders_handle_owner_message(struct hu_agent *agent, const char *owner, size_t owner_len,
                                       const char *channel_name, const char *text, size_t len,
                                       int64_t now, char *reply, size_t reply_cap);

/* Deliver reminders that are due. Call once per service-loop iteration; it
 * does its own pacing (at most one pass per 20 seconds). A reminder is marked
 * sent only when the channel accepted it; one more than two hours late is
 * marked missed instead of sent. */
void hu_reminders_tick(struct hu_agent *agent, struct hu_service_channel *channels,
                       size_t channel_count, int64_t now);

#endif /* HU_DAEMON_REMINDERS_H */
