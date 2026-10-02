#ifndef HU_DAEMON_MESSAGE_ROUTER_H
#define HU_DAEMON_MESSAGE_ROUTER_H

#include "human/behavior/tapback_band.h"          /* hu_tapback_band_t */
#include "human/channels/imessage_action.h"       /* hu_reply_style_t */
#include "human/channels/imessage_action_facts.h" /* hu_conversation_snapshot_t */
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hu_config;
struct hu_agent;
struct hu_channel_loop_msg;

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

/* Roadmap #18 (stale-tapback gate, reply-style path): pure demotion. When the
 * chosen reply style would send a tapback (TAPBACK / TAPBACK_PLUS_FLAT) but
 * the parent message is older than the tapback timing band, collapse to FLAT:
 * the reaction is dropped (never sent late), the reply text still flows (a
 * late TEXT reply is normal human behavior; a late tapback is a tell).
 * parent_seconds_ago <= 0 = unknown age → style unchanged. */
hu_reply_style_t hu_daemon_demote_stale_tapback_style(hu_reply_style_t style,
                                                      int64_t parent_seconds_ago,
                                                      const hu_tapback_band_t *band);

/* Age (seconds) of an inbound message, for snapshot.parent_seconds_ago.
 * Returns 0 (= unknown) when msg_timestamp_sec is <= 0 or in the future. */
int64_t hu_daemon_snapshot_age_sec(int64_t msg_timestamp_sec);

/* Build a dispatcher snapshot for an inbound message: parent_seconds_ago
 * populated from the message origin timestamp, everything else zeroed. */
hu_conversation_snapshot_t hu_daemon_snapshot_for_msg(int64_t msg_timestamp_sec);

/* Convenience form of hu_daemon_dispatch_imessage_reply (human/daemon.h) for
 * the daemon reply loop: derives parent guid, snapshot (incl. parent age for
 * the stale-tapback demotion), and react message id from the inbound msg. */
struct hu_channel_loop_msg;

/* Same, reporting whether TEXT reached the contact: false when the dispatch
 * ended as a bare tapback, was dropped by the parrot guard, or failed. The
 * underlying form (hu_daemon_dispatch_imessage_reply in human/daemon.h) has
 * the same _ex variant. */
hu_error_t hu_daemon_dispatch_imessage_reply_msg_ex(
    void *ch, const void *persona, const struct hu_agent *agent, const struct hu_config *config,
    const char *target, size_t target_len, const struct hu_channel_loop_msg *msg, const char *body,
    size_t body_len, bool *out_text_sent, bool text_required);
struct hu_channel;
struct hu_persona;
struct hu_conversation_snapshot;
hu_error_t hu_daemon_dispatch_imessage_reply_ex(
    struct hu_channel *ch, const struct hu_persona *persona, const struct hu_agent *agent,
    const struct hu_config *config, const char *target, size_t target_len,
    const char *parent_msg_guid, size_t parent_guid_len, const char *body, size_t body_len,
    const struct hu_conversation_snapshot *snapshot, int64_t inferred_message_id_for_react,
    bool *out_text_sent, bool text_required);

/* Record one production_outcomes row for a reply that was actually DELIVERED,
 * with the text exactly as sent (after the shaping stages and the dispatch
 * decision). Until 2026-09-12 the reactive loop wrote this row when the model
 * returned — before the quality retry, the style governor, the pre-send abort
 * and the tapback/text dispatch — so `chosen` was ungoverned model text and
 * turns that sent no text (or generated twice) still landed as replies; every
 * consumer (DPO/KTO miners, eval_emotion_register, live-style queries) read it
 * as delivered text. Call from the send funnel, BEFORE
 * hu_daemon_register_reply_for_reactions (which attaches the message_ref to
 * this row). HU_OK no-op when the agent has no collector, the text or prompt
 * is empty; the SQLite write error otherwise (logged). Also hands the
 * delivered text to hu_daemon_prospective_delivered (a no-op unless
 * HU_PROSPECTIVE is shadow/live). */
hu_error_t hu_daemon_record_delivered_reply(struct hu_agent *agent, const char *ch_name,
                                            const char *target, size_t target_len,
                                            const char *prompt, size_t prompt_len, const char *text,
                                            size_t text_len);

