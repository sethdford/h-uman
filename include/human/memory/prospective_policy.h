#ifndef HU_MEMORY_PROSPECTIVE_POLICY_H
#define HU_MEMORY_PROSPECTIVE_POLICY_H
/*
 * Prospective memory v2 — the pure decisions (spec
 * docs/superpowers/specs/2026-09-30-prospective-memory-v2-design.md §4.2-4.4).
 *
 * Every function here depends only on its arguments (the two gate readers
 * add getenv): Filter, Decide, the done-after-evidence rule, verdict parsing,
 * the renderers and the judge prompt. The store (prospective_repo.h) and the
 * orchestration (prospective_v2.h) call these; tests pin their truth tables
 * without a database or a model (.claude/rules/security-predicate-extraction.md).
 */
#include "human/core/gate_mode.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HU_PROSPECTIVE_RENDER_CAP        3 /* intentions per directive */
#define HU_PROSPECTIVE_JUDGE_CAP         3 /* Decide calls per turn */
#define HU_PROSPECTIVE_MAX_ATTEMPTS      2 /* surfaced-but-unused before expired */
#define HU_PROSPECTIVE_TIME_GRACE_S      (3 * 86400)
#define HU_PROSPECTIVE_BACKFILL_EXPIRE_S (14 * 86400)
#define HU_PROSPECTIVE_HISTORY_TURNS     20
#define HU_PROSPECTIVE_KEY_TERMS_MAX     6
#define HU_PROSPECTIVE_KEY_TERM_LEN      32

typedef enum hu_prospective_cue_kind {
    HU_PM_CUE_KEYWORD = 0,
    HU_PM_CUE_TIME,
    HU_PM_CUE_AFTER_EVENT, /* reserved: no v1 source, never eligible */
} hu_prospective_cue_kind_t;

typedef enum hu_prospective_status {
    HU_PM_PENDING = 0,
    HU_PM_SURFACED,
    HU_PM_DONE,
    HU_PM_CANCELED,
    HU_PM_EXPIRED,
} hu_prospective_status_t;

typedef enum hu_prospective_outcome {
    HU_PM_OUTCOME_NONE = 0,
    HU_PM_OUTCOME_USED,
    HU_PM_OUTCOME_IGNORED,
    HU_PM_OUTCOME_SUPPRESSED,
} hu_prospective_outcome_t;

typedef enum hu_prospective_source {
    HU_PM_SOURCE_EXTRACTOR = 0,
    HU_PM_SOURCE_PROMISE_KEEPER,
    HU_PM_SOURCE_FOLLOWUP,
} hu_prospective_source_t;

/* Column spellings. *_str returns NULL for an out-of-range value; *_parse
 * returns false (and leaves *out alone) for NULL or an unknown string. */
const char *hu_prospective_cue_kind_str(hu_prospective_cue_kind_t k);
bool hu_prospective_cue_kind_parse(const char *s, hu_prospective_cue_kind_t *out);
const char *hu_prospective_status_str(hu_prospective_status_t s);
bool hu_prospective_status_parse(const char *s, hu_prospective_status_t *out);
const char *hu_prospective_outcome_str(hu_prospective_outcome_t o); /* NULL for NONE */
const char *hu_prospective_source_str(hu_prospective_source_t s);

/* The legacy `fired` value a status writes: pending/surfaced 0, done 1,
 * canceled 2, expired 3. (The reverse mapping is the migration's trigger.) */
int hu_prospective_status_to_fired(hu_prospective_status_t s);

/* Gates (feature-gate-requires-measurement): HU_PROSPECTIVE for the reactive
 * keyword path, HU_PROSPECTIVE_TIME for time cues that initiate a message.
 * Both default OFF. */
hu_gate_mode_t hu_prospective_gate_mode(void);
hu_gate_mode_t hu_prospective_time_gate_mode(void);
/* The one operator line per process for a gate state
 * (silent-config-gated-subsystems): OFF names the key and values that
 * enable it. */
const char *hu_prospective_gate_banner(hu_gate_mode_t mode, bool time_gate);

/* ── Filter (§4.2): rule and clock checks, in code ───────────────────── */
typedef enum hu_prospective_filter {
    HU_PM_FILTER_SKIP = 0, /* not cued / not due / not eligible here */
    HU_PM_FILTER_ELIGIBLE, /* goes to Decide */
    HU_PM_FILTER_EXPIRE,   /* past its window: retire, never fire late */
    HU_PM_FILTER_CAPPED,   /* time cue over the 1-per-contact-per-day cap */
} hu_prospective_filter_t;

