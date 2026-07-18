#ifndef HU_DAEMON_MESSAGE_ROUTER_H
#define HU_DAEMON_MESSAGE_ROUTER_H

#include "human/core/allocator.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hu_config;
struct hu_agent;

/* Cross-channel context formatting helpers — DDD Phase 2.5 (follow-on slice),
 * extracted from daemon.c. These build the human-readable "cross-channel
 * awareness" context lines injected into proactive prompts (e.g. "Slack · 2h
 * ago: ..."). Compiled only when SQLite is enabled and not in test builds,
 * matching the original guard; the declarations are harmless otherwise because
 * the (also-guarded) call sites never reference them there.
 *
 * (hu_daemon_dispatch_imessage_reply, the iMessage reply-route dispatcher that
 * also lives in daemon_message_router.c, is declared in human/daemon.h as part
 * of the public daemon API.) */

/* Format a relative-time label ("just now", "2h ago", "3d ago") from an ISO/
 * "Y-m-d H:M" timestamp into out[0..out_sz). Empty/unparseable → "recent" or
 * the raw string. */
void hu_daemon_cross_channel_format_when(char *out, size_t out_sz, const char *ts);

/* Title-case a platform name (first char upper) into out[0..out_sz). */
void hu_daemon_cross_channel_platform_label(const char *plat, char *out, size_t out_sz);

/* Append `line` to a growing newline-joined buffer (realloc via alloc).
 * Returns false only on allocation failure (caller keeps the old buffer). */
bool hu_daemon_cross_ctx_append_line(hu_allocator_t *alloc, char **buf, size_t *buf_len,
                                     const char *line, size_t line_len);

/* Classify the iMessage effect (slam/confetti/…) of an outbound fragment and
 * log it once at info level. No-op under HU_IS_TEST. Dedups the three identical
 * classify-effect + log blocks that lived inline in the daemon reply loop.
 * observer is an hu_observer_t* (void* here to keep this header dependency-free). */
void hu_daemon_log_send_effect(void *observer, const char *eff_ch, const char *text, size_t len);

/* Plaintext-ify `text` for the pre-split sanitize using the channel's own name
 * (hu_channel_plaintext_for_split). Returns true with an owned *out (free via
 * alloc) only on a non-empty result; false otherwise (caller keeps raw text).
 * `ch` is a struct hu_channel* (void* to keep this header dependency-free). */
bool hu_daemon_plaintext_for_split_channel(void *ch, hu_allocator_t *alloc, const char *text,
                                           size_t len, char **out, size_t *out_len);

/* Register an outbound reply in the reaction lookup so a later tapback on it
 * can produce a DPO pair (imessage_tapback source). One call per reply
 * (first fragment / first choreography segment). Resolves the chat.db GUID
 * for iMessage sends (falls back to a time-based ref) and writes the ref
 * used into msg_ref_out when provided.
 *
 * 2026-07-18 audit: registration previously lived inline ONLY on the
 * fragment branch of the daemon reply loop; the choreography branch (added
 * ~2026-05-28) sent without registering, so reaction_lookup went stale and
 * ZERO imessage_tapback pairs were ever recorded despite 119 real inbound
 * tapbacks in 30 days. Centralizing here covers every reply route.
 *
 * No-op (msg_ref_out cleared) when built without HU_ENABLE_RL_FULL or when
 * config->reaction_collection.enabled is false. `config`/`agent` may be
 * NULL in tests; the GUID lookup is skipped when config is NULL. */
void hu_daemon_register_reply_for_reactions(const struct hu_config *config, struct hu_agent *agent,
                                            const char *ch_name, const char *thread,
                                            const char *prompt, const char *response,
                                            size_t response_len, char *msg_ref_out,
                                            size_t msg_ref_cap);

#ifdef __cplusplus
}
#endif

#endif /* HU_DAEMON_MESSAGE_ROUTER_H */
