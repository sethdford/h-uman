#ifndef HU_DAEMON_REPLY_DELAY_H
#define HU_DAEMON_REPLY_DELAY_H

/*
 * Contract C5, Part C — reply-delay model loading + shadow measurement.
 *
 * scripts/fit_reply_delay_model.py fits a quantile table of Seth's own
 * reply-delay distribution from chat.db, conditioned on (hour-of-day,
 * incoming-message-length bucket, contact reply-frequency tercile), with
 * hierarchical fallback for sparse cells (see that script's docstring for
 * the exact bucketing and fallback chain). This header is the C-side
 * consumer of the resulting ~/.human/reply_delay_model.json.
 *
 * Activation follows ~/.claude/rules/feature-gate-requires-measurement.md:
 * three states, gated by HU_REPLY_DELAY_MODEL, default OFF.
 *
 *   off    (default) — the model is never loaded or consulted.
 *   shadow — the model is consulted and LOGGED alongside the existing
 *            heuristic delay; the outbound send path is UNCHANGED.
 *   live   — reserved for a future task once shadow logging has a
 *            measurement showing the model tracks Seth's real behavior;
 *            NOT wired into any send-path decision by this contract.
 *
 * hu_reply_delay_from_model() is a pure, side-effect-free sampler: given a
 * model file, three feature values, and a caller-supplied seed, it returns
 * a deterministic sample from the fitted distribution's inverse-CDF
 * (linear interpolation across the model's p10/p25/p50/p75/p90 quantiles).
 * It performs its own hierarchical fallback (exact cell -> hour+length
 * marginal -> hour marginal -> global) so callers never need to know
 * whether a specific cell had enough chat.db samples.
 */

#include <stddef.h>
#include <stdint.h>

typedef enum hu_reply_delay_mode {
    HU_REPLY_DELAY_MODE_OFF = 0,
    HU_REPLY_DELAY_MODE_SHADOW,
    HU_REPLY_DELAY_MODE_LIVE,
} hu_reply_delay_mode_t;

/* Reads HU_REPLY_DELAY_MODEL from the environment. Unset or unrecognized
 * values are OFF (fail closed — an unset gate must never silently start
 * shadow-logging or, worse, driving behavior). Recognized (case-sensitive)
 * values: "off", "shadow", "live". */
hu_reply_delay_mode_t hu_reply_delay_mode_from_env(void);

/* Loads `model_path` (the JSON written by fit_reply_delay_model.py),
 * selects the best-available bucket for (hour, incoming_len, contact_freq)
 * via the hierarchical fallback chain baked into the model file, and
 * returns a deterministic sample (seconds) drawn from that bucket's
 * quantile table using `seed` as the PRNG seed (same seed -> same sample,
 * so tests and shadow-vs-shadow reruns are reproducible).
 *
 * Returns -1 if the model file is missing, unreadable, malformed, or has
 * no usable bucket at all (not even the global fallback) — never a
 * fabricated number for a model that isn't actually there
 * (~/.claude/rules/reports-success-does-nothing.md). Callers MUST check
 * for a negative return before using the value. */
int64_t hu_reply_delay_from_model(const char *model_path, int hour, size_t incoming_len,
                                  double contact_freq, uint32_t seed);

/* SHADOW measurement — logs the model's prediction alongside the
 * heuristic delay the daemon already computed for this reply, WITHOUT
 * changing anything about the reply itself. No-op when
 * hu_reply_delay_mode_from_env() is not HU_REPLY_DELAY_MODE_SHADOW, and
 * no-op (best-effort, never fatal) when the model can't be loaded.
 *
 * `heuristic_delay_secs` is the delay the EXISTING (non-model) code path
 * already computed or observed for this reply — the comparison baseline.
 * `incoming_len` and `contact_freq` are best-effort: at the current call
 * site (next to hu_missed_message_acknowledgment_gated, which does not
 * carry either value) they default to 0 / 0.0, which routes the model
 * through its (hour) or global fallback bucket rather than the full
 * (hour, len, freq) cell. Threading the real values through requires
 * enriching that call site in src/daemon.c, which is out of scope for
 * this contract (src/daemon.c is off-limits) — see the model file's
 * `cells` vs `hour_marginals`/`global` keys for what's actually reachable
 * today. */
void hu_reply_delay_shadow_log(int hour, size_t incoming_len, double contact_freq,
                               int64_t heuristic_delay_secs, const char *contact_key,
                               size_t contact_key_len, uint32_t seed);

#endif /* HU_DAEMON_REPLY_DELAY_H */
