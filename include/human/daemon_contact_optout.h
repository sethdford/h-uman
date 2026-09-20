#ifndef HU_DAEMON_CONTACT_OPTOUT_H
#define HU_DAEMON_CONTACT_OPTOUT_H
/*
 * Contestability for proactive contact (October roadmap O5, 2026-09-20).
 *
 * Recipients had no channel to decline being texted first. Now an inbound
 * message that asks us to stop ("stop texting me", "leave me alone",
 * "unsubscribe", ...) writes a per-contact suppression row, and the proactive
 * candidate loop skips that contact from the next tick — honoured within one
 * turn, no LLM in the loop, no gate to promote.
 *
 * HU_CONTACT_OPTOUT: unset or anything but "off" ⇒ honoured. Consent is the
 * default, not a feature to roll out; "off" exists only as an escape hatch
 * and is logged as such by the doctor.
 *
 * Reactive replies are untouched: replying to the message that said "stop"
 * is the acknowledgement. Only initiative is suppressed.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct hu_agent;
struct sqlite3;

/* Pure, testable without a daemon: does this text ask us to stop initiating
 * contact? Whole-phrase, case-insensitive (hu_str_contains_word_ci rules —
 * "unsubscribed" does not match "unsubscribe"); a negation immediately before
 * the phrase ("don't stop texting me", "never stop texting me") is NOT an
 * opt-out. Never matches the empty string. */
bool hu_contact_optout_detect(const char *text, size_t len);

/* HU_CONTACT_OPTOUT != "off". */
bool hu_contact_optout_enabled(void);

#ifdef HU_ENABLE_SQLITE
/* DB-level halves (the agent wrappers below are thin): pinned by tests
 * against an in-memory store with the pre/post contract
 * not-suppressed → observe(opt-out text) → suppressed → should_skip. */
bool hu_contact_optout_observe_db(struct sqlite3 *db, const char *contact, size_t contact_len,
                                  const char *text, size_t len, int64_t now);
bool hu_contact_optout_is_suppressed_db(struct sqlite3 *db, const char *contact);
#endif

/* Reactive path: record a suppression when `text` opts out. Returns true
 * only when a row was written (so callers can log once). */
bool hu_daemon_contact_optout_observe(struct hu_agent *agent, const char *contact,
                                      size_t contact_len, const char *text, size_t len);

/* Proactive candidate loop: true ⇒ do not run the proposer for this contact.
 * Like the reachability pre-filter, never writes a proactive_decisions row —
 * a suppressed contact is not a candidate, so it must not inflate FIR. */
bool hu_daemon_contact_optout_should_skip(struct hu_agent *agent, const char *contact);

#endif /* HU_DAEMON_CONTACT_OPTOUT_H */
