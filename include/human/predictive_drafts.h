/* include/human/predictive_drafts.h
 *
 * Predictive draft suggestions — Sprint 1 Story 1 (2026-05-19).
 *
 * Goal: when the user opens a thread with a contact, generate the N
 * most-likely draft messages they'd want to send next, given:
 *   - The user's persona summary (hu_personal_model_build_prompt)
 *   - A short summary of recent messages with that contact
 *   - The per-contact reaction signature
 *     (hu_calib_reaction_signature_from_model)
 *
 * Two-layer API mirroring contact_signature / imessage_gaps:
 *
 *   1) Pure prompt-builder (`hu_predictive_drafts_build_prompt`) — no
 *      LLM, no I/O. Testable on every build. Pins the wire format the
 *      generator hands to the local provider.
 *
 *   2) End-to-end generator (`hu_predictive_drafts_generate`) — calls
 *      the configured default provider and parses N drafts back out.
 *      Returns HU_ERR_NOT_SUPPORTED when no provider is configured,
 *      HU_ERR_NOT_FOUND when there's no signal to ground generation.
 */

#ifndef HU_PREDICTIVE_DRAFTS_H
#define HU_PREDICTIVE_DRAFTS_H

#include "human/core/allocator.h"
#include "human/core/error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Hard caps. Drafts are short by contract — 60 tokens ≈ 240 bytes.
 * The 512-byte slot gives headroom for UTF-8 multi-byte chars. */
#define HU_PREDICTIVE_DRAFT_TEXT_MAX      512
#define HU_PREDICTIVE_DRAFT_RATIONALE_MAX 128
#define HU_PREDICTIVE_DRAFT_MAX_N         8
#define HU_PREDICTIVE_DRAFT_DEFAULT_N     3
#define HU_PREDICTIVE_DRAFT_PROMPT_MAX    8192

struct hu_personal_model;

typedef struct hu_predictive_draft {
    char text[HU_PREDICTIVE_DRAFT_TEXT_MAX];
    float confidence; /* 0.0-1.0, self-reported by the model */
    char rationale[HU_PREDICTIVE_DRAFT_RATIONALE_MAX];
} hu_predictive_draft_t;

typedef struct hu_predictive_draft_set {
    hu_predictive_draft_t drafts[HU_PREDICTIVE_DRAFT_MAX_N];
    size_t draft_count;
} hu_predictive_draft_set_t;

/* Build the deterministic prompt string the generator will hand to the
 * LLM. Pure function — no I/O, no allocation, no provider calls.
 *
 * Inputs:
 *   contact_handle           — required; identifies the recipient. Long
 *                              handles are truncated to a safe limit.
 *   channel                  — optional ("imessage", "slack", etc.). NULL
 *                              or empty → omitted from the prompt.
 *   persona_summary          — optional. Typically the bytes written by
 *                              hu_personal_model_build_prompt. NULL or
 *                              empty → a "no persona signal yet" line.
 *   recent_messages_summary  — optional. Caller-supplied summary of the
 *                              last N messages with this contact. NULL
 *                              or empty → "no recent history" line.
 *   reaction_signature_summary — optional. Caller-supplied summary of
 *                              the contact's reaction patterns. NULL or
 *                              empty → omitted.
 *   n                        — number of drafts to request (1..MAX_N).
 *                              Values outside the range are clamped.
 *
 * Output:
 *   `out` is NUL-terminated when `out_cap > 0`. Returns bytes written
 *   (excluding NUL), or 0 if `out` is NULL or `out_cap` < 2.
 *
 * The prompt asks the model for STRICT JSON of the form:
 *   {"drafts":[{"text":"...","confidence":0.7,"rationale":"..."}]}
 * The shape is stable so the parser in
 * `hu_predictive_drafts_generate` can rely on it. */
size_t hu_predictive_drafts_build_prompt(const char *contact_handle, const char *channel,
                                         const char *persona_summary,
                                         const char *recent_messages_summary,
                                         const char *reaction_signature_summary, size_t n,
                                         char *out, size_t out_cap);

/* Render a one-line summary of a reaction signature derived from the
 * personal model. Stable, deterministic, no I/O. Suitable as the
 * `reaction_signature_summary` argument to build_prompt.
 *
 * Output shape example:
 *   "Alice loves: hiking, climbing (3 positive)."
 *
 * Returns bytes written (excluding NUL). Writes "" on NULL inputs or
 * when the model carries no reaction-derived facts. */
size_t hu_predictive_drafts_render_signature(const struct hu_personal_model *model,
                                             const char *contact_handle, char *buf, size_t cap);

/* End-to-end generation. Loads the user's default provider via
 * hu_provider_create_default and asks for N drafts. Returns:
 *   HU_OK                — success; *out is populated with draft_count drafts.
 *   HU_ERR_NOT_SUPPORTED — no provider configured (or no chat method).
 *   HU_ERR_NOT_FOUND     — no recent-history signal and no persona signal:
 *                          generating without grounding would hallucinate.
 *   HU_ERR_INVALID_ARGUMENT — required argument NULL/invalid.
 *   HU_ERR_PARSE         — provider returned malformed JSON.
 *
 * `recent_messages_summary` may be NULL when the caller doesn't have a
 * chat.db reader available; in that case grounding is supplied entirely
 * by the personal model + reaction signature. */
hu_error_t hu_predictive_drafts_generate(hu_allocator_t *alloc,
                                         const struct hu_personal_model *model,
                                         const char *contact_handle, const char *channel,
                                         const char *recent_messages_summary, size_t n,
                                         hu_predictive_draft_set_t *out);

/* Parse provider output into a draft set. Public for tests.
 *
 * Accepts either the strict JSON shape requested by `_build_prompt`
 * (`{"drafts":[{"text":"...","confidence":0.7,"rationale":"..."}]}`)
 * or a numbered-list fallback ("1. hey\n2. wanna grab coffee\n3. ...")
 * where each line is taken as a draft text with neutral confidence
 * and an empty rationale.
 *
 * Returns HU_OK with `out->draft_count > 0` when at least one draft
 * was extracted. Returns HU_ERR_PARSE when no draft survives parsing. */
hu_error_t hu_predictive_drafts_parse_response(const char *response, size_t response_len,
                                               hu_predictive_draft_set_t *out);

/* Optional provider-name override for `hu_predictive_drafts_generate`.
 * When set to a non-empty string, generate ignores the configured
 * default_provider and uses this one instead — useful when the user's
 * local MLX server is down and they want to fall back to a cloud
 * provider for one CLI invocation. Pass NULL or "" to clear. Stored
 * in a file-scope static, NOT thread-local; safe in the daemon's
 * single-threaded event loop but should not be invoked concurrently. */
void hu_predictive_drafts_set_provider_override(const char *provider_name);

/* CLI entry: `human drafts --contact <handle> [--channel <name>] [--n N]
 * [--provider <name>]`. Reads the user's config + personal_model file,
 * runs the generator, prints each draft to stdout. */
hu_error_t cmd_drafts(hu_allocator_t *alloc, int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* HU_PREDICTIVE_DRAFTS_H */
