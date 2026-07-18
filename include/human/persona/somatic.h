#ifndef HU_PERSONA_SOMATIC_H
#define HU_PERSONA_SOMATIC_H

#include "human/context/authentic.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdint.h>

typedef struct hu_somatic_state {
    float energy;
    float social_battery;
    float focus;
    float arousal;
    hu_physical_state_t physical;
    uint64_t last_interaction_ts;
    uint64_t last_recharge_ts;
    float conversation_load_accumulated;
} hu_somatic_state_t;

void hu_somatic_init(hu_somatic_state_t *state);

void hu_somatic_update(hu_somatic_state_t *state, uint64_t now_ts, float emotional_intensity,
                       uint32_t topic_switches, hu_physical_state_t scheduled_physical);

hu_error_t hu_somatic_build_context(hu_allocator_t *alloc, const hu_somatic_state_t *state,
                                    char **out, size_t *out_len);

const char *hu_somatic_energy_label(float energy);
const char *hu_somatic_battery_label(float battery);

/* ── Persistence (SOTA roadmap #11) ────────────────────────────────────────
 * The somatic state previously died with the process: every daemon restart
 * reset energy/social-battery to full, erasing "the day so far" — the
 * opposite of having an interior. Save/load make it continuous across
 * restarts; recovery-while-away needs no special handling because the next
 * hu_somatic_update(now_ts) already applies time-based recharge.
 *
 * Atomic write (tmp + rename). Load validates + clamps every field so a
 * hand-edited or corrupt file cannot inject out-of-range values into
 * behavior gates; on ANY parse failure the caller's state is untouched.
 * Explicit paths keep both fully unit-testable. */
hu_error_t hu_somatic_save_file(const hu_somatic_state_t *state, const char *path);
hu_error_t hu_somatic_load_file(hu_somatic_state_t *state, const char *path);

/* Default production path (~/.human/somatic_state.json) written into buf.
 * Returns NULL under HU_IS_TEST or when HOME is unset — callers treat NULL
 * as "persistence disabled" so tests never touch the real home dir. */
const char *hu_somatic_default_path(char *buf, size_t cap);

#endif
