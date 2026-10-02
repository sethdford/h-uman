#ifndef HU_PROVIDERS_PRIVATE_CONTEXT_H
#define HU_PROVIDERS_PRIVATE_CONTEXT_H

/* Private prompt blocks: sections of a system prompt that carry the owner's
 * real memory and message text. Rule (2026-10-01, owner's privacy rule): real
 * message or memory text never reaches a cloud model without explicit opt-in.
 *
 * Production serves replies from a LOCAL primary (mlx_local) behind the
 * reliable wrapper, whose fallbacks (model_fallbacks, extra providers such as
 * gemini) re-issue the SAME request to the cloud when the local attempt fails.
 * A private block therefore reaches only the reliable wrapper's primary
 * attempt (inner provider, caller's model); every other attempt — an extra
 * provider, a fallback model, the degradation fallback model — is sent with
 * the private blocks removed.
 *
 * Block contract: a block starts at a line beginning with one of the headings
 * below (at the start of the text or right after '\n') and runs through the
 * first blank line ("\n\n") after it, or to the end of the text. A block body
 * therefore must not contain a blank line. Removing a block that was inserted
 * at a line start restores the text byte-for-byte to what it was without it.
 *
 * Add a heading here when another prompt section carries private text. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include <stdbool.h>
#include <stddef.h>

/* HU_IMMERSIVE_CONTEXT (src/agent/turn/immersive_context.c). */
#define HU_PRIVATE_BLOCK_IMMERSIVE_CONTEXT "## What you know right now\n"

/* True when `text` contains at least one private block. */
bool hu_private_context_present(const char *text, size_t len);

/* Removes every private block from `text` in place; returns the new length
 * and NUL-terminates when `text[len]` is writable (the caller owns len + 1
 * bytes). Text without a private block is returned unchanged. */
size_t hu_private_context_strip(char *text, size_t len);

/* Heap copy of `text` with the private blocks removed (NUL-terminated,
 * free with len + 1). *out is NULL when `text` carries no private block:
 * the caller sends the original. */
hu_error_t hu_private_context_strip_dup(hu_allocator_t *alloc, const char *text, size_t len,
                                        char **out, size_t *out_len);

/* Scratch owned by a redacted request copy. */
typedef struct hu_private_request {
    hu_chat_request_t req;
    hu_chat_message_t *msgs;
    char **bodies; /* stripped system contents, one slot per message */
    size_t *body_lens;
    size_t count;
} hu_private_request_t;

/* The request to send on a non-primary attempt: `req` itself when no system
 * message carries a private block (no allocation), else a copy in `scratch`
 * whose system messages have their private blocks removed and whose
 * prompt_cache_id is cleared (the id hashes the unredacted prompt). On
 * allocation failure returns NULL — the caller must then NOT send `req`.
 * Release with hu_private_context_release either way. */
const hu_chat_request_t *hu_private_context_redact_request(hu_allocator_t *alloc,
                                                           const hu_chat_request_t *req,
                                                           hu_private_request_t *scratch);

void hu_private_context_release(hu_allocator_t *alloc, hu_private_request_t *scratch);

#endif /* HU_PROVIDERS_PRIVATE_CONTEXT_H */
