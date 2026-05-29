/* include/human/doctor/check_local_voice.h
 *
 * Doctor check (AC-3) reporting readiness of the local Gemma+LoRA voice path
 * that the AUTO routing default prefers. Surfaces the three operator-actionable
 * facts behind "why am I (not) getting my own voice":
 *   - routing mode (OFF / AUTO / FORCE)
 *   - LoRA adapter file present + nonempty (personalization.lora_adapter_path)
 *   - mlx_local provider base_url configured
 *   - binary built with HU_ENABLE_CURL (required to serve/swap the adapter)
 *
 * Verdict semantics:
 *   NA   — routing OFF, or no local model/url configured (cloud-only user)
 *   FAIL — routing intends local but a prerequisite is missing (curl off,
 *          url unset, or adapter file absent); message names the fix
 *   PASS — routing intends local and all prerequisites are present
 *
 * Live server reachability is intentionally NOT probed here (kept network-free
 * + deterministic); the runtime health probe (model_router_health.c) gates the
 * actual per-turn routing and the T4 fallback covers a downed server.
 *
 * ctx contract: borrowed `const struct hu_config *`, same shape as the sibling
 * check_reaction_collection_wired. NULL cfg → NA.
 */
#ifndef HU_DOCTOR_CHECK_LOCAL_VOICE_H
#define HU_DOCTOR_CHECK_LOCAL_VOICE_H

#include "human/core/error.h"
#include "human/doctor/check.h"
#include <stdbool.h>

struct hu_config;

typedef struct hu_doctor_check_local_voice_ctx {
    const struct hu_config *cfg;
} hu_doctor_check_local_voice_ctx_t;

/* Public vtable — registered by registry.c::register_defaults. */
extern hu_doctor_check_t hu_doctor_check_local_voice;

/* Test seam: explicit facts (no I/O, no #ifdef) so both the ready and the
 * misconfigured verdicts are coverable in a single binary. `routing` holds a
 * hu_mlx_local_routing_t value. Production code MUST call the vtable above. */
hu_doctor_check_result_t
hu_doctor_check_local_voice_run_for_test(int routing, bool model_configured, bool url_configured,
                                         bool adapter_present, bool curl_built);

#endif /* HU_DOCTOR_CHECK_LOCAL_VOICE_H */
