#ifndef HU_IDENTITY_RESOLVER_H
#define HU_IDENTITY_RESOLVER_H

/*
 * identity_resolver — conservative cross-channel identity unification.
 *
 * Builds an in-memory graph that recognizes the same person across
 * iMessage, Slack, Discord, Telegram, email, etc. — given only a list
 * of (handle, channel) pairs and (optionally) display names.
 *
 * Design rules (privacy-critical):
 *
 *   1. Conservative defaults. HIGH confidence is reserved for
 *      canonical-equivalence matches: same phone number (last-10
 *      digits) or same canonicalized email address. Display-name
 *      similarity NEVER reaches HIGH — at most LOW.
 *
 *   2. The merge confidence of a contact is the WEAKEST link in
 *      the alias chain. One LOW alias drags the whole contact to
 *      LOW. Callers can then choose to (a) treat HIGH-only contacts
 *      as authoritative, (b) treat MEDIUM as queryable but not
 *      fact-mergeable, and (c) treat LOW as "candidate — ask user".
 *
 *   3. Non-destructive. This module produces an alias table; it
 *      does NOT rewrite persona facts or memory subjects. Callers
 *      consult the table at query time.
 *
 *   4. Opaque platform IDs (Slack U0..., Discord user#tag,
 *      Telegram numeric ID) carry NO canonical signal on their own.
 *      They merge into a contact only when a phone/email/display-name
 *      bridges them — and a display-name bridge alone caps the
 *      merge at LOW.
 *
 * This module is a separate abstraction from src/memory/contact_graph.c
 * (which is the SQLite-bound runtime store used by the daemon for
 * routing). The identity_resolver is a batch, in-memory resolver used
 * by the memory layer to unify cross-channel facts conservatively.
 * Bridging the two (e.g. feeding contact_graph rows into a
 * hu_identity_resolve call) is left to callers.
 */

#include "human/core/error.h"
#include <stddef.h>

#define HU_IDENTITY_MAX_ALIASES  8
#define HU_IDENTITY_MAX_CONTACTS 256
#define HU_IDENTITY_HANDLE_CAP   128
#define HU_IDENTITY_CHANNEL_CAP  16
#define HU_IDENTITY_NAME_CAP     64

typedef enum {
    HU_IDENTITY_CONFIDENCE_NONE = 0,
    HU_IDENTITY_CONFIDENCE_LOW,    /* heuristic match — DON'T auto-merge facts */
    HU_IDENTITY_CONFIDENCE_MEDIUM, /* corroborated — soft merge for query */
    HU_IDENTITY_CONFIDENCE_HIGH,   /* unambiguous — safe to merge facts */
} hu_identity_confidence_t;

typedef struct {
    /* Canonical display name (first non-empty input name, or the
     * first handle if no name was supplied). Best-effort label only,
     * NOT used as a merge key. */
    char canonical_name[HU_IDENTITY_NAME_CAP];

    /* Aliases — the raw (handle, channel) pairs that resolved into
     * this contact. alias_count is the number of valid entries. */
    char aliases[HU_IDENTITY_MAX_ALIASES][HU_IDENTITY_HANDLE_CAP];
    char alias_channels[HU_IDENTITY_MAX_ALIASES][HU_IDENTITY_CHANNEL_CAP];
    size_t alias_count;

    /* WEAKEST confidence in the chain. A contact with one HIGH-merge
     * phone bridge and one LOW display-name bridge is LOW overall —
     * callers must use this to gate destructive operations. */
    hu_identity_confidence_t merge_confidence;
} hu_identity_contact_t;

typedef struct {
    hu_identity_contact_t contacts[HU_IDENTITY_MAX_CONTACTS];
    size_t contact_count;
} hu_identity_graph_t;

/*
 * Build the identity graph from parallel arrays of handles, channels,
 * and (optionally) display names.
 *
 *   handles[i]       — platform handle: "+15551234567", "U07ALICE",
 *                      "alice#1234", "alice@gmail.com", etc.
 *   channels[i]      — "imessage", "slack", "discord", "telegram",
 *                      "email", or any short identifier.
 *   display_names[i] — optional human-readable label. May be NULL or
 *                      an array containing NULL entries.
 *   handle_count     — number of valid entries in each input array.
 *   out              — caller-owned graph; will be zeroed on entry.
 *
 * Canonicalization rules (see file header):
 *   - Phone numbers: strip non-digits; compare last 10 digits if
 *     both have ≥10. Match → HIGH confidence bridge between aliases.
 *   - Emails: lowercase; strip dots in gmail.com local part; match
 *     full canonicalized form → HIGH confidence bridge.
 *   - Slack/Discord/Telegram IDs: opaque, no canonicalization.
 *     A bridge to such an alias requires either a co-located
 *     display-name OR a higher-confidence alias already in the
 *     contact (transitive).
 *   - Display names: lowercase; compare leading first-name token.
 *     Match between two different opaque IDs → LOW only.
 *
 * Returns HU_OK on success. HU_ERR_INVALID_ARGUMENT on null inputs.
 * If handle_count exceeds HU_IDENTITY_MAX_CONTACTS, the first
 * HU_IDENTITY_MAX_CONTACTS entries are processed and the rest
 * silently dropped (no crash). Aliases overflowing
 * HU_IDENTITY_MAX_ALIASES per contact are likewise dropped.
 */
hu_error_t hu_identity_resolve(const char *const *handles, const char *const *channels,
                               const char *const *display_names, size_t handle_count,
                               hu_identity_graph_t *out);

/*
 * Look up a handle in the graph. Returns a borrowed pointer into the
 * graph (caller must not free) or NULL if the handle has no merged
 * contact entry (i.e. it sits alone in its own contact, or wasn't
 * resolved at all). For "is X really Y?" callers should compare
 * the returned canonical_name + check merge_confidence.
 */
const hu_identity_contact_t *hu_identity_lookup(const hu_identity_graph_t *graph,
                                                const char *handle);

/*
 * Atomic, crash-safe persistence. The graph is written verbatim
 * (no encryption — this is on-device data, same as personal_model).
 * Save uses the tmp+fsync+rename pattern.
 *
 * load returns HU_ERR_NOT_FOUND if the file doesn't exist, leaving
 * *out initialized to an empty graph.
 */
hu_error_t hu_identity_save(const hu_identity_graph_t *graph, const char *path);
hu_error_t hu_identity_load(hu_identity_graph_t *out, const char *path);

/* ------- internal helpers exposed for unit testing ------- */

/* Canonicalize a phone string: keep digits only, drop a leading
 * country code if total >= 11 digits (assume US/CA pattern), write
 * trailing 10 digits to out. Returns digits written (0..10). If
 * fewer than 7 digits are present, writes nothing and returns 0
 * (too short to be a meaningful phone). */
size_t hu_identity_canonicalize_phone(const char *input, char *out, size_t cap);

/* Canonicalize an email: lowercase; strip dots in local part when
 * domain is gmail.com (or googlemail.com). Returns chars written. */
size_t hu_identity_canonicalize_email(const char *input, char *out, size_t cap);

#endif /* HU_IDENTITY_RESOLVER_H */
