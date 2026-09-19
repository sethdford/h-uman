#ifndef HU_AGENT_OUTBOUND_SENSITIVE_H
#define HU_AGENT_OUTBOUND_SENSITIVE_H

/* Outbound sensitive-disclosure detection — does the reply leak the OWNER's
 * own protected data?
 *
 * The 2026-07-29 security audit found the outbound path asymmetric with the
 * inbound one. Inbound classifies sensitivity and routes S3 to the local model
 * (agent_turn.c). Outbound has validators, but they screen for the wrong
 * things: response_guard catches AI-tells (special tokens, deliberation leaks,
 * repetition), crosstalk catches OTHER contacts' content bleeding in. Nothing
 * asked whether the reply contains Seth's street address, card number, or
 * credentials.
 *
 * WHY THIS IS NOT `hu_sensitivity_classify_message`
 * -------------------------------------------------
 * That classifier flags any email, any 10-15 digit run, and keywords like
 * "salary" / "diagnosis" / "phone number" as S2. Inbound that is correct and
 * invisible — it only picks a model. Applied as an outbound BLOCK it would
 * suppress ordinary messages, because the owner says those things constantly.
 * Only the shape-definitive subset is reused, via
 * `hu_sensitivity_hard_secret_shape`.
 *
 * FALSE POSITIVES ARE THE PRIMARY RISK
 * ------------------------------------
 * Seth tells people his city, employer, and kids' names in almost every
 * conversation — his real corpus messages do exactly that ("st. petersburg,
 * waterfront place on tampa bay", "It's Seth, from Vanguard"). A validator
 * that blocks those breaks the product worse than the leak it prevents. Hence
 * the tiers:
 *
 *   NEVER_SEND   — full street address, card/account numbers, credentials,
 *                  SSN-shaped strings. Blocked unconditionally.
 *   TRUST_GATED  — employer, city, kids' names, financial amounts. Fine for a
 *                  known contact, suppressed for an unknown sender. Detected
 *                  and reported today; NOT yet enforced (see the TODO below).
 *   (everything else) — always fine, never inspected.
 *
 * Two matching disciplines keep the tiers honest:
 *
 *   1. A street address must match BOTH a shape (house number + street type)
 *      AND one of the owner's declared values. Shape alone would block
 *      "meet me at 200 Central Ave", which is not the owner's data at all.
 *   2. Short values (city, a child's first name) match on WORD BOUNDARIES,
 *      never bare substrings — otherwise a protected "Ford" fires on
 *      "afford". See ~/.claude/rules/substring-classifier-pitfalls.md.
 *
 * ACTIVATION — off / shadow / live, DEFAULT SHADOW
 * -----------------------------------------------
 * Per .claude/rules/feature-gate-requires-measurement.md, and unusually for
 * that rule, the default is SHADOW rather than OFF: shadow costs nothing
 * visible (it logs what it WOULD have blocked and sends the message
 * unchanged) and shadow output is the only way to learn the real
 * false-positive rate on live traffic BEFORE a block can damage a
 * conversation. Do not promote HU_OUTBOUND_SENSITIVE to `live` without
 * reading a shadow sample and confirming legitimate city/employer/kid-name
 * messages are not being flagged.
 */

/* sensitivity.h supplies hu_hard_secret_kind_t — the shape-definitive subset
 * this module reuses instead of re-implementing SSN/Luhn a third time. */
#include "human/security/sensitivity.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Tiers + categories ───────────────────────────────────────────────── */

typedef enum hu_sensitive_tier {
    HU_SENSITIVE_TIER_NONE = 0,
    HU_SENSITIVE_TIER_NEVER_SEND = 1,  /* blocked for every recipient */
    HU_SENSITIVE_TIER_TRUST_GATED = 2, /* ok for known contacts only */
} hu_sensitive_tier_t;

