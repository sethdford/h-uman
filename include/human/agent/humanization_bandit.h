#ifndef HU_AGENT_HUMANIZATION_BANDIT_H
#define HU_AGENT_HUMANIZATION_BANDIT_H

#include "human/agent/contextual_bandit.h"
#include "human/persona.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decide humanization params (disfluency_frequency, backchannel_probability)
 * based on the bandit's Thompson sample for a given contact.
 *
 * Returns a humanization_config_t with only disfluency_frequency and
 * backchannel_probability set (other fields left at caller's defaults).
 *
 * Decision logic:
 * - Sample θ from the contact's arm's Beta(α, β) posterior
 * - If θ > 0.65: aggressive (disfluency=0.25, backchannel=0.45)
 * - Else if θ > 0.35: moderate (disfluency=0.15, backchannel=0.30)
 * - Else: conservative (disfluency=0.05, backchannel=0.10)
 * - New contacts default to conservative (safe)
 *
 * Thread-safe: reads bandit arm state only; does not modify. */
hu_humanization_config_t hu_humanization_decide_contact_params(hu_contextual_bandit_t *bandit,
                                                               uint64_t contact_handle);

/* Apply bandit-based humanization override to params if gate is enabled.
 * Gate controlled by HU_BANDIT_HUMANIZATION env var (default OFF).
 * If bandit is NULL or gate is off, params remain unchanged.
 * If gate is ON and bandit is non-NULL, calls hu_humanization_decide_contact_params
 * and updates inout_params with the bandit's decision.
 * Returns true if override was applied, false if gate was off or bandit NULL. */
bool hu_humanization_apply_bandit_override(hu_contextual_bandit_t *bandit, uint64_t contact_handle,
                                           hu_humanization_config_t *inout_params);

/* ── Persistence (2026-07-18: arm posteriors survive daemon restarts) ──
 * Without these, every restart reset all Beta(α,β) arms to the weak prior
 * and the bandit re-explored forever. Same pattern as somatic state
 * (src/persona/somatic.c): atomic tmp+rename save, validated + clamped
 * load, parse failure leaves the caller's state untouched. */

/* Serialize all arms to a JSON file at path. Atomic via tmp + rename.
 * contact handles are written as decimal STRINGS — they are full 64-bit
 * hashes and a JSON double (53-bit mantissa) would corrupt them.
 * Returns HU_OK, HU_ERR_INVALID_ARGUMENT, or HU_ERR_IO. */
hu_error_t hu_humanization_bandit_save_file(const hu_contextual_bandit_t *bandit, const char *path);

/* Merge arms from a JSON file into an existing bandit. Values are clamped
 * to the arm's legal range (alpha/beta >= 1.0, updates >= 0); entries with
 * a zero/unparseable handle are skipped. Any parse failure returns without
 * touching the bandit. Returns HU_OK, HU_ERR_NOT_FOUND (no file),
 * HU_ERR_IO, or HU_ERR_PARSE. */
hu_error_t hu_humanization_bandit_load_file(hu_contextual_bandit_t *bandit, const char *path);

/* Canonical on-disk location: $HOME/.human/bandit_humanization.json.
 * Returns buf on success, NULL when unavailable — and always NULL under
 * HU_IS_TEST so tests never touch the real home dir. */
const char *hu_humanization_bandit_default_path(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_HUMANIZATION_BANDIT_H */
