/* Response guard — last-mile sanitizer + degenerate-output detector.
 *
 * Sits between the provider's raw output and the channel's send. Catches
 * two classes of failure that no other gate (verifier, quality, Turing,
 * constitutional) protects against:
 *
 *   1. Special-token leaks. Provider returns text containing model
 *      internals like Harmony channel markers (<|channel|>, <|message|>,
 *      <|thought|>, <|start|>, <|end|>, <|return|>, <|user|>, <|assistant|>,
 *      <|system|>, <|tool_call|>, <|tool_response|>) or generic <|...|>
 *      tokens, or thinking blocks (<think>...</think>, <thought>...</thought>).
 *      These reach the wire when the inference server's stop-token /
 *      detokenizer config is wrong, OR when the model emits its own header
 *      mid-response. The guard strips them.
 *
 *   2. Degenerate repetition. Classic temperature/penalty failure where
 *      the model emits the same token / short pattern hundreds of times
 *      (e.g.  '" " " " " " " " ...'). The guard detects this and rejects
 *      the response so the daemon can either retry or suppress.
 *
 * Production root-cause: 2026-05-10 leak to a real human contact via the
 * apple-foundationmodel reflexive-tier provider. See:
 *   docs/postmortems/2026-05-10-response-guard.md (post-incident write-up)
 */
#ifndef HU_AGENT_RESPONSE_GUARD_H
#define HU_AGENT_RESPONSE_GUARD_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    /* Response was clean — no rewrites, safe to send as-is. */
    HU_GUARD_OK = 0,
    /* Response was rewritten in place (special tokens stripped, etc.).
     * `*out_response` and `*out_len` are updated; caller should free the
     * old response (if it was allocator-owned) and use the new one. */
    HU_GUARD_REWROTE = 1,
    /* Response is degenerate / unsendable. Caller MUST NOT send it.
     * `*out_response` is set to NULL and `*out_len` to 0. */
    HU_GUARD_REJECT = 2,
} hu_guard_outcome_t;

typedef struct {
    /* What the guard found / changed (for logging + telemetry). */
    bool stripped_harmony_tokens;
    bool stripped_thinking_block;
    bool detected_degenerate_repetition;
    /* Sprint 29 — semantic leak class. Set when the response was rejected
     * because it contained a chain-of-thought / prompt-context dump that
     * earlier phases couldn't catch (no markup, no repetition):
     *   G1. Numbered analytical-list dump (≥ 3 long numbered items).
     *   G2. Model self-talk / scene-direction echo ("the prompt says",
     *       "Wait, the prompt", "I should still maintain", etc.).
     *   G3. Third-person-about-the-user double-pattern ("[Name] is a
     *       [profession]", "He's talking to ...", "lives alone with ...",
     *       etc. — ≥ 2 distinct hits).
     * Sprint 30 added:
     *   G4. Prompt-template label leak ("Persona:", "Scene Direction:",
     *       "User: \"", "Rules: All lowercase", etc.).
     * See `docs/postmortems/2026-05-12-cot-leak.md`. */
    bool detected_semantic_leak;
    /* Sprint 31 — context-aware detections. These ONLY fire when the
     * caller used `hu_response_guard_check_ex` with a non-NULL context.
     *
     * G5. Length anomaly. Set when response_len > recent_avg_len *
     *     HU_GUARD_LENGTH_ANOMALY_MULT (default 8x). Caught the
     *     2026-05-12 Brea leak (979 chars vs 44 char rolling avg = 22x)
     *     post-hoc.
     * G6. Director echo. Set when a 30+ char substring of the
     *     director's scene-direction text appears verbatim in the
     *     response. Caught the same Brea leak (which quoted
     *     "Professional, slightly skeptical, ask for clarification
     *     on why they"). */
    bool detected_length_anomaly;
    bool detected_director_echo;
    /* Sprint 35 — persona PII echo. Set when `ctx->persona_name` is
     * non-NULL and the response contains the persona name in a
     * third-person profile construct (e.g. `"<Name> is a"`,
     * `"<Name>'s job"`, `"<Name> lives"`, `"<Name> works"`). Catches
     * the 2026-05-11 leak class where the model echoed the operator's
     * loaded persona name back to the recipient. */
    bool detected_persona_pii_echo;
    /* Sprint 36 — persona identity / core-anchor echo. Set when
     * `ctx->persona_identity` is non-NULL (≥ 25 bytes) and a 25-byte
     * verbatim substring of it appears in the response (case-
     * insensitively). Catches first-person identity leaks like
     * `"i'm a Chief Architect at Pure Health Solutions"` that G7
     * cannot catch (no name in third-person construct). */
    bool detected_persona_identity_echo;
    /* If rejected, the longest run length that triggered rejection. */
    size_t max_repetition_run;
    /* Number of bytes removed by sanitization (0 if rejected outright). */
    size_t bytes_stripped;
} hu_guard_report_t;

/* Sprint 31 — optional per-turn context for context-aware leak
 * detections. Pass to `hu_response_guard_check_ex`. NULL means "no
 * context available" — the function behaves identically to
 * `hu_response_guard_check`. */