typedef struct hu_prospective_filter_facts {
    hu_prospective_cue_kind_t cue_kind;
    hu_prospective_status_t status;
    bool is_group;        /* group chat: never eligible */
    bool is_self;         /* the owner's own handle: never eligible */
    bool keyword_in_text; /* whole-word match of the cue in the inbound text */
    int64_t due_at;       /* time cues; 0 = none */
    int64_t expires_at;   /* 0 = never */
    int64_t now;
    int64_t grace_s;       /* time cues: due_at + grace_s is the last moment to fire */
    size_t surfaced_today; /* time intentions already surfaced for this contact today */
} hu_prospective_filter_facts_t;

hu_prospective_filter_t hu_prospective_filter(const hu_prospective_filter_facts_t *f);

/* ── Decide (§4.3): the fire-time check ──────────────────────────────── */
typedef enum hu_prospective_verdict {
    HU_PM_VERDICT_FIRE = 0,
    HU_PM_VERDICT_RESOLVED,
    HU_PM_VERDICT_CANCEL,
    HU_PM_VERDICT_NOT_NOW,
    HU_PM_VERDICT_PARSE_FAIL,
} hu_prospective_verdict_t;

typedef enum hu_prospective_action {
    HU_PM_ACT_KEEP_PENDING = 0,
    HU_PM_ACT_SURFACE,
    HU_PM_ACT_MARK_DONE,
    HU_PM_ACT_MARK_CANCELED,
} hu_prospective_action_t;

/* First word of the judge's answer, case-folded, after an optional leaked
 * "<think>…</think>" block: "fire"; "already_resolved" / "already resolved"
 * / "resolved"; "cancel" / "canceled" / "cancelled"; "not_now" / "not now" /
 * "not-now". Anything else is PARSE_FAIL. Reads at most `len` bytes. */
hu_prospective_verdict_t hu_prospective_parse_verdict(const char *raw, size_t len);
/* "fire" | "already_resolved" | "cancel" | "not_now" | "parse_fail"; NULL out of range. */
const char *hu_prospective_verdict_str(hu_prospective_verdict_t v);

/* judge_ok=false (model error), NOT_NOW and PARSE_FAIL keep the item
 * pending: the system fails toward silence. */
hu_prospective_action_t hu_prospective_decide(bool judge_ok, hu_prospective_verdict_t v);

/* ── Done only after evidence (§4.3) ─────────────────────────────────── */
/* Content words of an action (>= 4 chars; not a function word, not a
 * generic intention verb like "ask"/"remember"/"check"/"send"; no
 * contractions; possessive 's stripped), lowercased, de-duplicated, at most
 * `max`. Returns the count. */
size_t hu_prospective_key_terms(const char *action, char out[][HU_PROSPECTIVE_KEY_TERM_LEN],
                                size_t max);
/* The delivered reply carries the action: at least half of its key terms
 * (and at least one) appear as whole words, a trailing plural 's' tolerated
 * either way. An action with no key terms can never be proven used. */
bool hu_prospective_reply_uses_action(const char *action, const char *reply, size_t reply_len);
/* Status after a delivery: used -> DONE; else attempts_before + 1 >=
 * max_attempts -> EXPIRED; else back to PENDING. */
hu_prospective_status_t hu_prospective_after_delivery_status(bool used, int attempts_before,
                                                             int max_attempts);

/* ── Rendering ───────────────────────────────────────────────────────── */
typedef enum hu_prospective_render_style {
    HU_PM_RENDER_LEGACY = 0, /* "[PROSPECTIVE MEMORY: Remember to: a (triggered by: c) | …]" */
    HU_PM_RENDER_SOFT, /* "[PROSPECTIVE MEMORY: If it fits naturally, you could bring up: a | …]" */
    HU_PM_RENDER_DUE_LIST, /* "- a\n- b\n" — the proactive due_followups section */
} hu_prospective_render_style_t;

/* Renders up to HU_PROSPECTIVE_RENDER_CAP of `n` items into buf[cap] and
 * returns how many were rendered; 0 means nothing usable (buf is "" and
 * *out_len 0). LEGACY is byte-for-byte the pre-v2 directive for the same
 * inputs and cap 1024, and needs `cues`; SOFT and DUE_LIST ignore `cues`. */
