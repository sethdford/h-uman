#ifndef HU_ONBOARD_H
#define HU_ONBOARD_H

#include "core/allocator.h"
#include "core/error.h"
#include <stdbool.h>

/**
 * Shared starter persona JSON written by both `human init` and
 * `human onboard` to `~/.human/personas/default.json`.
 *
 * Single source of truth — the previous duplication between
 * `cli_commands.c::HU_INIT_DEFAULT_PERSONA` and
 * `onboard.c::HU_ONBOARD_DEFAULT_PERSONA` had silently drifted into
 * having `channel_overlays` declared as a JSON array of records
 * (where the parser at `src/persona/persona.c::parse_overlay` only
 * accepts an object keyed by channel name) AND with numeric overlay
 * values (where the parser only consumes string-typed values via
 * `hu_json_get_string`). The combined effect was that overlays were
 * silently dropped on load and `directive_variant_for_overlay`
 * returned `NULL_OVERLAY` for every channel in production.
 *
 * This canonical blob:
 *   - Declares `channel_overlays` as an OBJECT keyed by channel name.
 *   - Uses STRING-typed `formality` / `avg_length` / `emoji_usage`
 *     so the parser actually reads them.
 *   - Maps each Tier-1 channel (telegram/discord/imessage/slack) to
 *     overlay values that route to a meaningful directive variant
 *     per `src/memory/personal_model.c::directive_variant_for_overlay`.
 */
/**
 * Returns a pointer to the starter persona JSON as a flat NUL-terminated
 * string. Internally the literal is split across two arrays to satisfy
 * GCC's -Werror=overlength-strings on strict Linux builds (the combined
 * content exceeds C99's 4095-char minimum guarantee); the accessor
 * lazily joins them into a single static buffer on first call.
 *
 * If out_len is non-NULL, it receives the length of the returned string
 * (excluding NUL) so callers can avoid a redundant strlen.
 */
const char *hu_starter_persona_get(size_t *out_len);

/**
 * Run the interactive setup wizard (no CLI overrides).
 * On macOS with Apple Intelligence enabled, defaults to Apple on-device.
 */
hu_error_t hu_onboard_run(hu_allocator_t *alloc);

/**
 * Run the setup wizard with optional CLI overrides.
 * cli_provider: pre-select provider (NULL = interactive prompt).
 * cli_api_key: pre-fill API key (NULL = interactive prompt).
 * apple_shortcut: if true, skip all prompts and configure Apple on-device.
 */
hu_error_t hu_onboard_run_with_args(hu_allocator_t *alloc, const char *cli_provider,
                                    const char *cli_api_key, bool apple_shortcut);

/**
 * Check if this is the first run (no ~/.human/config.json exists).
 */
bool hu_onboard_check_first_run(void);

#endif /* HU_ONBOARD_H */
