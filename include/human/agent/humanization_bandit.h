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

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_HUMANIZATION_BANDIT_H */