size_t hu_prospective_render(hu_prospective_render_style_t style, const char *const *actions,
                             const char *const *cues, size_t n, char *buf, size_t cap,
                             size_t *out_len);

/* ── Commitment-owner phrasing (spec §4.1: source=promise_keeper mirrors
 * `commitments`, whose hu_commitment_t.owner is "user" or "assistant") ──
 * A commitment the CONTACT made ("user" owner) must render as a question
 * about THEM, never first person as if Seth owed the follow-through.
 * `summary` is the commitment's own clause (e.g. "send the lasagna
 * recipe"); when `contact_committed` is false (Seth's own promise) it
 * passes through unchanged. Returns the bytes written, 0 when `summary`
 * is NULL/empty or the result does not fit in `cap` (buf is then ""). */
size_t hu_prospective_commitment_action(const char *summary, bool contact_committed, char *buf,
                                        size_t cap);

/* ── Dated-moment frames (fix round 1 of task 9) ──────────────────────
 * The dated-moment path queues hu_contextual_proactive_situation_frame's
 * line, "they mentioned <topic> (<when>); confidence <d.dd>", as the
 * delayed follow-up's topic. Its relative <when> is stale once due, and its
 * wrapper words defeat the done-after-evidence key terms, so the time
 * mirror stores just <topic>. Accepts EXACTLY that shape (<when> a
 * relative-time phrase: today / tomorrow / yesterday / in N days / N days
 * ago are what the frame writes) and writes the
 * topic NUL-terminated into out[cap], returning its length; returns 0 (out
 * "") for anything else or when it does not fit — the caller then mirrors
 * the text verbatim. Reads at most `len` bytes of `frame`. */
size_t hu_prospective_frame_topic(const char *frame, size_t len, char *out, size_t cap);

/* ── Ledger -> time-row mirror text (controller rulings F1/F4) ─────────
 * The ONE decision of what action text a dated ledger row (a commitment or
 * a delayed follow-up) mirrors as, shared by the live writers
 * (src/memory/superhuman.c) and the one-time backfill
 * (hu_prospective_v2_backfill) so both produce identical rows:
 *   - owner-owned (`who` NULL, "" or "me"): a follow-up that is a
 *     dated-moment frame mirrors as its topic (hu_prospective_frame_topic);
 *     anything else mirrors verbatim;
 *   - contact-owned (any other `who`): rephrased to third person through
 *     hu_prospective_commitment_action, or SKIPPED when that is not safe —
 *     never a first-person row for a contact's promise.
 * `buf` must hold HU_PROSPECTIVE_MIRROR_CAP bytes. On a VERBATIM / TOPIC /
 * REPHRASED result *action (pointing at `text` or `buf`) and *action_len
 * are the text to store; on a SKIP they are NULL / 0. */
#define HU_PROSPECTIVE_MIRROR_CAP 600

typedef enum hu_prospective_mirror {
    HU_PM_MIRROR_VERBATIM = 0,
    HU_PM_MIRROR_TOPIC,         /* a dated-moment frame's topic, in buf */
    HU_PM_MIRROR_REPHRASED,     /* a contact's promise in third person, in buf */
    HU_PM_MIRROR_SKIP_TOO_LONG, /* contact-owned text too long to rephrase safely */
    HU_PM_MIRROR_SKIP_UNSAFE,   /* contact-owned text the rephraser refused */
} hu_prospective_mirror_t;

hu_prospective_mirror_t hu_prospective_mirror_action(bool is_followup, const char *text,
                                                     size_t text_len, const char *who,
                                                     size_t who_len, char *buf, size_t cap,
                                                     const char **action, size_t *action_len);

/* ── Judge prompt ────────────────────────────────────────────────────── */
const char *hu_prospective_judge_system(size_t *len);
/* The user turn: the last lines of `history` (at most 4000 bytes, cut at a
 * line start; "(none)" when empty), the intention and its cue ("they just
 * mentioned \"<cue>\"" for keyword, "it came due N day(s) ago" for time),
 * then "answer:". Returns the bytes written, 0 when it does not fit. */
size_t hu_prospective_judge_user(char *buf, size_t cap, const char *history, size_t history_len,
                                 const char *action, const char *cue,
                                 hu_prospective_cue_kind_t kind, int64_t overdue_s);

/* Local midnight at or before `now` — the day of the per-day cap. */
int64_t hu_prospective_local_day_start(int64_t now);

#endif /* HU_MEMORY_PROSPECTIVE_POLICY_H */