/* Burst re-poll triage. The re-poll before a reply has already consumed every
 * message in `burst` (the channel's read cursor moved past them), so a message
 * from a sender other than `batch_key` must not be dropped: it is appended to
 * msgs[*count..cap) for the tick's batch loop to reach after the current turn.
 * Until 2026-09-30 those were silently discarded (Dermot twice on 09-24, while
 * Lexi's turn was reading). Returns how many could not be kept (msgs full),
 * each logged as a warning. */
size_t hu_daemon_burst_carry(struct hu_channel_loop_msg *msgs, size_t *count, size_t cap,
                             const struct hu_channel_loop_msg *burst, size_t burst_count,
                             const char *batch_key);

/* Where an inbound photo gets described. When the config declares a cloud
 * substitute for `model` (reliability.model_fallbacks, i.e. the primary is a
 * local model the cloud doesn't know), vision goes straight to the first
 * fallback provider with that substitute: the local primary is text-only, and
 * its 422s on every photo opened the primary's circuit breaker (2026-09-30).
 * Returns false (use the agent's own provider) when nothing is declared. */
bool hu_daemon_vision_route(const struct hu_config *cfg, const char *model, size_t model_len,
                            const char **provider_out, const char **model_out);

/* "Reply no sooner than": the director's chosen delay runs alongside the
 * turn's work instead of before it. hold() records until_ms (monotonic, as
 * hu_time_get_current_ms) for one contact, replacing any earlier hold; NULL
 * clears it. wait_ms() is how long a reply to that contact must still wait
 * (0 for any other contact or once the time has passed). The reply dispatch
 * waits it out and clears it, so a burst's later bubbles do not wait again. */
void hu_daemon_reply_hold(const char *key, size_t key_len, int64_t until_ms);
int64_t hu_daemon_reply_hold_wait_ms(const char *key, size_t key_len, int64_t now_ms);
/* hold() for `ms` from now. */
void hu_daemon_reply_hold_for(const char *key, size_t key_len, uint32_t ms);

struct hu_persona;
/* Rating-tool traffic on the owner's number (hu_share_is_tool_traffic): the
 * batch gets no reply and teaches nothing. Checked before any per-batch
 * bookkeeping. Logs when it withholds. */
bool hu_daemon_tool_traffic(const struct hu_persona *p, const char *key, size_t key_len,
                            const char *text, size_t len, void *observer);

/* 1:1 only: a hurt signal the HU_HURT_HANDOFF gate hands to the owner, so the
 * daemon does not auto-reply (see hurt_handoff.h). */
bool hu_daemon_hurt_withheld(const struct hu_persona *p, const char *key, size_t key_len,
                             const char *text, size_t len, bool is_group);

/* Inbound text whose U+FFFC attachment vision could not describe (it is not
 * `buf`, where a description is written): the placeholder is replaced by a
 * note that a picture came and did not load, kept after any text, written to
 * buf. Anything else is returned unchanged. *len is updated. */
const char *hu_daemon_unseen_photo(const char *text, size_t *len, char *buf, size_t cap);

/* Quality-retry draft. The quality gate used to free a reply before asking for
 * a better one; when the retry came back empty the contact got nothing (Lexi,
 * 2026-09-23). keep() takes ownership of the draft for `key` (dropping any
 * earlier one); settle() then always runs on the next result: an empty
 * *response for the same key gets the draft back, anything else drops it.
 * Returns true when *response is non-empty afterwards. */
void hu_daemon_quality_draft_keep(hu_allocator_t *alloc, const char *key, size_t key_len,
                                  char *draft, size_t draft_len);
bool hu_daemon_quality_draft_settle(hu_allocator_t *alloc, const char *key, size_t key_len,
                                    char **response, size_t *response_len);

/* hu_vision_describe_image on the provider hu_daemon_vision_route picks, else
 * on agent->provider with `model`. */
hu_error_t hu_daemon_describe_image(hu_allocator_t *alloc, struct hu_agent *agent,
                                    const struct hu_config *cfg, const char *path, size_t path_len,
                                    const char *model, size_t model_len, char **desc_out,
                                    size_t *desc_len);

#ifdef __cplusplus
}
#endif

#endif /* HU_DAEMON_MESSAGE_ROUTER_H */
