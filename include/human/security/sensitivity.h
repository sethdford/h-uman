#ifndef HU_SECURITY_SENSITIVITY_H
#define HU_SECURITY_SENSITIVITY_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Content sensitivity classification (inspired by EdgeClaw S1/S2/S3 tiers).
 *
 * S1 (Safe)      — no sensitive content; route to any provider including cloud.
 * S2 (Sensitive)  — contains PII or semi-private data; log warning, prefer local.
 * S3 (Private)    — contains secrets, keys, or highly private data; local-only.
 */

typedef enum hu_sensitivity_level {
    HU_SENSITIVITY_S1 = 1, /* safe — cloud OK */
    HU_SENSITIVITY_S2 = 2, /* sensitive — PII detected, prefer local */
    HU_SENSITIVITY_S3 = 3  /* private — secrets/keys, local-only */
} hu_sensitivity_level_t;

typedef struct hu_sensitivity_result {
    hu_sensitivity_level_t level;
    const char *reason; /* static string describing why; NULL if S1 */
    float confidence;   /* 0.0-1.0: how confident we are in the classification */
    int signal_count;   /* number of independent signals that triggered this level */
} hu_sensitivity_result_t;

/* Classify a user message for data sensitivity using rule-based detection.
 * Checks keywords, regex-like patterns (SSN, credit card, private keys),
 * and file path patterns. Returns S1 if no sensitive content found. */
hu_sensitivity_result_t hu_sensitivity_classify_message(const char *msg, size_t msg_len);

/* Classify a file path for sensitivity.
 * Paths to SSH keys, .env files, credentials, etc. trigger S3. */
hu_sensitivity_result_t hu_sensitivity_classify_path(const char *path, size_t path_len);

/* Classify a tool name for sensitivity.
 * Tools that access secrets or credentials trigger S3. */
hu_sensitivity_result_t hu_sensitivity_classify_tool(const char *tool_name, size_t tool_len);

/* ── Hard-secret SHAPE detection (no keyword layer) ───────────────────── */

/* Kinds of value whose SHAPE alone is proof enough to never transmit.
 * Deliberately excludes everything keyword-driven: `hu_sensitivity_classify_message`
 * treats "salary", "phone number", any email, and any 10-15 digit run as
 * sensitive, which is the right posture for ROUTING an inbound message to a
 * local model but catastrophic as an OUTBOUND block — the owner says those
 * things in ordinary conversation. The kinds below have no legitimate reason
 * to appear in a text message the owner sends, so they are safe to act on. */
typedef enum hu_hard_secret_kind {
    HU_HARD_SECRET_NONE = 0,
    HU_HARD_SECRET_SSN,         /* NNN-NN-NNNN (or space-separated) */
    HU_HARD_SECRET_CARD,        /* 13-19 digits, Luhn-valid */
    HU_HARD_SECRET_PRIVATE_KEY, /* -----BEGIN ... PRIVATE KEY----- */
    HU_HARD_SECRET_API_TOKEN,   /* sk-/ghp_/xoxb-/AKIA-prefixed credential */
} hu_hard_secret_kind_t;

/* Shape-only scan for never-transmit values. Single source of truth: the
 * outbound sensitive-disclosure stage and the S3 arm of
 * hu_sensitivity_classify_message both go through these predicates rather
 * than each re-implementing SSN/Luhn (src/agent/outbound/moderation.c
 * previously carried a weaker no-Luhn copy).
 *
 * Returns the first kind found, or HU_HARD_SECRET_NONE. Pure — no allocation,
 * no state, safe to call on untrusted bytes of any length. */
hu_hard_secret_kind_t hu_sensitivity_hard_secret_shape(const char *text, size_t len);

/* Stable lowercase identifier for logs/telemetry. Never NULL. Carries the
 * KIND only — never the matched bytes (see .claude/rules/quality-gates.md
 * "never log secrets"). */
const char *hu_sensitivity_hard_secret_kind_str(hu_hard_secret_kind_t kind);

/* Merge multiple sensitivity results, taking the highest level. */
hu_sensitivity_result_t hu_sensitivity_merge(const hu_sensitivity_result_t *a,
                                             const hu_sensitivity_result_t *b);

/* Returns true if the level requires local-only processing. */
bool hu_sensitivity_requires_local(hu_sensitivity_level_t level);

const char *hu_sensitivity_level_str(hu_sensitivity_level_t level);

#endif /* HU_SECURITY_SENSITIVITY_H */
