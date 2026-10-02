#ifndef HU_CORE_LOCAL_ONLY_GUARD_H
#define HU_CORE_LOCAL_ONLY_GUARD_H

/* Local-only enforcement (owner decision 2026-10-01): conversation content
 * never leaves this machine. Two halves:
 *
 *   1. Routing (callers): code that would pick a cloud model or create a cloud
 *      provider asks hu_local_only_enforced() and stays on the primary local
 *      provider instead (director/classify, analytical routing, S3 switch,
 *      on-device-failure retry, guard slim retry, image description).
 *   2. The backstop (this module + src/core/http.c): every model request
 *      (generation, embedding, transcription endpoint shapes) to a host that
 *      is not loopback / a unix socket is refused before a byte is sent.
 *      Non-model HTTP (feeds, channel APIs, OAuth) is never touched.
 *
 * Mode precedence: HU_LOCAL_ONLY env (0|off|false, 1|on|live|true,
 * audit|shadow) > the mode configured from config at startup
 * (privacy.local_only, see providers/local_only_config.h) > OFF when nothing
 * was configured (tests, tools that never call configure).
 *   OFF     — no check, byte-identical to before.
 *   SHADOW  — "audit": log what WOULD be refused, refuse nothing, route as OFF.
 *   LIVE    — "enforce": refuse, and route locally.
 *
 * Locality is decided by the ENDPOINT (loopback host or unix socket), never
 * by provider name. Sibling PR #581 (feat/thread-context,
 * providers/local_only.{h,c}) currently decides by provider NAME plus
 * cloud-model-name prefixes; the two rules must be reconciled when both
 * land. */

#include "human/core/error.h"
#include "human/core/gate_mode.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* True when `url` names this machine: http(s)/ws(s) to localhost,
 * *.localhost, 127.0.0.0/8 or ::1; a unix socket ("unix:", "http+unix://");
 * or an absolute filesystem path (in-process model file / socket path).
 * NULL, empty and anything unparseable are NOT local (fail closed). */
bool hu_provider_endpoint_is_local(const char *url, size_t url_len);

/* True when `url`'s path has the shape of a model request: generation,
 * embedding or transcription (Gemini ":generateContent" family and
 * ":predict", OpenAI-compatible "/chat/completions", "/completions",
 * "/embeddings", "/responses", "/audio/transcriptions", Anthropic
 * "/v1/messages", Ollama "/api/chat|generate|embed"). Channel and feed APIs
 * never match. */
bool hu_local_only_url_is_model_request(const char *url);

/* Parse an HU_LOCAL_ONLY value. Returns 0 (OFF), 1 (SHADOW/audit), 2 (LIVE),
 * or -1 for NULL / empty / unrecognized (the configured mode then applies). */
int hu_local_only_env_parse(const char *value);

/* Pure resolution. env_value is the raw HU_LOCAL_ONLY string (may be NULL).
 * cfg_value: -1 key absent, 0 false, 1 true. When the key is absent the
 * switch is ON only if the primary provider's endpoint is local — a
 * cloud-primary install is never bricked by a default it did not choose. */
hu_gate_mode_t hu_local_only_resolve(const char *env_value, int cfg_value, bool primary_is_local);

/* Process-wide configured mode (set once at startup from config). */
void hu_local_only_configure(hu_gate_mode_t mode);
/* Forget the configured mode (tests). */
void hu_local_only_reset(void);

/* Effective mode right now: HU_LOCAL_ONLY env, else configured, else OFF. */
hu_gate_mode_t hu_local_only_mode(void);
/* mode == LIVE. Routing decisions use this; SHADOW routes as OFF. */
bool hu_local_only_enforced(void);

/* Thread-local caller tag for the refusal log line ("director", "vision",
 * ...). Returns the previous tag so callers can restore it. NULL = "unknown". */
const char *hu_local_only_set_caller(const char *tag);

/* The backstop. HU_OK when the request may go out; HU_ERR_PERMISSION_DENIED
 * when LIVE and `url` is a non-local model request. Logs exactly one WARN
 * line per refused (or, in SHADOW, would-be-refused) request:
 *   [local_only] refused provider=<host> model=<model> caller=<tag>
 * Never logs content: the host, a sanitized model id and the caller tag only.
 * `body` is read only to find the "model" field. */
hu_error_t hu_local_only_check_request(const char *url, const char *body, size_t body_len);

/* Counters since process start (or the last hu_local_only_reset). */
uint64_t hu_local_only_refused_count(void);
uint64_t hu_local_only_audit_count(void);

#endif /* HU_CORE_LOCAL_ONLY_GUARD_H */
