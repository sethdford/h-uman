#ifndef HU_DAEMON_SHARE_QUEUE_H
#define HU_DAEMON_SHARE_QUEUE_H

/* Saved shares — "saw this and thought of you" (spec
 * docs/superpowers/specs/2026-09-28-expressive-imessage-design.md, Phase 5.4).
 * Seth texts a link to his own number with "save <link>" or "for <name>
 * <link>"; the daemon keeps it in a small TSV queue and offers it to the
 * director the next time that person (or, if untagged, anyone) writes. */

#include "human/core/error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct hu_persona;

typedef struct {
    char url[512];
    char for_name[64]; /* "" = untagged */
} hu_share_capture_t;

/* A save command: starts with "save" or "for <name>" and carries exactly one
 * http(s) link. A bare link or a sentence with a link is conversation. */
bool hu_share_capture_parse(const char *text, size_t len, hu_share_capture_t *out);

/* One of Seth's own handles (persona relationship "test"). */
bool hu_share_is_owner(const struct hu_persona *p, const char *handle, size_t len);

/* A name as Seth would say it ("mindy", "mom") -> that contact's handle. By
 * first name or relationship; an ambiguous match resolves to nothing. */
bool hu_share_resolve_contact(const struct hu_persona *p, const char *name, char *handle_out,
                              size_t cap);

/* Queue file ops (path explicit; production uses <state>/share_queue.tsv). */
hu_error_t hu_share_queue_add(const char *path, int64_t ts, const char *url,
                              const char *for_handle);
/* The oldest unsent link tagged for handle, else the oldest untagged one. */
bool hu_share_queue_next(const char *path, const char *handle, char *url_out, size_t cap);
hu_error_t hu_share_queue_mark_sent(const char *path, const char *url);

struct hu_channel;

/* Seth saving a link from one of his own numbers: file it and write a short
 * ack ("saved for Mindy 👍"). False — and nothing filed — for anyone else or
 * for anything that is not a save command. */
bool hu_share_capture_handle(const struct hu_persona *p, const char *sender, size_t sender_len,
                             const char *text, size_t len, const char *queue_path, int64_t now,
                             char *ack, size_t ack_cap);

/* Send this contact's next saved link as its own bubble and mark it sent. */
bool hu_share_send_saved(struct hu_channel *ch, const char *target, size_t target_len,
                         const char *queue_path);

#endif
