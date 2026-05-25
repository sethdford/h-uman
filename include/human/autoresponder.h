/* include/human/autoresponder.h
 *
 * Persona-aware autoresponder — Sprint B Story 3 (2026-05-19).
 *
 * Goal: when the user is in DND AND incoming message is from an
 * allowlisted contact, generate a reply in the user's voice — using
 * the current persona + recent style metrics + minimal context.
 *
 * Anti-goals (from backlog Story 3):
 *   - NEVER auto-reply on first contact with someone (allowlist gate)
 *   - NEVER include the user's location, calendar, or other sensitive
 *     personal info
 *   - NEVER claim to BE the user ("hey it's Seth"); always frame as
 *     the user's assistant ("hey, this is Seth's assistant")
 *
 * Design split (mirrors predictive_drafts):
 *
 *   1. Pure decision predicate — `hu_autoresponder_should_respond`
 *      decides whether to reply at all. Takes the config struct +
 *      contact handle + current time. No I/O. Easy to test under
 *      every config combination.
 *
 *   2. Pure response template — `hu_autoresponder_build_prompt`
 *      assembles the LLM prompt deterministically. No I/O.
 *
 *   3. End-to-end generator — `hu_autoresponder_generate_reply`
 *      calls the provider + applies safety post-processing.
 *      Returns HU_ERR_NOT_SUPPORTED when no provider is configured.
 *
 *   4. Logging — `hu_autoresponder_log_reply` writes one line to
 *      ~/.human/autoresponder.log per reply for daily-digest review.
 */
#ifndef HU_AUTORESPONDER_H
#define HU_AUTORESPONDER_H

#include "human/core/allocator.h"
#include "human/core/error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HU_AUTORESPONDER_MAX_ALLOWLIST         32
#define HU_AUTORESPONDER_MAX_SCHEDULES         8
#define HU_AUTORESPONDER_HANDLE_MAX            128
#define HU_AUTORESPONDER_REPLY_MAX             512
#define HU_AUTORESPONDER_LOG_LINE_MAX          1024
#define HU_AUTORESPONDER_DEFAULT_USER_NAME_MAX 64

/* Day-of-week mask. Sunday=bit0, Saturday=bit6. 0x7F = every day. */
#define HU_DOW_MASK_DAILY    0x7F
#define HU_DOW_MASK_WEEKDAYS 0x3E /* Mon-Fri */
#define HU_DOW_MASK_WEEKENDS 0x41 /* Sun+Sat */

/* A single DND window in local time. start_minute_of_day < end_minute_of_day
 * defines a normal range; start > end means the window WRAPS midnight
 * (e.g. 22:00 → 07:00 means "evening + early morning"). */
typedef struct hu_dnd_schedule {
    int16_t start_minute_of_day; /* 0..1439 */
    int16_t end_minute_of_day;   /* 0..1439 */
    uint8_t days_of_week_mask;   /* bit0=Sun, bit6=Sat */
} hu_dnd_schedule_t;

typedef struct hu_autoresponder_config {
    bool enabled;
    char allowlist[HU_AUTORESPONDER_MAX_ALLOWLIST][HU_AUTORESPONDER_HANDLE_MAX];
    size_t allowlist_count;
    hu_dnd_schedule_t dnd_schedule[HU_AUTORESPONDER_MAX_SCHEDULES];
    size_t schedule_count;
    /* Display name to use in the canned "this is X's assistant" framing.
     * Empty → "the user's assistant" (anonymous fallback). */
    char user_display_name[HU_AUTORESPONDER_DEFAULT_USER_NAME_MAX];
    /* Path to the per-line log file. NULL or empty → default
     * (~/.human/autoresponder.log). */
    char log_path[256];
} hu_autoresponder_config_t;

struct hu_personal_model;

/* ── pure decision predicate ──────────────────────────────────────────
 *
 * Returns true iff ALL of:
 *   - cfg->enabled is true
 *   - contact_handle case-insensitively matches at least one allowlist
 *     entry (NOT a prefix match — exact only; prevents
 *     "+1555alice" matching "+15550000000")
 *   - now_unix falls within at least one configured DND schedule (in
 *     local time, computed from tz_offset_seconds)
 *
 * tz_offset_seconds is the LOCAL offset from UTC (negative for West).
 * Pass 0 to interpret schedule times as UTC.
 *
 * Returns false on any NULL pointer. */
bool hu_autoresponder_should_respond(const hu_autoresponder_config_t *cfg,
                                     const char *contact_handle, int64_t now_unix,
                                     int32_t tz_offset_seconds);

/* True iff `now_unix` falls inside any of cfg's DND schedules. Pure;
 * exposed for unit testing the schedule predicate in isolation. */
bool hu_autoresponder_in_dnd_window(const hu_autoresponder_config_t *cfg, int64_t now_unix,
                                    int32_t tz_offset_seconds);

/* True iff handle exactly (case-insensitive) matches an allowlist
 * entry. Pure; exposed for unit testing. */
bool hu_autoresponder_handle_allowlisted(const hu_autoresponder_config_t *cfg,
                                         const char *contact_handle);

