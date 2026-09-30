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

#endif /* HU_DAEMON_PERSON_DATES_H */
