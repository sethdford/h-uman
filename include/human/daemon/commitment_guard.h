#ifndef HU_DAEMON_COMMITMENT_GUARD_H
#define HU_DAEMON_COMMITMENT_GUARD_H

/*
 * Commitment guard (HU_COMMITMENT_GUARD=off|shadow|live, default off).
 *
 * The twin must never commit Seth to something he can't or wouldn't do. Before
 * a drafted reply is sent, this module asks: does the draft accept or propose a
 * plan or time, agree to give or lend money or things, promise a favour, or
 * make a sensitive decision or disclosure? If it does:
 *
 *   plan, calendar busy          -> REWRITE_CONFLICT: the local model rewrites
 *                                   the reply so it does not commit, and the
 *                                   owner is notified.
 *   plan, calendar unknown       -> HOLD (no calendar access, no helper, or the
 *                                   draft names no time to check).
 *   money / sensitive / big favour -> HOLD: the commitment part is held, a
 *                                   natural non-committal reply goes out
 *                                   (local rewrite), the owner is notified.
 *   plan free, small favour      -> ALLOW.
 *
 * A rewrite is re-checked by the detector; if it still commits, or the rewrite
 * fails, NOTHING is sent and the owner is notified (safety floor: the cost of
 * a wrong send is Seth's real life).
 *
 * Pipeline per drafted reply:
 *   1. hu_commitment_prefilter — cheap, RECALL-oriented keyword pass (time and
 *      date expressions, commitment verbs, money, request cues in the inbound).
 *      It only decides whether to spend a model call; it is NOT the decision.
 *      Most turns stop here.
 *   2. Local detector — the loopback provider only (never a cloud fallback),
 *      tagged `X-HU-Purpose: commitment_check` and no X-HU-Priority (reply
 *      path). Returns {"kind","stakes","when","confidence"}.
 *   3. Calendar — free/busy for the commitment's window from the local
 *      EventKit helper (calendar_free_busy.h). No titles, no attendees.
 *   4. hu_commitment_decide — pure decision table above.
 *
 *   off    — nothing runs; the reply is byte-identical.
 *   shadow — steps 1-4 run (and, every HU_COMMIT_AUDIT_EVERY-th prefilter miss,
 *            the detector too, to measure prefilter recall). ONE line per reply,
 *            "[HU_COMMITMENT_GUARD shadow] ...", with enums, booleans and
 *            latencies only. The reply is unchanged.
 *   live   — the decision is applied.
 *
 * Privacy: message text, drafts, event titles, names and handles are never
 * logged. The owner notification names the contact (display name only) and
 * the kind of commitment, never what was said. All model calls are local.
 */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/gate_mode.h"
#include "human/daemon/calendar_free_busy.h"
#include "human/provider.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hu_agent;

#define HU_COMMIT_MIN_CONFIDENCE 0.6
#define HU_COMMIT_AUDIT_EVERY    10   /* shadow: detector on every Nth prefilter miss */
#define HU_COMMIT_TEXT_MAX       600  /* bytes of inbound / draft sent to the detector */
#define HU_COMMIT_TIMED_WINDOW_S 7200 /* "at 7" -> [19:00, 21:00) */

typedef enum hu_commit_kind {
    HU_COMMIT_NONE = 0,
    HU_COMMIT_PLAN,
    HU_COMMIT_MONEY,
    HU_COMMIT_FAVOUR,
    HU_COMMIT_SENSITIVE,
} hu_commit_kind_t;

typedef enum hu_commit_decision {
    HU_COMMIT_SKIP = 0,         /* prefilter miss: no detector call */
    HU_COMMIT_ALLOW,            /* no commitment, or low-stakes and free */
    HU_COMMIT_REWRITE_CONFLICT, /* commits to a time the calendar says is busy */
    HU_COMMIT_HOLD,             /* consequential, or a plan the calendar can't confirm */
    HU_COMMIT_DETECT_FAILED,    /* local detector unavailable / unparseable */
} hu_commit_decision_t;

typedef struct hu_commit_detection {
    hu_commit_kind_t kind;
    bool high_stakes;   /* money and sensitive always; favour per the model */
    bool has_when;      /* when_start/when_end are valid */
    int64_t when_start; /* unix seconds, resolved in local time */
    int64_t when_end;
    double confidence;
} hu_commit_detection_t;

