#ifndef HU_CHANNELS_CLI_H
#define HU_CHANNELS_CLI_H

#include "human/channel.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>

hu_error_t hu_cli_create(hu_allocator_t *alloc, hu_channel_t *out);
void hu_cli_destroy(hu_channel_t *ch);

/* SOTA-2026 init-11 (S1.5): attach a persona handle so outbound CLI
 * sends honor the resolved typing profile via `hu_typing_send`.
 *
 * Contract:
 *   - persona == NULL ⇒ legacy byte-for-byte send path; the typing
 *     simulator is never invoked. This is the default after
 *     `hu_cli_create` and is what existing tests depend on.
 *   - persona != NULL ⇒ `hu_typing_profile_resolve(persona, "cli", …)`
 *     is consulted on every send. An `instant` profile still falls
 *     back to the byte-for-byte path.
 *
 * The handle is intentionally `const void *` to mirror the typing
 * simulator's opacity convention — the CLI channel does not pull in
 * persona.h. The caller owns `persona` and must keep it alive for the
 * channel's lifetime; the channel does not copy or free it. */
void hu_cli_set_persona(hu_channel_t *ch, const void *persona);

/* Read a line from stdin. Caller must free. Returns NULL on EOF. */
char *hu_cli_readline(hu_allocator_t *alloc, size_t *out_len);

/* True for exit, quit, :q */
bool hu_cli_is_quit_command(const char *line, size_t len);

#endif /* HU_CHANNELS_CLI_H */
