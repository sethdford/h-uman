#ifndef HU_MEMORY_REMINDER_REPO_H
#define HU_MEMORY_REMINDER_REPO_H

/* Durable owner reminders (life-admin slice 1).
 *
 * Delivery is claim-based so a reminder is sent at most once per claim:
 * hu_reminder_repo_claim_due moves due rows pending -> sending inside one
 * transaction, the caller sends, then marks each row sent (or back to
 * pending if the channel refused). A row left in `sending` by a crash is
 * returned to pending after 10 minutes, so a reminder can repeat after a
 * crash mid-send but is never silently lost. Rows more than `late_after_s`
 * past due are marked missed instead of claimed. */

#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

#define HU_REMINDER_OWNER_MAX   128
#define HU_REMINDER_CHANNEL_MAX 32
#define HU_REMINDER_TEXT_MAX    256

typedef struct hu_reminder {
    int64_t id;
    char owner[HU_REMINDER_OWNER_MAX];     /* handle the reminder is delivered to */
    char channel[HU_REMINDER_CHANNEL_MAX]; /* channel it was asked on */
    char what[HU_REMINDER_TEXT_MAX];
    int64_t due_at;
    int64_t updated_at;
} hu_reminder_t;

#ifdef HU_ENABLE_SQLITE

#include <sqlite3.h>

hu_error_t hu_reminder_repo_ensure_schema(sqlite3 *db);

hu_error_t hu_reminder_repo_add(sqlite3 *db, const char *owner, const char *channel,
                                const char *what, int64_t due_at, int64_t now, const char *source,
                                int64_t *out_id);

/* Claim up to `cap` due reminders (due_at <= now), oldest first. First marks
 * pending rows more than `late_after_s` overdue as missed (*out_missed counts
 * them) and returns stale `sending` rows to pending. */
hu_error_t hu_reminder_repo_claim_due(sqlite3 *db, int64_t now, int64_t late_after_s,
                                      hu_reminder_t *out, size_t cap, size_t *out_n,
                                      size_t *out_missed);

/* status: "sent", "pending" (the send failed; retry), "done", or
 * "missed_told" (a missed reminder the owner has now been told about). */
hu_error_t hu_reminder_repo_mark(sqlite3 *db, int64_t id, const char *status, int64_t now);

/* Back to pending with a new due time. */
hu_error_t hu_reminder_repo_snooze(sqlite3 *db, int64_t id, int64_t new_due, int64_t now);

/* The reminder most recently delivered to `owner` at or after `since`.
 * HU_ERR_NOT_FOUND when there is none. */
hu_error_t hu_reminder_repo_last_sent(sqlite3 *db, const char *owner, int64_t since,
                                      hu_reminder_t *out);

/* Pending reminders for `owner`, soonest first. */
hu_error_t hu_reminder_repo_upcoming(sqlite3 *db, const char *owner, hu_reminder_t *out, size_t cap,
                                     size_t *out_n);

/* Missed reminders the owner has not been told about yet, oldest first. */
hu_error_t hu_reminder_repo_missed(sqlite3 *db, hu_reminder_t *out, size_t cap, size_t *out_n);

#endif /* HU_ENABLE_SQLITE */

#endif /* HU_MEMORY_REMINDER_REPO_H */