/* ── pure prompt builder ──────────────────────────────────────────────
 *
 * Assembles the LLM prompt deterministically. Caller-owned buffer.
 * Writes a system+user pair as a single prompt string with stable
 * section markers. NUL-terminated; returns bytes written.
 *
 * The prompt instructs the model to:
 *   - Reply in the user's persona voice
 *   - Frame as the user's assistant ("hey, this is %s's assistant")
 *   - NEVER claim to be the user
 *   - NEVER mention location/calendar/sensitive info
 *   - Keep response under HU_AUTORESPONDER_REPLY_MAX chars
 *
 * persona_summary may be NULL/empty (no persona signal yet).
 * contact_model may be NULL (no per-contact context yet); if provided,
 * injects top-3 facts as "Contact insights:" section. */
size_t hu_autoresponder_build_prompt(const hu_autoresponder_config_t *cfg,
                                     const char *contact_handle, const char *channel,
                                     const char *incoming_text, const char *persona_summary,
                                     const struct hu_personal_model *contact_model, char *out,
                                     size_t cap);

/* ── post-processing: strip dangerous phrasing ───────────────────────
 *
 * Scans the generated text for patterns that violate the anti-goals
 * (e.g. "hey it's <name>" without the "'s assistant" framing). When a
 * violation is found, replaces the line with a safe canned reply.
 *
 * Returns: bytes written (excluding NUL). Writes a non-empty fallback
 * when input is empty/NULL.
 *
 * Pure; safe for unit testing. */
size_t hu_autoresponder_sanitize_reply(const hu_autoresponder_config_t *cfg, const char *raw_text,
                                       char *out, size_t cap);

/* ── end-to-end: generate + sanitize + log ───────────────────────────
 *
 * Calls hu_provider_create_default, runs the prompt, sanitizes the
 * reply, writes a log line, and returns the sanitized reply in `out`.
 *
 * Returns:
 *   HU_OK                — success
 *   HU_ERR_INVALID_ARGUMENT — required arg missing
 *   HU_ERR_PERMISSION_DENIED — should_respond returned false
 *   HU_ERR_NOT_SUPPORTED — no provider configured
 *   HU_ERR_IO             — provider call or log-write failed */
hu_error_t hu_autoresponder_generate_reply(hu_allocator_t *alloc,
                                           const hu_autoresponder_config_t *cfg,
                                           const struct hu_personal_model *model,
                                           const char *contact_handle, const char *channel,
                                           const char *incoming_text, int64_t now_unix,
                                           int32_t tz_offset_seconds, char *out, size_t cap);

/* Append one timestamped JSON line to ~/.human/autoresponder.log (or
 * cfg->log_path when set). Format:
 *   {"ts":<unix>,"contact":"<handle>","channel":"<name>","reply":"<text>"}
 * Strings are JSON-escaped (quotes, backslashes, control chars). */
hu_error_t hu_autoresponder_log_reply(const hu_autoresponder_config_t *cfg,
                                      const char *contact_handle, const char *channel,
                                      const char *reply_text, int64_t now_unix);

/* Load an autoresponder config from a JSON file (typically
 * ~/.human/autoresponder.json). The file format mirrors the struct:
 *
 *   {
 *     "enabled": true,
 *     "user_display_name": "Seth",
 *     "allowlist": ["+15551234567", "alice@example.com"],
 *     "schedules": [
 *       { "start": "22:00", "end": "07:00", "days": "daily" }
 *     ],
 *     "log_path": "/Users/seth/.human/autoresponder.log"
 *   }
 *
 * Time strings are "HH:MM" (24h, local). "days" accepts: "daily",
 * "weekdays", "weekends", or a comma-separated list like "mon,wed,fri".
 *
 * Returns:
 *   HU_OK              — config populated
 *   HU_ERR_NOT_FOUND   — file does not exist (out is zeroed, enabled=false)
 *   HU_ERR_PARSE       — file exists but isn't valid JSON / shape
 *   HU_ERR_IO          — file exists but read failed */
hu_error_t hu_autoresponder_config_load_from_file(const char *path, hu_autoresponder_config_t *out);

/* CLI entry: `human autoresponder digest [--since-hours N] [--log <path>]`.
 * Reads the rolling JSON log (default ~/.human/autoresponder.log) and
 * prints a summary of replies sent in the last N hours (default 24).
 *
 * Returns HU_OK on success (even when the log file doesn't exist —
 * "0 replies in the last N hours" is a valid result). */
hu_error_t cmd_autoresponder(hu_allocator_t *alloc, int argc, char **argv);

/* Pure helpers exposed for unit testing:
 *
 * Aggregate one JSON-line log into counts. `now_unix` and `since_seconds`
 * gate which lines are counted. Returns total_lines_in_window; populates
 * out_contact_count via the optional `out_contact_counts` array (caller
 * supplies a small bounded array of {handle, count} pairs). */
typedef struct hu_autoresponder_digest_contact {
    char handle[HU_AUTORESPONDER_HANDLE_MAX];
    int32_t count;
} hu_autoresponder_digest_contact_t;

typedef struct hu_autoresponder_digest {
    int32_t total_replies;
    hu_autoresponder_digest_contact_t per_contact[HU_AUTORESPONDER_MAX_ALLOWLIST];
    size_t per_contact_count;
} hu_autoresponder_digest_t;

/* Pure: aggregate log content from `body` (NUL-terminated) into `out`.
 * Lines that don't parse as the expected JSON shape are silently
 * skipped — robust to log-format drift. */
void hu_autoresponder_digest_aggregate(const char *body, int64_t now_unix, int64_t since_seconds,
                                       hu_autoresponder_digest_t *out);

#ifdef __cplusplus
}
#endif
#endif /* HU_AUTORESPONDER_H */
