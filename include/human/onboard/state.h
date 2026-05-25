#ifndef HU_ONBOARD_STATE_H
#define HU_ONBOARD_STATE_H

#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>

/**
 * Onboarding step identifiers.
 * Order matters — they define the default progression path.
 */
typedef enum hu_onboard_step_id {
    HU_ONBOARD_STEP_WELCOME = 0,
    HU_ONBOARD_STEP_PROVIDER,
    HU_ONBOARD_STEP_PERSONA,
    HU_ONBOARD_STEP_CHANNELS,
    HU_ONBOARD_STEP_TESTSEND,
    HU_ONBOARD_STEP_COMPLETE,
} hu_onboard_step_id_t;

/**
 * Complete persistent state for the onboarding wizard.
 *
 * This struct is serialized as-is to ~/.human/onboard-state.json
 * (with a schema version prefix) after every step transition.
 * A crash at any point can be recovered by resuming from the
 * current step with all prior answers intact.
 *
 * Schema version 1 is locked at Sprint 51 close. Future additions
 * ship as schema v2 with an explicit migration path.
 */
typedef struct hu_onboard_state {
    int schema_version;               /* 1 — locked; rejects mismatches */
    hu_onboard_step_id_t current;     /* current step in the wizard */
    hu_onboard_step_id_t history[10]; /* back-navigation stack */
    size_t history_depth;             /* count of valid history entries */

    /* Per-step persisted answers — one sub-struct per step.
     * These are accumulative; once a step completes, its answers
     * persist through subsequent steps and future resumes. */

    struct {
        char provider_name[32];     /* "gemini", "anthropic", "openai", "mlx_local", etc. */
        bool provider_smoke_passed; /* true = connection test succeeded */
    } provider;

    struct {
        char template_choice;    /* '1' | '2' | '3' | 'm' for markdown-import */
        char markdown_path[512]; /* iff template_choice=='m' */
    } persona;

    struct {
        bool imessage_enabled;     /* selected in Step 4 */
        bool slack_enabled;        /* selected in Step 4 */
        bool discord_enabled;      /* selected in Step 4 */
        bool telegram_enabled;     /* selected in Step 4 */
        bool imessage_fda_pending; /* requires FDA grant on next step */
    } channels;

    struct {
        char contact_handle[128]; /* phone number / email / username of test recipient */
        bool test_send_succeeded; /* true = Step 5 completed */
    } testsend;
} hu_onboard_state_t;

/**
 * Initialize a freshly-allocated onboard state to defaults.
 * (current = WELCOME, schema_version = 1, all fields zeroed).
 */
void hu_onboard_state_init(hu_onboard_state_t *state);

/**
 * Save state atomically to path (tmp + fwrite + fflush + fsync + rename).
 *
 * This mirrors the pattern in hu_personal_model_save:
 * - Crash before rename: <path>.tmp is partial, <path> is untouched.
 * - Crash after rename: <path> is the new file, intact.
 * - No in-between window: rename(2) is atomic on POSIX.
 *
 * Returns HU_OK on success, HU_ERR_IO on any write/sync failure,
 * HU_ERR_INVALID_ARGUMENT if state or path is NULL.
 */
hu_error_t hu_onboard_state_save(const hu_onboard_state_t *state, const char *path);

/**
 * Load and validate state from path.
 *
 * - Returns HU_OK on success, populating *out.
 * - Returns HU_ERR_IO if the file doesn't exist or can't be read.
 * - Returns HU_ERR_INVALID_ARGUMENT if state is NULL or path is empty.
 * - Returns HU_ERR_SCHEMA_MISMATCH if schema_version != 1 (rejects v2, v0, etc.).
 *
 * If load fails, *out is NOT modified (safe to call with uninitialized *out).
 */
hu_error_t hu_onboard_state_load(hu_onboard_state_t *out, const char *path);

#endif /* HU_ONBOARD_STATE_H */
