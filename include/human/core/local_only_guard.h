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
 * Locality is decided by the ENDPOINT (loopback host, unix socket or an
 * in-process model path). Two vetoes sit on top of it, matching sibling PR
 * #581 (feat/thread-context, providers/local_only.{h,c}): a loopback gateway
 * that forwards to the cloud (litellm, openrouter, ...) is not local, and a
 * cloud model NAME (gemini*, gpt-*, claude*, grok*, o1/o3/o4, *:cloud,
 * *-cloud) is refused even on a loopback URL. This parser is the canonical
 * one.
 *
 * TODO(after #581 and this land): make #581's hu_local_only_endpoint_is_local
 * and hu_local_only_model_is_cloud delegate to
 * hu_local_only_provider_endpoint_is_local / hu_local_only_model_name_is_cloud
 * below, so there is one rule.
 *
 * Voice allow-list (owner ruling 2026-10-02, "allow voice services only"):
 * under local_only the only content that may leave is (a) reply text to the
 * configured TTS service for an outgoing voice note and (b) inbound audio to
 * the configured STT service. Services are named "tts:<vendor>" /
 * "stt:<vendor>", derived from the ENDPOINT (vendor = the host's registrable
 * label: api.cartesia.ai -> cartesia). Conversational voice (Gemini Live,
 * OpenAI Realtime), websocket chat, vision and media generation are model
 * requests and are never allowable. */

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

/* Parse an HU_LOCAL_ONLY value. Returns 0 (OFF: 0|off|false), 1 (SHADOW:
 * audit|shadow), 2 (LIVE: 1|on|live|true|enforce), or -1 for NULL / empty
 * (the configured mode then applies). Any other value fails CLOSED to 2, and
 * hu_local_only_mode() logs one WARN naming it. */
int hu_local_only_env_parse(const char *value);

/* True for a model name only a cloud API serves: gemini*, gpt-*, chatgpt*,
 * claude*, grok*, o1/o3/o4[-*], *-cloud, *:cloud (path prefixes like
 * "publishers/google/models/" are skipped). Case-insensitive. */
bool hu_local_only_model_name_is_cloud(const char *model, size_t model_len);

/* Provider-level locality. override: >0 providers[].local=true, <0 false,
 * 0 unset. Cloud-forwarding gateways (openrouter, litellm, portkey, helicone,
 * together, groq, fireworks, requesty) are never local; in-process backends
 * (embedded, coreml, mlx, llamacpp, llama-cli, huml, apple*) are local with no
 * URL; otherwise the endpoint decides. */
bool hu_local_only_provider_endpoint_is_local(const char *provider_name, const char *base_url,
                                              int override);

/* When `url` is a voice service endpoint (TTS or STT), writes "tts:<vendor>"
 * or "stt:<vendor>" into out and returns true. */
bool hu_local_only_voice_service(const char *url, char *out, size_t cap);

/* The voice allow-list ("tts:cartesia", "stt:cartesia", ...). Copied; at most
 * 16 entries. NULL / 0 = nothing allowed. Reset by hu_local_only_reset(). */
void hu_local_only_set_allow(const char *const *items, size_t count);
bool hu_local_only_service_allowed(const char *service);

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
/* Like set_caller, but keeps a more specific tag an outer frame already set
 * (a proactive turn stays "proactive" inside hu_agent_turn). Restore with
 * hu_local_only_set_caller(returned). */
const char *hu_local_only_enter(const char *tag);

/* The backstop. HU_OK when the request may go out; HU_ERR_PERMISSION_DENIED
 * when LIVE and `url` is a non-local model request. Logs exactly one WARN
 * line per refused (or, in SHADOW, would-be-refused) request:
 *   [local_only] refused provider=<host> model=<model> caller=<tag>
 * Never logs content: the host, a sanitized model id and the caller tag only.
 * `body` is read only to find the "model" field. */
hu_error_t hu_local_only_check_request(const char *url, const char *body, size_t body_len);

/* Websocket backstop. Stricter than HTTP: under local_only any non-local
 * websocket is refused unless it is an allowed voice service (every remote
 * websocket in this codebase carries conversation: Gemini Live, OpenAI
 * Realtime, OpenAI ws_streaming chat). */
hu_error_t hu_local_only_check_ws(const char *url);

/* The same strict rule for any endpoint configured as "local" by field name
 * (voice.local_stt_endpoint / local_tts_endpoint): a non-local URL is
 * refused unless it is an allowed voice service. */
hu_error_t hu_local_only_check_endpoint(const char *url);

/* Provider-level overrides, registered at apply time (providers[].local and
 * the cloud-gateway list), consulted by every per-request check. A request
 * whose URL starts with a VETOED base (a gateway, or providers[].local=false)
 * is never local, even on loopback; one under a VOUCHED base
 * (providers[].local=true) is local, even on a LAN address. Vetoed wins.
 * Copied; at most 16 of each, 255 bytes each. */
void hu_local_only_set_endpoint_overrides(const char *const *vouched, size_t vouched_count,
                                          const char *const *vetoed, size_t vetoed_count);
/* hu_provider_endpoint_is_local with the registered overrides applied. */
bool hu_local_only_request_url_is_local(const char *url);

/* The caller tag set on this thread (NULL when none). */
const char *hu_local_only_current_caller(void);

/* Explicit service gate for a content path that is not an endpoint shape
 * (e.g. "tool:web_search"). Refused unless allowed; logs and counts like the
 * backstop. `url` may be NULL. */
hu_error_t hu_local_only_check_service(const char *service, const char *url);

/* Counters since process start (or the last hu_local_only_reset). */
uint64_t hu_local_only_refused_count(void);
uint64_t hu_local_only_audit_count(void);

#endif /* HU_CORE_LOCAL_ONLY_GUARD_H */
