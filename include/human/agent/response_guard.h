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
#include <stdint.h>

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
    /* Sprint 41 (2026-05-26 Jordan incident) — naked discourse-marker
     * opener. Set when the response STARTS with a discourse-marker
     * filler ("tbh", "ngl", "imo", "honestly", "fr", "lowkey", ...)
     * directly followed by a bare greeting noun ("morning", "hey",
     * "yo", ...) with no completing clause. The pattern fires
     * because LoRA adapters trained on Seth's raw texting corpus
     * learn discourse markers as high-probability sentence openers
     * ("tbh that's annoying", "ngl idk") and then paste them onto
     * proactive greetings where they make zero pragmatic sense
     * ("tbh morning. you awake yet?" → "to-be-honest morning" is
     * not how humans speak — backchannel markers require something
     * to be honest ABOUT). See `hu_response_is_naked_discourse_opener`
     * for the exact predicate. */
    bool detected_naked_discourse_opener;
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
     * recent_avg_len * length_anomaly_mult` (default 8x; compact
     * channels 6x). 0 disables the check (e.g. no chat history yet). */
    size_t recent_avg_len;

    /* Sprint 39 — per-channel G5 multiplier. 0 = use default (8).
     * Compact channels (imessage, cli, sms) should pass 6 from
     * `hu_guard_length_anomaly_mult_for_channel`. */
    unsigned length_anomaly_mult;

    /* Task 10 (AC-9) — learned per-contact baseline length from the personal
     * model. When non-zero, G5 uses this as the baseline instead of
     * recent_avg_len (falling back to recent_avg_len if this is 0). Populated
     * from personal_model.style.avg_message_length when available. Enables
     * Seth-normal length to a given contact to bypass the length anomaly
     * detector (e.g. a contact Seth habitually sends 500-char msgs to can
     * receive 500-char replies without triggering G5). 0 disables this check
     * (falls back to recent_avg_len + multiplier). */
    size_t learned_avg_message_length;

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

    /* Sprint 38 — loaded persona's `biography` (long-form backstory).
     * Same 25-byte verbatim substring rule as `persona_identity`.
     * Checked independently so a leak quoting only biography (no
     * identity/core_anchor overlap) still trips G8. NULL or
     * persona_biography_len < 25 disables this source. */
    const char *persona_biography;
    size_t persona_biography_len;

    /* Sprint 41 follow-up — per-call escape hatch for G9 (naked
     * discourse-marker opener). Set to true when the calling channel
     * has legitimate short interjections that should bypass G9 (e.g.
     * a voice channel where "tbh morning" might be a valid backchannel
     * + greeting separated by a pause-token the text guard cannot see).
     * Default false → G9 runs. There is also a process-wide kill-switch
     * via `hu_response_guard_set_naked_opener_globally_disabled` for
     * operators who want to disable G9 across all channels without
     * touching every call site. */
    bool naked_opener_disabled;
} hu_guard_context_t;

/* Sprint 38 — cumulative REJECT counts by detector (process-wide).
 * Incremented atomically when `hu_response_guard_check_ex` returns
 * HU_GUARD_REJECT. Use `hu_guard_reject_stats_reset()` in tests
 * that assert on absolute counts. */
typedef struct {
    uint64_t semantic_leak;
    uint64_t length_anomaly;
    uint64_t director_echo;
    uint64_t persona_pii_echo;
    uint64_t persona_identity_echo;
    /* Sprint 41 — naked discourse-marker opener (G9). */
    uint64_t naked_discourse_opener;
    /* Sprint 41 follow-up #3 — G9 retry-outcome breakdown.
     *
     *   g9_retry_rescued  — G9 rejected → retry succeeded with a clean
     *                       response that does NOT trip G9 again. The
     *                       guard saved a real Jordan-class leak.
     *   g9_retry_thrashed — G9 rejected → retry ALSO tripped G9. The
     *                       LoRA is stuck in the bad opener pattern; the
     *                       fallback path (canned response) ships.
     *   g9_retry_starved  — G9 rejected → retry failed or returned empty,
     *                       so no message reaches the user via this turn.
     *
     * Operators read these to answer "is G9 helping or hurting?":
     *   - rescued / (rescued + thrashed + starved) ≈ rescue rate
     *   - thrashed > 5% → LoRA needs retraining with DPO negatives
     *   - starved > 1% → contact is silently underserved; tune marker list
     *     or disable G9 for that channel. */
    uint64_t g9_retry_rescued;
    uint64_t g9_retry_thrashed;
    uint64_t g9_retry_starved;
} hu_guard_reject_stats_t;

void hu_guard_reject_stats_snapshot(hu_guard_reject_stats_t *out);
void hu_guard_reject_stats_reset(void);

/* Default G5 multiplier (8×). Compact messaging channels use 6×. */
#define HU_GUARD_LENGTH_ANOMALY_MULT_DEFAULT 8u
#define HU_GUARD_LENGTH_ANOMALY_MULT_COMPACT 6u

/* G5 absolute floor. The length-anomaly check fires only when the
 * response exceeds BOTH this floor AND recent_avg_len × mult. Rationale:
 * the leak class G5 defends against (chain-of-thought / prompt-context
 * dumps — e.g. the 2026-05-12 leak at 979 chars) is intrinsically large
 * in ABSOLUTE terms. A normal-length human reply (a few sentences) can
 * never be such a dump regardless of how terse the recipient's recent
 * messages were. Without this floor, a collapsed rolling average (e.g.
 * 18 chars after a run of forced-short replies) makes a perfectly
 * natural 135-char reply look "anomalous" at 7.5× — rejecting it, which
 * triggers a repair-prompt retry that shortens the reply further, which
 * drops the average again: a death-spiral toward sub-human terseness.
 * 320 chars ≈ 2-3 long sentences, well above a typical iMessage and well
 * below the documented leak size, so it removes the spiral without
 * weakening dump detection. */
#define HU_GUARD_LENGTH_ANOMALY_FLOOR 320u

/* Channel-aware G5 threshold. imessage / cli / sms → 6×; else 8×. */
unsigned hu_guard_length_anomaly_mult_for_channel(const char *channel, size_t channel_len);

/* Sprint 40 — selection-step audit helpers (observability only).
 * Log when A/B or multi-candidate paths ship a response that would trip
 * G1/G2 so post-mortems can trace *why* a numbered candidate list leaked. */
bool hu_guard_audit_numbered_analysis_dump(const char *s, size_t len);
bool hu_guard_audit_self_talk_leak(const char *s, size_t len);

/* 2026-05-19 — critique-as-response echo detector.
 *
 * The reflection-retry loop appends an LLM-generated critique to the
 * conversation history before re-running chat completion. The LLM
 * occasionally echoes the critique structure back as its retry attempt,
 * producing a response that starts with one of the evaluator's verdict
 * tokens (NEEDS_RETRY / needs_retry). Those responses would otherwise
 * reach the user as the assistant reply.
 *
 * Pure predicate — easy to unit-test (see tests/test_response_guard.c).
 * Returns true iff the response is a critique echo. */
bool hu_response_is_critique_echo(const char *s, size_t len);

/* Sprint 41 (2026-05-26 Jordan incident) — naked discourse-marker opener.
 *
 * Returns true if `s[0..len)` STARTS with one of a small set of discourse-
 * marker fillers (tbh, ngl, imo, imho, fwiw, fr, frfr, honestly, lowkey,
 * highkey, lmao, lol, deadass) directly followed by a bare greeting noun
 * (morning, evening, afternoon, night, hey, hi, hello, sup, yo, hola,
 * howdy) with no completing clause. This catches the production class:
 *
 *   "tbh morning. you awake yet?"     -> REJECT (filler + greeting + EOS-ish)
 *   "ngl hey wanna grab coffee?"      -> REJECT (filler + greeting + EOS-ish)
 *
 * It does NOT fire when the greeting is followed by a copula-style verb
 * making the marker pragmatically valid:
 *
 *   "tbh morning is the worst"        -> OK ("to be honest, morning IS X")
 *   "tbh hey was a fun show"          -> OK
 *
 * Nor when the message contains no discourse marker:
 *
 *   "morning! you awake yet?"         -> OK (no naked marker)
 *
 * Pure predicate — easy to unit-test. Used as Phase 5 (G9) inside
 * hu_response_guard_check_ex. */
bool hu_response_is_naked_discourse_opener(const char *s, size_t len);

/* Sprint 41 follow-up — process-wide kill switch for G9. When set to true,
 * G9 is skipped for ALL callers regardless of per-call context. Use to
 * disable the detector at runtime without recompiling (e.g. operator
 * sees a false-positive burst at 3am and wants to silence the rule until
 * morning). Default false — G9 runs.
 *
 * Set via config wire-up at daemon startup (planned follow-up) or via
 * `human ctl response-guard-disable g9` (planned CLI follow-up). The
 * setter is intentionally simple to keep the kill-switch reachable.
 *
 * Thread-safe: atomic bool. */
void hu_response_guard_set_naked_opener_globally_disabled(bool disabled);
bool hu_response_guard_naked_opener_globally_disabled(void);

/* Sprint 41 follow-up #3 — record the outcome of a G9-triggered retry.
 *
 * Call this from the agent_turn / agent_stream retry path AFTER the
 * second response_guard_check has run on the retry result. Pass:
 *   - retry_succeeded: the retry produced a non-empty response that the
 *     daemon will actually send (HU_GUARD_OK or HU_GUARD_REWROTE on the
 *     retry).
 *   - retry_tripped_g9_again: the retry produced a response that ALSO
 *     trips G9 (caller checked via hu_response_is_naked_discourse_opener
 *     on the retry text OR observed HU_GUARD_REJECT with
 *     detected_naked_discourse_opener set).
 *
 * Pure function over (retry_succeeded, retry_tripped_g9_again):
 *   ( true,  false) → rescued
 *   ( true,  true)  → thrashed (sent but bad — should not happen if guard
 *                      is wired correctly; counted defensively)
 *   ( false, _    ) → starved
 *
 * No-op when neither input applies (e.g. the original response was OK).
 * Thread-safe atomic increments. */
void hu_response_guard_record_g9_retry_outcome(bool retry_succeeded, bool retry_tripped_g9_again);

/* Sprint 41 follow-up #4 — per-channel G9 disable list.
 *
 * Daemon snapshots config->response_guard.g9_disabled_channels at startup
 * via hu_response_guard_set_g9_disabled_channels. The list is then
 * consulted by hu_response_guard_g9_disabled_for_channel from the
 * guard_context construction sites (agent_turn / agent_stream) — when
 * the current channel matches, those sites set
 * ctx->naked_opener_disabled = true and G9 is bypassed for that call.
 *
 * Setter copies the channel names into a process-private array so the
 * caller's config strings can be freed independently. Passing NULL+0
 * clears the list. Thread-safe via atomic snapshot pointer. */
void hu_response_guard_set_g9_disabled_channels(const char *const *channels, size_t count);
bool hu_response_guard_g9_disabled_for_channel(const char *channel, size_t channel_len);

void hu_guard_log_selection_audit(const void *observer, const char *contact_key,
                                  size_t contact_key_len, size_t candidate_count, size_t best_idx,
                                  int best_quality, size_t response_len, const char *response,
                                  size_t response_text_len);

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
hu_error_t hu_response_guard_check(hu_allocator_t *alloc, const char *response, size_t response_len,
                                   char **out_response, size_t *out_len,
                                   hu_guard_outcome_t *out_outcome, hu_guard_report_t *report);

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
hu_error_t hu_response_guard_check_ex(hu_allocator_t *alloc, const char *response,
                                      size_t response_len, const hu_guard_context_t *ctx,
                                      char **out_response, size_t *out_len,
                                      hu_guard_outcome_t *out_outcome, hu_guard_report_t *report);

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
