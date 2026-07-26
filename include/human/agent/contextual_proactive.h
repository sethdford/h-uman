#ifndef HU_CONTEXTUAL_PROACTIVE_H
#define HU_CONTEXTUAL_PROACTIVE_H

/* Contextual (context-driven) proactive outreach.
 *
 * The existing proactive check-in path (hu_service_run_proactive_checkins) is
 * SCHEDULE-driven: it texts first "because it's 10am Tuesday". This module
 * closes the gap to CONTEXT-driven outreach: when a conversation surfaces a
 * future-dated event ("my interview is Friday", "I have surgery next week"),
 * detect it, freeze a post-event "how'd it go?" message FROM the detected
 * topic, and schedule it to fire after the event through the EXISTING governed
 * scheduled-send path (master_enabled gate + backoff governor + outbound
 * sanitizer + validator chain).
 *
 * Design notes:
 *   - Detection reuses hu_event_extract (topic + temporal_ref). This module
 *     adds only: temporal_ref -> absolute post-event send time, topic
 *     normalization, message freezing, and the OFF/SHADOW/ON activation gate.
 *   - The sent text is frozen at DETECT time from the real stored topic and
 *     never regenerated at send time. This is the cross-contact-bleed guard:
 *     nothing is invented when the message fires (cf. the director's
 *     no-invented-FACTS rule). The specific (topic) always comes from the
 *     stored obligation.
 *   - Activation is gated OFF -> SHADOW -> ON per
 *     .claude/rules/feature-gate-requires-measurement.md. Unprompted outreach
 *     is the highest-stakes behavior in the system; it ships OFF by default
 *     and is only promoted by a blind A/B measurement, not a green suite.
 *
 * All decision functions are PURE (take now_ts, no clock / no env / no I/O) so
 * the temporal logic is unit-testable without the daemon or real time.
 */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Activation state. Precedence when parsing the env: ON > SHADOW > OFF.
 * Default (unset / unrecognized) is OFF — zero behavior change, zero cost. */
typedef enum hu_contextual_proactive_mode {
    HU_CONTEXTUAL_PROACTIVE_OFF = 0,    /* detector does not run; no scheduling */
    HU_CONTEXTUAL_PROACTIVE_SHADOW = 1, /* detect + log what WOULD be sent; no send */
    HU_CONTEXTUAL_PROACTIVE_ON = 2,     /* detect + schedule via governed send path */
} hu_contextual_proactive_mode_t;

/* Max contextual obligations decided from a single inbound message. */
#define HU_CONTEXTUAL_PROACTIVE_MAX 4

/* Minimum event-extraction confidence to act on a detected event. Below this,
 * the temporal reference is too weak to risk an unprompted text. */
#define HU_CONTEXTUAL_PROACTIVE_MIN_CONFIDENCE 0.7

/* Local hour-of-day at which a "how'd it go?" fires on the event day. Chosen so
 * a daytime event ("interview is Friday") is followed up Friday evening. */
#define HU_CONTEXTUAL_PROACTIVE_SEND_HOUR 19

/* A detected SITUATION — "something happened//will happen worth noticing" — not
 * a message.
 *
 * This carried a `message[256]` frozen at detection time until 2026-07-26. That
 * design produced three separate garbled sends because it interpolated
 * extractor output into "how'd the %s go?" and validated the fragment against a
 * finite word blocklist:
 *   07-21  "how'd the What's go?"
 *   07-26  "hey how are you doing with don't understand provide?"  (sister
 *          replied "Turn AI off")
 *   07-26  "how'd the Friday or Saturday of go?" — was still QUEUED to send on
 *          Aug 2 when it was purged
 * Each was patched by adding words to the blocklist. Validating arbitrary
 * extracted prose against a word list is unwinnable, and freezing the string
 * days ahead of delivery means a bad one sits in the queue waiting.
 *
 * So the detector now emits only what it can actually know — WHAT and WHEN —
 * and the message is composed at SEND time by init_proposer, which already owns
 * propose-or-decline with the persona, the G1-G9 guards and DPO capture on
 * reject (see hu_proactive_compose_inputs.situation_context, built for exactly
 * this in docs/plans/2026-05-26-m3-dispatch-unification/). An LLM looking at the
 * live thread can decline a nonsense topic; snprintf cannot. */
typedef struct hu_contextual_proactive_decision {
    char topic[128];    /* normalized topic, e.g. "interview" (from the message) */
    int64_t send_at_ms; /* absolute epoch millis the situation becomes relevant */
    double confidence;  /* event-extraction confidence [0,1] */
} hu_contextual_proactive_decision_t;

