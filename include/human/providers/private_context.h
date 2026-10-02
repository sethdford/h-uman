#ifndef HU_PROVIDERS_PRIVATE_CONTEXT_H
#define HU_PROVIDERS_PRIVATE_CONTEXT_H

/* Private prompt blocks: sections of a system prompt that carry the owner's
 * real memory and message text. Rule (2026-10-01, owner's privacy rule): real
 * message or memory text never reaches a cloud model without explicit opt-in.
 *
 * The rule keys on WHERE an attempt runs, never on whether it is the first
 * attempt: a private block may reach an attempt only when its provider is
 * local AND the model it asks for is a declared local model. Production runs
 * a local mlx primary behind the reliable wrapper, but agent_turn also routes
 * by MODEL NAME (analytical turns to gemini-3.1-pro-preview, S3 messages to
 * the degradation fallback_model, a failed on-device reply to
 * gemini-3.1-flash-lite) — each of those is a cloud attempt even when it is
 * the first one. Every non-local attempt is sent with the private blocks
 * removed; unknown provider names and undeclared models count as non-local
 * (fail closed).
 *
 * Interim: feat/thread-context's providers/local_only.{h,c} becomes the
 * canonical span table; this header folds into it after that merges.
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

/* On-device provider names (no data leaves the machine). "compatible" — what
 * an mlx_local instance reports — is NOT on the list: callers that know the
 * configured name (from_config) declare it via hu_reliable_set_primary_local. */
bool hu_private_context_provider_name_is_local(const char *name);

/* May a private block go to `prov` asked for `model`? A reliable provider
 * answers via hu_reliable_attempt_is_local (local primary + declared local
 * model); any other provider only by an on-device name. NULL → false. */
bool hu_private_context_attempt_is_local(const hu_provider_t *prov, const char *model,
                                         size_t model_len);

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
    bool ready; /* hu_private_context_request_for built `redacted` */
    const hu_chat_request_t *redacted;
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

/* The request one attempt may send: `req` itself when `local`, else the
 * redacted copy, built at most once per `scratch` (zero-initialize it; it is
 * only allocated when a non-local attempt actually happens). NULL when the
 * copy could not be built — the caller must then not send. Release with
 * hu_private_context_release. */
const hu_chat_request_t *hu_private_context_request_for(hu_allocator_t *alloc, bool local,
                                                        const hu_chat_request_t *req,
                                                        hu_private_request_t *scratch);

/* One chat attempt with the rule applied at the call: `req` as is when
 * (prov, model) is local, else the redacted copy (built and released here).
 * HU_ERR_OUT_OF_MEMORY when the copy cannot be built — nothing is sent. */
hu_error_t hu_private_context_chat(hu_provider_t *prov, hu_allocator_t *alloc,
                                   const hu_chat_request_t *req, const char *model,
                                   size_t model_len, double temperature, hu_chat_response_t *out);

#endif /* HU_PROVIDERS_PRIVATE_CONTEXT_H */