/* What the live/shadow line reports, and what tests assert. */
typedef struct hu_commitment_guard_result {
    bool prefilter;   /* prefilter fired */
    bool audited;     /* shadow recall audit of a prefilter miss */
    bool detector_ok; /* the detector answered with parseable JSON */
    hu_commit_detection_t detection;
    hu_calendar_state_t calendar;
    hu_commit_decision_t decision;
    bool rewritten;  /* LIVE: *response replaced by a local rewrite */
    bool suppressed; /* LIVE: *response freed and NULLed: nothing is sent */
    bool notified;   /* LIVE: owner notified */
    int64_t prefilter_us;
    int64_t detect_ms;
    int64_t calendar_ms;
    int64_t rewrite_ms;
} hu_commitment_guard_result_t;

/* Everything the guard touches outside the draft, injectable for tests. */
typedef struct hu_commitment_guard_io {
    hu_allocator_t *alloc; /* owns *response (size len + 1) */
    hu_provider_t local;   /* loopback provider; vtable NULL = unavailable */
    const char *model;
    size_t model_len;
    hu_calendar_query_fn calendar; /* NULL -> HU_CAL_UNKNOWN */
    void *calendar_ctx;
    const char *contact_name; /* owner notice only; NULL -> "a contact" */
    int64_t now;              /* unix seconds, for the detector's "Now:" */
} hu_commitment_guard_io_t;

/* Gate mode from HU_COMMITMENT_GUARD; default OFF. */
hu_gate_mode_t hu_commitment_guard_mode(void);

/* Recall-oriented pass over the draft and the inbound it answers. True = worth
 * a detector call. Pure; NULL-safe. */
bool hu_commitment_prefilter(const char *draft, size_t draft_len, const char *inbound,
                             size_t inbound_len);

/* Deterministic money floor, used only when the detector is unavailable: the
 * draft offers to pay / send / lend money. Pure; NULL-safe. */
bool hu_commitment_money_floor(const char *draft, size_t draft_len);

/* Parse the detector's reply (JSON object, possibly wrapped in prose or a code
 * fence). Dates in "when" are resolved in local time. False when there is no
 * object or "kind" is not one of none|plan|money|favour|sensitive. */
bool hu_commitment_parse(const char *raw, size_t raw_len, hu_commit_detection_t *out);

/* Pure decision table (see the top of this header). */
hu_commit_decision_t hu_commitment_decide(const hu_commit_detection_t *d, hu_calendar_state_t cal);

/* Run the guard on one drafted reply. OFF returns at once without touching
 * anything. SHADOW never changes *response. LIVE may replace *response (a
 * rewrite, allocated with io->alloc) or free it and set it to NULL / 0
 * (suppressed: the caller must not send). Always fills *out when non-NULL. */
hu_error_t hu_commitment_guard_run(hu_gate_mode_t mode, const hu_commitment_guard_io_t *io,
                                   const char *inbound, size_t inbound_len, char **response,
                                   size_t *response_len, hu_commitment_guard_result_t *out);

/* Daemon glue for the reactive reply path: resolves the gate, the agent's
 * loopback provider, the contact's display name and the EventKit helper, then
 * runs the guard and logs one aggregate line. Returns true when *response
 * changed (rewritten or suppressed). */
bool hu_daemon_commitment_guard_apply(struct hu_agent *agent, const char *batch_key, size_t key_len,
                                      const char *inbound, size_t inbound_len, char **response,
                                      size_t *response_len);

const char *hu_commit_kind_name(hu_commit_kind_t k);
const char *hu_commit_decision_name(hu_commit_decision_t d);

#ifdef HU_IS_TEST
/* Test hooks for the daemon glue: a provider stands in for the loopback
 * resolution, a calendar function for the EventKit helper. NULL clears. */
void hu_commitment_guard_set_test_provider(const hu_provider_t *p);
void hu_commitment_guard_set_test_calendar(hu_calendar_query_fn fn, void *ctx);
/* Detector calls made since the last reset (shadow audit counter included). */
void hu_commitment_guard_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* HU_DAEMON_COMMITMENT_GUARD_H */
