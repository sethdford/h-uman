#ifndef HU_DAEMON_PERSON_DATES_H
#define HU_DAEMON_PERSON_DATES_H

/* Birthdays and anniversaries that belong to a person (life-admin slice 4).
 *
 * Before this, the only dated source was the persona's important_dates,
 * which name no person (and are empty in the owner's live persona), so the
 * briefing could not say whose birthday it was and nothing could draft a
 * message to them. Two sources now fill that in:
 *   - the owner, from their own number: "mom's birthday is march 3",
 *     "remember Mindy's anniversary is 6/12", "our anniversary is june 12";
 *   - macOS Contacts birthdays, matched to persona contacts by phone number.
 * An owner-given date wins over Contacts for the same person.
 *
 * Gate: HU_DATES off | shadow | live (default off). Shadow parses and logs;
 * nothing is stored and Contacts is not read. Nothing here messages anyone
 * but the owner. */

#include "human/daemon/briefing.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct hu_person_date_cmd {
    char who[64];   /* as written: "mom", "Mindy", "my", "our" */
    char label[32]; /* "birthday" or "anniversary" */
    int month, day;
} hu_person_date_cmd_t;

/* Parse "<who>'s <birthday|bday|anniversary> is <date>" (optionally after
 * "remember", "fyi", "note", "btw"). Dates: "march 3", "mar 3rd", "3 march",
 * "3rd of march", "3/3", "03-03", with or without a year. Pure. */
bool hu_person_date_parse(const char *text, size_t len, hu_person_date_cmd_t *out);

/* Days from the local date of `now` to the next month/day (0 = today). A
 * Feb 29 date falls on Feb 28 in other years. -1 for an invalid date. */
int hu_person_date_days_away(int month, int day, int64_t now);

struct hu_agent;

/* Owner command: store the date and write a one-line ack. Returns true when
 * consumed (caller sends `reply`, skips the model). */
bool hu_person_dates_handle_owner_message(struct hu_agent *agent, const char *owner,
                                          size_t owner_len, const char *text, size_t len,
                                          int64_t now, char *reply, size_t reply_cap);

/* Person dates within `window_days` of now, soonest first, labelled with the
 * person ("Betty's birthday", "your anniversary"). Empty unless HU_DATES is
 * live. Contacts is read from $HOME/Library/Application Support/AddressBook,
 * or HU_ADDRESSBOOK_DIR when set (tests). */
size_t hu_person_dates_upcoming(struct hu_agent *agent, int64_t now, int window_days,
                                hu_briefing_date_t *out, size_t cap);

/* ── Drafts on the day (slice 4, second half) ─────────────────────────────
 *
 * Gate: HU_DATE_NUDGES off | shadow | live (default off). Live, between 9am
 * and 8pm local, the owner gets one question at a time about a person date
 * that is today:
 *   today is Betty's birthday. want me to text Betty: "happy birthday mom!"?
 *   reply send, skip, or send: your own words
 * Only "send", "send it", "skip" and "send: <text>" answer it, and only while
 * a question is open; nothing reaches the contact without one of them. The
 * question is recorded only after it was delivered, so "send" can never
 * approve a draft the owner did not see. Approved text goes through the
 * outbound sanitizer on the contact's channel; a failure is told to the
 * owner. Shadow logs what would be asked and sends nothing. */

/* "happy birthday mom!" — parents by relationship, others by first name. */
size_t hu_date_draft_text(const char *name, const char *relationship, const char *label, char *buf,
                          size_t cap);

/* True when HU_DATE_NUDGES is live and `contact_id` has a person date
 * (owner-given or a Contacts birthday) today: a date note is due, so the
 * unprompted gate skips that contact's routine check-ins for the day. */
bool hu_person_date_note_due_today(struct hu_agent *agent, const char *contact_id, int64_t now);

struct hu_service_channel;

void hu_date_nudges_tick(struct hu_agent *agent, struct hu_service_channel *channels,
                         size_t channel_count, int64_t now);

#endif /* HU_DAEMON_PERSON_DATES_H */
