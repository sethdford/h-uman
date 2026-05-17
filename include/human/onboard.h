#ifndef HU_ONBOARD_H
#define HU_ONBOARD_H

#include "core/allocator.h"
#include "core/error.h"
#include <stdbool.h>
#include <stddef.h>

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

/**
 * Inputs for the post-wizard "What's next" message formatter.
 *
 * Pure-data record so `hu_onboard_nextstep_format` can be tested without
 * spawning a subprocess. See sprints/sprint-9/designs/US-9.2.md for the
 * truth table.
 */
typedef struct hu_onboard_nextstep_ctx {
    const char *config_path; /* NUL-terminated; required. */
    const char *provider;    /* "apple" | "mlx_local" | "gemini" | ... */
    bool platform_is_apple;  /* compile-time __APPLE__ at call site */
    bool already_exists;     /* true => print early-exit variant */
    bool parsed_ok;          /* result of post-write hu_config_load */
} hu_onboard_nextstep_ctx_t;

/**
 * Format the post-wizard "What's next" block into a caller-provided buffer.
 *
 * Pure function: no I/O, no globals, no allocation. Writes a
 * NUL-terminated, newline-terminated block to `out` (size `out_sz`).
 *
 * Returns:
 *   HU_OK on success;
 *   HU_ERR_INVALID_ARGUMENT if ctx, ctx->config_path, or out is NULL or
 *     out_sz is 0;
 *   HU_ERR_IO if the message would be truncated (out_sz too small).
 *
 * Truth table (covered by tests/test_onboard_nextstep.c):
 *
 *   already_exists | parsed_ok | platform_is_apple | block
 *   --------------- | --------- | ----------------- | -----
 *   true            | (n/a)     | true              | Config already exists at <path>.\nRun 'human
 * doctor' to check status, or 'human doctor imessage' to pair iMessage.\n true            | (n/a)
 * | false             | Config already exists at <path>.\nRun 'human doctor' to check status.\n
 *   false           | false     | (any)             | Config written to <path>.\nWarning: config
 * written but failed to parse — run 'human doctor --fix' to repair\n false           | true      |
 * true              | Config verified OK\nConfig written to <path>.\nWhat's next:\n  1. Pair
 * iMessage:  human doctor imessage\n  2. Start the agent: human agent\n false           | true |
 * false             | Config verified OK\nConfig written to <path>.\nWhat's next:\n  1. Start the
 * agent: human agent\n  (Tier-1 channels other than iMessage require manual config — see
 * docs/guides/channels.md)\n
 */
hu_error_t hu_onboard_nextstep_format(const hu_onboard_nextstep_ctx_t *ctx, char *out,
                                      size_t out_sz);

#endif /* HU_ONBOARD_H */
