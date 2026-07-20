#ifndef HU_CHANNELS_IMESSAGE_CAPS_H
#define HU_CHANNELS_IMESSAGE_CAPS_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>

/* iMessage capability probe (plan: docs/plans/2026-07-19-native-imessage).
 *
 * Native fidelity (real tapbacks, threaded replies, typing indicators, read
 * receipts, edit/unsend, effects) runs through Apple's private IMCore via the
 * `imsg` bridge helper, which macOS gates behind library validation. On a
 * SIP-enabled box the bridge is unavailable and every advanced verb MUST
 * degrade honestly rather than fall back to UI puppetry (synthetic keystrokes
 * / AXShowMenu) or to a green SMS bubble.
 *
 * This module is the single source of truth for "what can we actually do".
 * Everything fails CLOSED: an unparseable / missing / un-probed status never
 * authorizes an advanced verb. */

typedef struct hu_imessage_caps {
    bool basic;       /* send / receive / history — works with SIP on */
    bool advanced;    /* IMCore bridge live: reactions, typing, receipts, ... */
    bool sip_enabled; /* SIP on ⇒ bridge gated (informational for operators) */
    bool probed;      /* a status report was successfully parsed */
    /* Per-selector availability. The bridge being live does NOT imply every
     * IMCore selector exists: on macOS 26 `editMessage` is absent while
     * `retractMessagePart` is present, so a gate keyed only off `advanced`
     * would emit edit calls that always fail. When the status report carries
     * no selector section (older/newer imsg), selectors_reported is false and
     * these verbs fall back to `advanced`. */
    bool selectors_reported;
    bool sel_edit;    /* editMessage / editMessageItem */
    bool sel_retract; /* retractMessagePart ⇒ unsend */
} hu_imessage_caps_t;

typedef enum hu_imessage_verb {
    HU_IMSG_VERB_SEND = 0,       /* plain send — basic */
    HU_IMSG_VERB_REACT,          /* real tapback (associatedMessageType) */
    HU_IMSG_VERB_REPLY_THREADED, /* nesting reply (databaseReplyToGUID) */
    HU_IMSG_VERB_TYPING,         /* send-side typing indicator */
    HU_IMSG_VERB_READ_RECEIPT,   /* mark-read */
    HU_IMSG_VERB_EDIT,           /* edit sent message (macOS 13+) */
    HU_IMSG_VERB_UNSEND,         /* retract sent message (macOS 13+) */
    HU_IMSG_VERB_EFFECT,         /* com.apple.messages.effect.CK.* */
} hu_imessage_verb_t;

/* Pure parse of `imsg status` output. Fails closed: on empty/garbage input
 * *caps is all-false with probed=false and HU_OK is still returned (absence of
 * capability is not an error). Returns HU_ERR_INVALID_ARGUMENT only on NULL
 * arguments. Word-boundary aware — "Not available" must NOT read as available. */
hu_error_t hu_imessage_caps_parse(const char *status_out, size_t len, hu_imessage_caps_t *caps);

/* True when `verb` is authorized by these capabilities. NULL/zeroed caps deny
 * everything (fail closed). Advanced verbs require caps->advanced. */
bool hu_imessage_caps_allows(const hu_imessage_caps_t *caps, hu_imessage_verb_t verb);

/* One-line operator-readable summary, e.g.
 * "imessage caps: basic=yes bridge=no sip=on (advanced verbs degrade)". */
void hu_imessage_caps_describe(const hu_imessage_caps_t *caps, char *buf, size_t cap);

/* Runs `imsg status` and parses it. Fails closed on any spawn/timeout error.
 * Caller owns nothing. Intended to be called once at channel start and cached. */
hu_error_t hu_imessage_caps_probe(hu_allocator_t *alloc, hu_imessage_caps_t *caps);

/* Process-wide cached probe — the single shared capability view used by every
 * native call site (send path, reply path, react path). Probes once on first
 * call and logs the result; never NULL. */
const hu_imessage_caps_t *hu_imessage_caps_cached(hu_allocator_t *alloc);

/* ── T0.1 blue guard ────────────────────────────────────────────────────
 * "Perfect and blue": the daemon must never emit a green bubble. Apple's own
 * chat.db is the SIP-free source of truth — `handle.service` and
 * `message.service` carry {iMessage, SMS, RCS}. RCS renders GREEN, so blue
 * means iMessage and nothing else. (`imsg whois --local` was evaluated and
 * rejected: it reports service=unknown even for known iMessage handles.) */

typedef enum hu_imessage_service {
    HU_IMSG_SERVICE_UNKNOWN = 0,
    HU_IMSG_SERVICE_IMESSAGE,
    HU_IMSG_SERVICE_SMS,
    HU_IMSG_SERVICE_RCS,
} hu_imessage_service_t;

typedef enum hu_blue_verdict {
    HU_BLUE_ALLOW = 0, /* proven iMessage-reachable — safe to send */
    HU_BLUE_HOLD,      /* green or unproven — do NOT send */
} hu_blue_verdict_t;

/* Case-insensitive exact-token parse of a chat.db service string. */
hu_imessage_service_t hu_imessage_service_from_string(const char *s, size_t len);

/* Only HU_IMSG_SERVICE_IMESSAGE is blue. */
bool hu_imessage_service_is_blue(hu_imessage_service_t svc);

/* Decide whether sending to a handle stays blue.
 * `recent_msg_service` = service of the most recent message with that handle
 * (freshest evidence, wins when known); `handle_service` = the handle row's
 * service (authoritative fallback). Fails CLOSED: no evidence ⇒ HOLD. */
hu_blue_verdict_t hu_imessage_blue_verdict(hu_imessage_service_t recent_msg_service,
                                           hu_imessage_service_t handle_service);

/* Run an `imsg` bridge verb and report success. Centralizes the
 * spawn/check/free idiom every native call site would otherwise repeat. */
bool hu_imsg_run_ok(hu_allocator_t *alloc, const char *const *argv, int timeout_s);

#endif /* HU_CHANNELS_IMESSAGE_CAPS_H */
