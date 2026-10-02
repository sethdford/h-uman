#ifndef HU_DAEMON_REPLAY_TURN_H
#define HU_DAEMON_REPLAY_TURN_H

/* Real-turn replay harness (docs/guides/replay-harness.md).
 *
 * Replays one real inbound iMessage turn through the daemon's llm_decides
 * reply path — director, slice A context load, slice B prompt build, length
 * calibration, voice-first decision, director arm guard, the streaming agent
 * turn with tools off, the AI-tell and quality retries, the validator chain,
 * the send-path shaper, the outbound sanitizer and the bubble splitter — and
 * returns the text that would have been sent, without sending anything.
 *
 * Three guarantees, each pinned by tests/test_replay_turn.c:
 *   - nothing can be sent: the turn runs against a null channel whose every
 *     outbound entry only counts the call;
 *   - nothing leaves the machine: hu_replay_provider_create_local refuses any
 *     endpoint that is not loopback, and the wrapper is the only provider the
 *     CLI installs (agent + director);
 *   - arms are comparable: the wrapper pins model and (optionally)
 *     temperature on every call and fingerprints each reply request, so two
 *     arms whose requests are byte-identical show up as such. */

#include "human/agent.h"
#include "human/channel.h"
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/daemon/director.h"
#include "human/provider.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ── Loopback guard ──────────────────────────────────────────────────── */

/* True only for http(s)://127.0.0.1, ://localhost or ://[::1], with an
 * optional port and path, and no userinfo. Everything else — any other host,
 * a lookalike ("localhost.evil.com", "127.0.0.1.nip.io"), NULL, "" — is false. */
bool hu_replay_url_is_loopback(const char *url);

/* ── Null channel ────────────────────────────────────────────────────── */

typedef struct hu_replay_channel {
    const hu_channel_history_entry_t *history; /* borrowed; served to load_conversation_history */
    size_t history_count;
    uint32_t max_chars;    /* get_response_constraints (iMessage: 200) */
    size_t outbound_calls; /* send, send_event, react, reply, react_emoji, send_sticker */
    size_t typing_calls;   /* start_typing, stop_typing */
    size_t mark_read_calls;
} hu_replay_channel_t;

/* Named "imessage" so per-channel overlays, constraints and cadence rules
 * match production. Every outbound entry returns HU_ERR_NOT_SUPPORTED. */
void hu_replay_channel_init(hu_replay_channel_t *rc, const hu_channel_history_entry_t *history,
                            size_t history_count);
hu_channel_t hu_replay_channel_as_channel(hu_replay_channel_t *rc);

/* ── Pinning provider wrapper ────────────────────────────────────────── */

typedef struct hu_replay_provider {
    hu_provider_t inner;
    hu_provider_vtable_t vtable; /* inner's entries, each present one wrapped */
    bool owns_inner;             /* deinit the inner provider with the wrapper */
    char model[128];             /* forced on every call; "" = pass through */
    bool force_temperature;      /* override every call's temperature */
    double temperature;
    size_t calls;       /* every call this turn (reply, director, classifiers) */
    size_t reply_calls; /* chat / chat_with_tools / stream_chat this turn */
    uint64_t reply_fp;  /* FNV-1a of the last reply request (messages, max_tokens) */
    size_t reply_bytes; /* bytes of message content in that request */
    size_t reply_system_bytes;
    bool capture;   /* keep the last reply request's text in `captured` */
    char *captured; /* malloc'd; system prompt + messages, role-tagged */
    size_t captured_len;
} hu_replay_provider_t;

/* Wraps `inner`. `model` NULL/"" passes the caller's model through. */
void hu_replay_provider_init(hu_replay_provider_t *rp, hu_provider_t inner, bool owns_inner,
                             const char *model, bool force_temperature, double temperature);
hu_provider_t hu_replay_provider_as_provider(hu_replay_provider_t *rp);
/* Zero the per-turn counters and fingerprint (keeps configuration). */
void hu_replay_provider_reset_turn(hu_replay_provider_t *rp);
/* Free `captured` and, when owned, the inner provider. */
void hu_replay_provider_deinit(hu_replay_provider_t *rp, hu_allocator_t *alloc);

/* Create the OpenAI-compatible provider `provider_name` (e.g. "mlx_local")
 * against `endpoint`. Refuses with HU_ERR_PERMISSION_DENIED, creating
 * nothing, unless hu_replay_url_is_loopback(endpoint). */
hu_error_t hu_replay_provider_create_local(hu_allocator_t *alloc, const char *provider_name,
                                           const char *endpoint, const char *api_key,
                                           hu_provider_t *out);

/* ── One turn ────────────────────────────────────────────────────────── */

typedef struct hu_replay_turn_input {
    const char *contact_id; /* the sender's handle (the daemon's batch key) */
    const char *inbound;    /* inbound bubbles joined by '\n', as the daemon batches them */
    size_t inbound_len;
    const hu_channel_history_entry_t *history; /* thread BEFORE the inbound, oldest first */
    size_t history_count;
    bool director; /* llm_decides director call (HU_REPLAY_DIRECTOR) */
    uint32_t seed; /* shaping seed; the daemon uses time(NULL) */
} hu_replay_turn_input_t;

typedef enum hu_replay_action {
    HU_REPLAY_ACTION_TEXT = 0, /* bubbles hold the reply */
    HU_REPLAY_ACTION_TAPBACK,  /* director chose a tapback: no text turn */
    HU_REPLAY_ACTION_SILENCE,  /* director left it on read */
    HU_REPLAY_ACTION_DROPPED,  /* AI-tell after the retry, or an empty reply */
    HU_REPLAY_ACTION_ERROR,    /* the agent turn failed (see err) */
} hu_replay_action_t;

#define HU_REPLAY_MAX_BUBBLES 16

typedef struct hu_replay_turn_result {
    hu_replay_action_t action;
    hu_error_t err;
    bool director_valid;
    hu_director_result_t director;
    bool voice_memo;          /* voice-first decided a memo for this turn */
    const char *voice_reason; /* static string, or NULL */
    bool retried;             /* AI-tell or quality retry ran */
    const char *ai_tell;      /* static string of the last tell seen, or NULL */
    uint32_t max_chars;       /* agent->max_response_chars for the turn */
    char *text;               /* final text before the split; owned */
    size_t text_len;
    char *bubbles[HU_REPLAY_MAX_BUBBLES]; /* owned */
    size_t bubble_lens[HU_REPLAY_MAX_BUBBLES];
    size_t bubble_count;
    size_t provider_calls;
    size_t reply_calls;
    uint64_t reply_fp;
    size_t reply_bytes;
    size_t reply_system_bytes;
    size_t channel_outbound_calls; /* must be 0 */
} hu_replay_turn_result_t;

const char *hu_replay_action_name(hu_replay_action_t action);

/* Run one turn on `agent`, whose provider should be `rp` (the fingerprint and
 * call counts are read from it; NULL leaves them 0). The agent's session
 * store is detached for the turn — prior turns come from `in->history`, not
 * from the store — and every per-turn agent field is reset afterwards, as the
 * daemon does. HU_OK whenever a result was produced (including a failed
 * agent turn, reported as ACTION_ERROR with `err`). */
hu_error_t hu_replay_turn_run(hu_allocator_t *alloc, hu_agent_t *agent, const hu_config_t *config,
                              hu_replay_provider_t *rp, const hu_replay_turn_input_t *in,
                              hu_replay_turn_result_t *out);
void hu_replay_turn_result_deinit(hu_allocator_t *alloc, hu_replay_turn_result_t *r);

#endif /* HU_DAEMON_REPLAY_TURN_H */