typedef struct hu_contextual_proactive_result {
    hu_contextual_proactive_decision_t items[HU_CONTEXTUAL_PROACTIVE_MAX];
    size_t count;
} hu_contextual_proactive_result_t;

/* Parse an activation mode from a string (the HU_PROACTIVE_CONTEXTUAL value).
 * "on"/"live"/"1"/"true" -> ON; "shadow" -> SHADOW; everything else (incl.
 * NULL/empty/"off"/"0") -> OFF. Pure; NULL-safe. */
hu_contextual_proactive_mode_t hu_contextual_proactive_mode_from_str(const char *s);

/* Read HU_PROACTIVE_CONTEXTUAL from the environment and parse it. Default OFF. */
hu_contextual_proactive_mode_t hu_contextual_proactive_mode(void);

/* Stable lowercase label for a mode ("off"/"shadow"/"on"), for log/evidence. */
const char *hu_contextual_proactive_mode_str(hu_contextual_proactive_mode_t mode);

/* Resolve an event_extract temporal_ref ("Friday", "next week", "the 23rd",
 * "March 15th", "tomorrow", "in 3 days") to the absolute epoch SECONDS at which
 * a post-event "how'd it go?" should fire — the event day at local
 * HU_CONTEXTUAL_PROACTIVE_SEND_HOUR. Returns 0 if the reference is unresolvable,
 * resolves to the past, or is too vague to anchor (e.g. "this week", "yesterday").
 * Pure: takes now_ts, never calls time(). */
int64_t hu_contextual_proactive_resolve_send_at(const char *temporal_ref, size_t len,
                                                int64_t now_ts);

/* Normalize a raw event description into a clean topic noun for the message
 * slot: strip leading filler ("my ", "a ", "an ", "the ", "i have ", "got ",
 * "have a ", ...), trailing punctuation, and surrounding space. Writes a
 * NUL-terminated lowercase-preserving string to out. Returns the written
 * length (0 if the topic is empty after stripping). Pure. */
size_t hu_contextual_proactive_normalize_topic(const char *topic, size_t len, char *out,
                                               size_t cap);

/* Render a detected situation as the SITUATION FRAME handed to init_proposer —
 * "what's up with this contact right now", not an outbound message.
 *
 * Replaces hu_contextual_proactive_build_message + the
 * hu_contextual_proactive_topic_is_sendable blocklist, both deleted 2026-07-26.
 * Those took extractor output, spliced it into "how'd the %s go?", and tried to
 * validate the fragment against ~40 clause words. Three garbled messages reached
 * real contacts anyway, each "fixed" by adding words to the list; validating
 * arbitrary prose against a finite word list cannot be completed.
 *
 * The frame is deliberately DESCRIPTIVE, not imperative: it states what was
 * detected and leaves whether-and-how entirely to the proposer, which sees the
 * live thread and can decline. Feeds
 * hu_proactive_compose_inputs.situation_context, whose doc comment already
 * scoped it as "silence duration, event/calendar triggers, joke callbacks".
 *
 * A low-quality topic is no longer a correctness problem: the model reads it in
 * context and declines, where snprintf could only splice. Writes a
 * NUL-terminated line; returns bytes written (0 on empty topic — never fabricate
 * a topicless situation). Pure. */
size_t hu_contextual_proactive_situation_frame(const hu_contextual_proactive_decision_t *d,
                                               int64_t now_ts, char *out, size_t cap);

/* Detect future-dated events in an inbound/outbound message and decide the set
 * of contextual proactive obligations to store. For each extracted event with
 * confidence >= MIN_CONFIDENCE, a non-empty normalized topic, and a resolvable
 * FUTURE send time, records {topic, frozen message, send_at_ms}. De-duplicates
 * by topic. Does NOT schedule or send — the caller applies the activation gate.
 * Pure w.r.t. the clock (takes now_ts); allocates only internally for
 * extraction and frees it before returning. */
hu_error_t hu_contextual_proactive_decide(hu_allocator_t *alloc, const char *inbound, size_t len,
                                          int64_t now_ts, hu_contextual_proactive_result_t *out);

/* Render the SHADOW-mode metric: a structured one-line capture of the decision
 * distribution for one inbound message — count + per-decision
 * topic@send_epoch(confidence). This is the SHADOW evidence artifact
 * (cf. hu_salience_summarize): a PURE function the daemon logs and tests assert,
 * so the metric is proven via the test path, never by running the live daemon.
 * Writes a NUL-terminated string to out; returns the written length (0 if no
 * decisions). Pure. */
size_t hu_contextual_proactive_shadow_summary(const hu_contextual_proactive_result_t *res,
                                              const char *contact_id, char *out, size_t cap);

#endif /* HU_CONTEXTUAL_PROACTIVE_H */
