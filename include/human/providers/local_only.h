#ifndef HU_PROVIDERS_LOCAL_ONLY_H
#define HU_PROVIDERS_LOCAL_ONLY_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include <stdbool.h>
#include <stddef.h>

/* Local-only (private) prompt spans — the one place that strips them.
 *
 * Scope: this strips a marked BLOCK from requests that go through the
 * reliable provider. It does NOT make the underlying message content
 * local-only. Other paths send chat.db text to a cloud model directly and
 * never pass through here. The known one: the director and emotion
 * detection (daemon.c -> daemon_director.c) send the last 5 chat.db
 * messages to a raw gemini provider (g_classify_provider, created
 * unwrapped in daemon.c). That is pre-existing and an open owner decision.
 *
 * What it does guarantee: a marked span may reach the on-device model but is
 * removed from every reliable-provider attempt (primary or extra, first try
 * or fallback) that is not local. An attempt is local when its provider's
 * endpoint is loopback / unix-socket and not a cloud proxy, AND its model
 * name is not a cloud model's. The model check covers routes that switch
 * model BY NAME on the same provider (agent_turn.c: the analytical tier ->
 * gemini-3.1-pro-preview, S3 -> fallback_model, on-device failure -> the
 * reflexive cloud model, and the degradation retry).
 *
 * A span opens at a line starting with a registered heading and closes after
 * its end line, or — for a kind with no end line — at the first blank line.
 * A span with no close (the positional prompt cap cut it) fails closed: the
 * rest of that message is removed. To protect a new section, give it a unique
 * heading and add one row to k_span_kinds in src/providers/local_only.c. */

/* HU_THREAD_CONTEXT's "## Recent thread" block (daemon/thread_context.h). */
#define HU_LOCAL_ONLY_THREAD_BEGIN "## Recent thread"
#define HU_LOCAL_ONLY_THREAD_END   "## End of recent thread\n"

typedef struct hu_local_only_span_kind {
    const char *begin; /* line prefix that opens the span */
    const char *end;   /* full line that closes it (inclusive); NULL = first blank line */
    const char *tag;   /* log prefix of the owning gate, e.g. "HU_THREAD_CONTEXT" */
} hu_local_only_span_kind_t;

/* The registered kinds (read-only). */
const hu_local_only_span_kind_t *hu_local_only_span_kinds(size_t *count);

/* True only for in-process backends (no URL): a provider NAME says nothing
 * about where an HTTP endpoint points. NULL / unknown -> not local. */
bool hu_local_only_provider_name_is_local(const char *name);

/* True for a loopback or unix-socket base URL. Strict: the string must open
 * with http:// https:// ws:// wss:// (host 127.0.0.0/8 with octets 0-255,
 * localhost, *.localhost, ::1), or be "unix:" / an absolute path. 0.0.0.0,
 * scheme-less strings and anything unparsable are NOT local. */
bool hu_local_only_url_is_local(const char *base_url);

/* The endpoint decision. `override` > 0 forces local, < 0 forces not local
 * (config providers[].local). Otherwise, in order: a cloud gateway/proxy name
 * (openrouter, litellm, ...) is never local; a path backend (coreml, mlx,
 * embedded, llama-cli, llamacpp, huml — base_url is a model path) is always
 * local; a non-empty base_url decides by hu_local_only_url_is_local; no URL
 * is local only for in-process backends. */
bool hu_local_only_endpoint_is_local(const char *provider_name, const char *base_url, int override);

/* hu_local_only_provider_name_is_local(get_name()). An mlx_local instance
 * reports "compatible", so HTTP providers need the endpoint rule instead. */
bool hu_local_only_provider_is_local(const hu_provider_t *prov);

/* True for model names that only a cloud API serves (gemini-*, gpt-*,
 * claude-*, ...): sending one to a local provider still marks the attempt
 * non-local. */
bool hu_local_only_model_is_cloud(const char *model, size_t model_len);

/* The attempt decision: local provider AND not a cloud model name. */
bool hu_local_only_attempt_is_local(bool provider_local, const char *model, size_t model_len);

/* One log line per span kind present in `text`, for an attempt that is
 * about to be sent without it:
 *   [<tag>] applied=0 reason=stripped_cloud_model|stripped_cloud_endpoint model=<m>
 * stripped_cloud_model = an on-device endpoint asked for a cloud model by name
 * (agent_turn.c analytical / S3 routes): the block never reached the model
 * that answered, which a measurement must not count as "applied". */
void hu_local_only_log_strip(const char *text, size_t len, bool endpoint_local, const char *model,
                             size_t model_len);
void hu_local_only_log_strip_request(const hu_chat_request_t *req, bool endpoint_local,
                                     const char *model, size_t model_len);

/* True when `s` holds at least one span. */
bool hu_local_only_has_span(const char *s, size_t len);

/* Copy of `s` with every span removed. `*out` is NULL (and `*out_len` 0)
 * when `s` holds no span — the caller keeps the original. Returns
 * HU_ERR_OUT_OF_MEMORY when a span exists but the copy failed. */
hu_error_t hu_local_only_strip(hu_allocator_t *alloc, const char *s, size_t len, char **out,
                               size_t *out_len);

/* Turn-local, stripped shallow copy of a chat request. */
typedef struct hu_local_only_request {
    hu_chat_request_t req;
    hu_chat_message_t *msgs; /* NULL when nothing was stripped */
    char **owned;            /* per-message stripped content, NULL entries untouched */
    size_t *owned_lens;
    size_t count;
    size_t stripped; /* messages that lost a span */
} hu_local_only_request_t;

/* Point `*use` at a request safe for a non-local attempt: `in` itself when it
 * carries no span, else `scratch->req` (spans removed, prompt_cache_id
 * cleared so a cloud cache keyed on the full prompt is never reused). Always
 * pair with hu_local_only_request_release. On error `*use` is NULL and the
 * caller must not send. */
hu_error_t hu_local_only_request_prepare(hu_allocator_t *alloc, const hu_chat_request_t *in,
                                         hu_local_only_request_t *scratch,
                                         const hu_chat_request_t **use);

void hu_local_only_request_release(hu_allocator_t *alloc, hu_local_only_request_t *scratch);

#endif /* HU_PROVIDERS_LOCAL_ONLY_H */