typedef enum hu_sensitive_category {
    HU_SENSITIVE_CAT_NONE = 0,
    /* NEVER_SEND */
    HU_SENSITIVE_CAT_STREET_ADDRESS,
    HU_SENSITIVE_CAT_HARD_SECRET, /* shape-detected: card / SSN / credential */
    /* TRUST_GATED */
    HU_SENSITIVE_CAT_EMPLOYER,
    HU_SENSITIVE_CAT_CITY,
    HU_SENSITIVE_CAT_FAMILY_NAME,
    HU_SENSITIVE_CAT_FINANCIAL,
} hu_sensitive_category_t;

/* Stable lowercase identifiers for logs. Never NULL, never the matched
 * bytes — the category IS the log payload. */
const char *hu_sensitive_tier_str(hu_sensitive_tier_t tier);
const char *hu_sensitive_category_str(hu_sensitive_category_t cat);

/* Default tier for a category, so a config block that declares a value
 * without an explicit tier still lands in the right bucket. */
hu_sensitive_tier_t hu_sensitive_category_default_tier(hu_sensitive_category_t cat);

/* ── The protected set ────────────────────────────────────────────────── */

/* One protected value. `value` is borrowed — the provider owns the storage
 * and must outlive the scan (in production it is the parsed config, which
 * lives as long as the daemon). */
typedef struct hu_sensitive_value {
    const char *value;
    size_t value_len;
    hu_sensitive_tier_t tier;
    hu_sensitive_category_t category;
} hu_sensitive_value_t;

typedef struct hu_sensitive_set {
    const hu_sensitive_value_t *values;
    size_t count;
} hu_sensitive_set_t;

/* What a scan found. `secret_kind` is meaningful only when category is
 * HU_SENSITIVE_CAT_HARD_SECRET. Deliberately carries NO copy of the matched
 * text — callers log categories, never values. */
typedef struct hu_sensitive_finding {
    hu_sensitive_tier_t tier;
    hu_sensitive_category_t category;
    hu_hard_secret_kind_t secret_kind;
} hu_sensitive_finding_t;

/* ── The pure predicate ───────────────────────────────────────────────── */

/* Does `text` disclose a protected value?
 *
 * Extracted pure per .claude/rules/security-predicate-extraction.md: no
 * allocator, no agent, no pipeline, no I/O — so the whole truth table
 * (including the false-positive cases that matter most) is pinned by unit
 * tests that construct a set literal and call this directly.
 *
 * `set` may be NULL or empty: hard-secret SHAPES are still detected (they
 * need no declared value), and the address rule degrades to a no-op. That is
 * the deliberate fail-open choice for an unconfigured deployment — see the
 * one-shot operator log in the stage.
 *
 * When several values match, the HIGHEST tier wins (NEVER_SEND over
 * TRUST_GATED) so a caller acting only on tier never under-reacts.
 *
 * Returns true and fills *out when something is disclosed; returns false and
 * zeroes *out otherwise. `out` may be NULL if the caller only wants the bool. */
bool hu_sensitive_scan(const char *text, size_t text_len, const hu_sensitive_set_t *set,
                       hu_sensitive_finding_t *out);

/* Normalize for matching: lowercase, non-alphanumerics collapsed to single
 * spaces, and digit-group separators dropped so "$1,250" and "1250" agree.
 * Exposed because the normalization IS half the contract — tests pin it
 * directly rather than inferring it from scan outcomes. Returns the written
 * length (always NUL-terminated; truncated if `cap` is too small). */
size_t hu_sensitive_normalize(const char *src, size_t src_len, char *dst, size_t cap);

/* Reduce a street address to its matchable core: house number + street name +
 * street-type token + optional directional, with unit/apartment designators
 * dropped. This is what makes "4341 34th St S" and
 * "4341 34TH ST S APT 652" match each other in both directions.
 *
 * Writes the normalized core to `dst` and returns its length, or 0 when
 * `src` has no address shape at all (no house number followed by a street
 * type). A declared STREET_ADDRESS value with no extractable core is SKIPPED
 * by the scan rather than falling back to a substring match — falling back
 * would make a mis-declared value like "waterfront place" fire on the
 * owner's real messages. */
