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
extern const char hu_starter_persona_json[];

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
 * Format the post-onboard "next step" CLI message.
 *
 * Pure function: no I/O, no globals, no syscalls. The 4 boolean inputs
 * map deterministically to exactly 5 distinct output strings; the
 * truth table below is the contract.
 *
 * Precedence ladder (earlier rows short-circuit later ones):
 *
 *   imessage_paired | persona_set | ollama_ok | brew_installed | output id
 *   ----------------|-------------|-----------|----------------|------------------
 *   —               | false       | —         | —              | fallback_bare
 *   false           | true        | —         | —              | pair_imessage
 *   true            | true        | false     | —              | chat_cloud
 *   true            | true        | true      | false          | chat_no_brew
 *   true            | true        | true      | true           | all_ready
 *
 * The 5 outputs are guaranteed `strcmp`-distinct and none contains the
 * legacy generic string "You're all set". The `all_ready` output is
 * additionally guaranteed to contain "human chat" and to NOT contain
 * either "setup" or "configure".
 *
 * The prototype is annotated `warn_unused_result` so callers that
 * silently drop the return code fail with `-Werror`.
 *
 * @param imessage_paired  true if at least one iMessage chat is allow-listed
 * @param persona_set      true if `~/.human/personas/default.json` parses cleanly
 * @param ollama_ok        true if `GET http://127.0.0.1:11434/api/tags` returned 200
 * @param brew_installed   true if Homebrew is detected on the system
 * @param buf              output buffer; MUST be non-NULL and `buflen >= 1`
 * @param buflen           size of `buf` in bytes
 * @return
 *   - `HU_OK` on success; `buf` contains a NUL-terminated next-step string.
 *   - `HU_ERR_INVALID_ARGUMENT` if `buf == NULL` or `buflen == 0`.
 *   - `HU_ERR_IO` if `buflen` is strictly less than the formatted message's
 *     terminating NUL. See `sprints/sprint-43/designs/US-43.2.md`
 *     "Design Decision" for why this reuses `HU_ERR_IO` rather than
 *     introducing a new `HU_ERR_BUFFER_TOO_SMALL` enum value.
 *     On short-buffer the function writes a NUL terminator at `buf[0]`
 *     (so the buffer is always safe to read) before returning.
 */
__attribute__((warn_unused_result)) hu_error_t hu_onboard_nextstep_format(bool imessage_paired,
                                                                          bool persona_set,
                                                                          bool ollama_ok,
                                                                          bool brew_installed,
                                                                          char *buf, size_t buflen);

#endif /* HU_ONBOARD_H */
