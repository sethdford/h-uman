#ifndef HU_CHANNELS_IMESSAGE_BB_EVENT_H
#define HU_CHANNELS_IMESSAGE_BB_EVENT_H

#include "human/core/allocator.h"
#include <stdbool.h>
#include <stddef.h>

/* Real-time IMCore bridge events from `imsg watch --bb-events`
 * (plan: docs/plans/2026-07-19-native-imessage/bb-events-schema.md).
 *
 * The bridge dylib, injected into Messages.app, appends event records to a
 * JSONL inbox; `imsg watch --bb-events` tails that inbox and re-wraps each
 * record onto the SAME stdout stream that already carries chat.db message
 * rows. The observed stdout envelope is:
 *
 *     {"event":"started-typing","kind":"bridge-event","data":{...}}
 *
 * `kind == "bridge-event"` is the discriminator — chat.db message rows carry
 * no `kind` field at all. Key ORDER is not stable across lines (Swift dict
 * encoding), so this parser is strictly key-based; never assume field order.
 *
 * Scope, honestly: the dylib registers observers for exactly two IMCore
 * notifications, so the complete vocabulary is started-typing / stopped-typing
 * / aliases-removed. Read receipts, tapbacks and edit/unsend do NOT arrive
 * here — tapbacks come from `imsg watch --reactions` (chat.db-derived), and
 * the other two are not observed at all.
 *
 * Everything fails CLOSED: a malformed, truncated, or unrecognised line is not
 * a bridge event, and never authorizes behavior. */

typedef enum hu_imessage_bb_kind {
    HU_IMSG_BB_NONE = 0,        /* not a bridge-event line (or unparseable) */
    HU_IMSG_BB_TYPING_START,    /* remote party began typing */
    HU_IMSG_BB_TYPING_STOP,     /* remote party stopped typing */
    HU_IMSG_BB_ALIASES_REMOVED, /* account aliases removed (operational) */
    HU_IMSG_BB_UNKNOWN,         /* a bridge event we do not model yet */
} hu_imessage_bb_kind_t;

/* Fixed-size fields: these are handles and chat GUIDs, both well under 128B,
 * and a stack-only struct keeps the hot watch-drain path allocation-free. */
#define HU_IMSG_BB_ID_CAP 128

typedef struct hu_imessage_bb_event {
    hu_imessage_bb_kind_t kind;
    char chat_guid[HU_IMSG_BB_ID_CAP]; /* e.g. "iMessage;-;+15551234567" */
    char handle[HU_IMSG_BB_ID_CAP];    /* e.g. "+15551234567" */
    double timestamp;                  /* Unix epoch SECONDS, fractional */
} hu_imessage_bb_event_t;

/* Pure parse of ONE stdout line from `imsg watch --bb-events`.
 *
 * Returns true only when the line is a bridge event (`kind == "bridge-event"`)
 * — i.e. when the caller should NOT treat it as a chat.db message row. Message
 * rows, blank lines, truncated JSON and garbage all return false with
 * *out zeroed (kind == HU_IMSG_BB_NONE).
 *
 * A recognised bridge event whose `data` object is missing or lacks the
 * inferred chat/handle keys still parses (with empty ids) rather than being
 * dropped: those key names are inferred from the dylib string table, not
 * observed from real traffic (see schema doc §5), so dropping on mismatch
 * would silently blind us if the guess is wrong. */
bool hu_imessage_bb_event_parse(hu_allocator_t *alloc, const char *line, size_t len,
                                hu_imessage_bb_event_t *out);

/* ── Line framing over a pipe ───────────────────────────────────────────
 * The watch subprocess is read with read(2), so event records arrive split
 * across arbitrary chunk boundaries. This accumulator reassembles whole lines
 * and invokes `on_event` once per recognised bridge event.
 *
 * Lives here rather than in the channel so it can be driven directly by REAL
 * captured `imsg watch --bb-events` output in tests — the framing is where the
 * subtle bugs are (split mid-JSON, \r\n, over-long lines), and it must be
 * exercised against real bytes, not hand-written ones. */

typedef struct hu_imessage_bb_stream {
    char line[4096];
    size_t line_len;
    bool overflow; /* dropping the remainder of an over-long line */
} hu_imessage_bb_stream_t;

typedef void (*hu_imessage_bb_event_fn)(const hu_imessage_bb_event_t *ev, void *user);

/* Feed one read(2) chunk. Non-event lines (chat.db message rows) are skipped
 * silently — the caller keeps handling those exactly as before. Zero-initialise
 * the stream before first use. */
void hu_imessage_bb_stream_consume(hu_imessage_bb_stream_t *st, hu_allocator_t *alloc,
                                   const char *buf, size_t n, hu_imessage_bb_event_fn on_event,
                                   void *user);

/* ── Activation gate ────────────────────────────────────────────────────
 * Per .claude/rules/feature-gate-requires-measurement.md, a new subsystem
 * that can change what gets SENT ships OFF → SHADOW → LIVE. */

typedef enum hu_imessage_bb_mode {
    HU_IMSG_BB_MODE_OFF = 0, /* default: `--bb-events` not even requested */
    HU_IMSG_BB_MODE_SHADOW,  /* stream consumed; events logged; no behavior change */
    HU_IMSG_BB_MODE_LIVE,    /* events shape send behavior */
} hu_imessage_bb_mode_t;

/* Parses HU_IMESSAGE_BB_EVENTS. NULL/empty/unrecognised ⇒ OFF (fail closed).
 * Precedence is LIVE > SHADOW > OFF, matching the other h-uman gates. */
hu_imessage_bb_mode_t hu_imessage_bb_mode_from_env(const char *value);

/* ── Send-hold policy (the reason typing matters) ───────────────────────
 * Extracted as a pure predicate per
 * .claude/rules/security-predicate-extraction.md so the decision is testable
 * without a live bridge, a subprocess, or a chat.db.
 *
 * `last_kind` is the most recent typing event seen for this contact
 * (HU_IMSG_BB_NONE if none), `last_event_ts` its timestamp, `now_ts` the
 * current time — both Unix epoch seconds.
 *
 * Returns true when an outbound send should be HELD because the contact
 * appears to be mid-sentence. On true, *hold_seconds_out (when non-NULL)
 * receives how much longer to wait before reconsidering. */
bool hu_imessage_bb_should_hold_send(hu_imessage_bb_kind_t last_kind, double last_event_ts,
                                     double now_ts, double *hold_seconds_out);

/* The tunable core of the hold decision, split out so the timing policy can be
 * reasoned about and tested on its own. `elapsed_s` is how long ago the
 * typing-START was observed (already validated non-negative by the caller).
 *
 * Returns true to HOLD the outbound send. On true, *hold_seconds_out (when
 * non-NULL) receives how long to wait before reconsidering.
 *
 * The tension: iMessage sends no "typing stopped" event when a contact simply
 * abandons a draft, so a naive "hold while typing" waits forever. The policy
 * must therefore bound how long a single typing-START is allowed to suppress
 * a send. */
bool hu_imessage_bb_hold_policy(double elapsed_s, double *hold_seconds_out);

#endif /* HU_CHANNELS_IMESSAGE_BB_EVENT_H */