size_t hu_sensitive_address_core(const char *src, size_t src_len, char *dst, size_t cap);

/* ── Protected-set provider ───────────────────────────────────────────── */

/* Supplies the protected set at send time. Production wires this to the
 * parsed `privacy` config block; tests inject a literal set. The callback
 * returns a BORROWED set whose storage outlives the call.
 *
 * Same pluggable shape as hu_outbound_crosstalk_set_lookup: the stage cannot
 * reach config from inside the pipeline, and hardcoding values in C would rot
 * the moment the owner moves house.
 *
 * Return 0 on success (with *out_set set, possibly to an empty set), -1 on
 * failure. Process-wide, not per-pipeline. NULL clears it. */
typedef int (*hu_sensitive_provider_fn_t)(void *userdata, const hu_sensitive_set_t **out_set);

void hu_outbound_sensitive_set_provider(hu_sensitive_provider_fn_t fn, void *userdata);

/* Resolve the currently-registered set. Returns NULL when no provider is
 * wired (the scan then sees hard-secret shapes only). */
const hu_sensitive_set_t *hu_outbound_sensitive_current_set(void);

/* Register the parsed `privacy` config block as the protected set
 * (src/agent/outbound/sensitive_config.c). Call once during startup, after
 * config load. Passing NULL clears the provider.
 *
 * The values are BORROWED from the config's arena, so the config must outlive
 * the registration — clear it before hu_config_deinit. */
struct hu_config;
void hu_outbound_sensitive_register_config(const struct hu_config *cfg);

/* ── Activation gate ──────────────────────────────────────────────────── */

typedef enum hu_sensitive_mode {
    HU_SENSITIVE_MODE_OFF = 0,
    HU_SENSITIVE_MODE_SHADOW = 1, /* DEFAULT — log would-block, send unchanged */
    HU_SENSITIVE_MODE_LIVE = 2,
} hu_sensitive_mode_t;

/* Resolve from HU_OUTBOUND_SENSITIVE (cached after first call).
 * Unset or unrecognized → SHADOW. "off" and "live" are the only overrides. */
hu_sensitive_mode_t hu_outbound_sensitive_mode(void);

/* The deflection used when a regenerate has already been spent and the reply
 * STILL discloses. Deliberately in-voice and content-free: it neither repeats
 * the protected value nor announces that a filter fired. */
#define HU_SENSITIVE_DEFLECTION "not something i want to put in a text — ask me another way"

/* ── Reactive-path entry point ────────────────────────────────────────── */

/* In-place check for call sites that BYPASS the outbound pipeline — namely
 * the reactive daemon send path, which runs its own ordered mutator chain
 * (src/daemon/daemon_shape.c) and therefore never reaches the pipeline stage.
 * Without this, the path that answers live inbound texts would be the ONLY
 * unprotected one, while the feature looked wired.
 *
 * There is no LLM in reach here, so there is no regenerate step: a LIVE
 * NEVER_SEND finding is replaced outright with HU_SENSITIVE_DEFLECTION. The
 * pipeline stage keeps the full regenerate-then-deflect ladder.
 *
 * `buf` must have capacity `cap`; the deflection is written only if it fits,
 * and the message is truncated to empty if it does not (fail closed — never
 * send a partially-scrubbed body). Returns the new length. No-op when OFF, or
 * in SHADOW beyond a log line. */
size_t hu_outbound_sensitive_apply_inplace(char *buf, size_t len, size_t cap);

#if HU_IS_TEST
/* Override the cached mode (tests only). Pass -1 to re-read the env. */
void hu_outbound_sensitive_set_mode_for_test(int mode);
#endif

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_OUTBOUND_SENSITIVE_H */