typedef struct {
    /* Rolling average reply length from the recipient over the last
     * N messages. The guard rejects if `response_len >
     * recent_avg_len * HU_GUARD_LENGTH_ANOMALY_MULT` (default 8x).
     * 0 disables the check (e.g. no chat history yet). */
    size_t recent_avg_len;

    /* The director's / scene-direction text for this turn (the
     * upstream prompt fragment that drove tone/style decisions).
     * The guard rejects if a 30+ char substring of director_text
     * appears verbatim (case-insensitively) in the response. NULL
     * or director_len < 30 disables the check. */
    const char *director_text;
    size_t director_len;

    /* Sprint 37 — past-turn director history (most-recent-first). G6
     * iterates these after the current `director_text` to catch model
     * output that quotes a *previous* turn's director rather than the
     * current one. Same 30-byte minimum match. NULL or
     * director_history_count == 0 disables the cross-turn check. */
    const char *const *director_history;
    const size_t *director_history_lens;
    size_t director_history_count;

    /* Sprint 35 — loaded persona's name (e.g. `"Seth"`). The guard
     * rejects if this name appears in a third-person profile
     * construct (e.g. `"<Name> is a"`, `"<Name>'s job"`,
     * `"<Name> lives"`). NULL or persona_name_len < 2 disables
     * the check. Word-boundary aware — `"Bethseth"` does NOT match
     * `"Seth"`. */
    const char *persona_name;
    size_t persona_name_len;

    /* Sprint 36 — loaded persona's `identity` (or `core_anchor` as
     * fallback). Free-form biographical string, e.g. `"51-year-old
     * technical professional, lives alone with a cat"`. The guard
     * rejects if any contiguous 25-byte substring of this string
     * appears verbatim (case-insensitively) in the response.
     * Catches first-person identity leaks where the model quotes
     * persona context back to the recipient without using the
     * name. NULL or persona_identity_len < 25 disables the check. */
    const char *persona_identity;
    size_t persona_identity_len;
} hu_guard_context_t;

/* Run the guard over a response.
 *
 * Inputs:
 *   alloc      — for the rewrite buffer (only consulted if the response
 *                needs rewriting). Required.
 *   response   — raw response bytes from the provider. NOT freed by this
 *                function; caller retains ownership.
 *   response_len — length in bytes (NUL-terminator not required).
 *
 * Outputs (always written):
 *   *out_response — pointer to the response to send:
 *                   - HU_GUARD_OK:      same as `response` (no copy).
 *                   - HU_GUARD_REWROTE: newly allocated, allocator-owned,
 *                                       NUL-terminated; caller frees with
 *                                       `alloc->free(.., out_response,
 *                                       (*out_len) + 1)`.
 *                   - HU_GUARD_REJECT:  NULL.
 *   *out_len      — length of *out_response in bytes (excl. NUL).
 *   *out_outcome  — what happened.
 *   report        — optional; if non-NULL, populated with details.
 *
 * Returns HU_OK unless arguments are invalid or allocation failed. The
 * outcome enum (not the error code) tells the caller what to do. */
hu_error_t hu_response_guard_check(hu_allocator_t *alloc,
                                   const char *response, size_t response_len,
                                   char **out_response, size_t *out_len,
                                   hu_guard_outcome_t *out_outcome,
                                   hu_guard_report_t *report);

/* Context-aware variant. Same as `hu_response_guard_check` but accepts
 * an optional `ctx` with rolling-average reply length and the director's
 * scene-direction text. Enables Sprint 31's context-aware detections:
 *
 *   G5 (length anomaly): if `ctx->recent_avg_len > 0` and `response_len
 *       > ctx->recent_avg_len * HU_GUARD_LENGTH_ANOMALY_MULT` (default
 *       8x), REJECT.
 *   G6 (director echo): if `ctx->director_text` is non-NULL and at
 *       least 30 chars long, scan the response for a verbatim 30+ char
 *       substring of director_text. If found, REJECT.
 *
 * `ctx == NULL` is identical to `hu_response_guard_check` — no
 * context-aware detections run. */
hu_error_t hu_response_guard_check_ex(hu_allocator_t *alloc,
                                      const char *response, size_t response_len,
                                      const hu_guard_context_t *ctx,
                                      char **out_response, size_t *out_len,
                                      hu_guard_outcome_t *out_outcome,
                                      hu_guard_report_t *report);

/* Lower-level helpers, exposed for testing. ────────────────────────────── */

/* Return true if `s[0..len)` contains a known Harmony-format channel
 * marker or any <|...|> special token. */
bool hu_response_guard_has_special_token(const char *s, size_t len);

/* Return the longest run of the same single character in `s[0..len)`.
 * Whitespace, punctuation, alphanumerics — all counted equally. */
size_t hu_response_guard_longest_char_run(const char *s, size_t len);

/* Return the longest number of times any short token (1..8 chars,
 * delimited by whitespace) repeats consecutively in `s[0..len)`. */
size_t hu_response_guard_longest_token_run(const char *s, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_RESPONSE_GUARD_H */
