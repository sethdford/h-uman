#ifndef HU_DAEMON_H
#define HU_DAEMON_H

#include "channel.h"
#include "channel_loop.h"
#include "core/allocator.h"
#include "core/error.h"
#include "intelligence/trust.h"
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

struct hu_agent;
struct hu_config;
struct hu_observer;

hu_error_t hu_daemon_start(void);
hu_error_t hu_daemon_stop(void);
bool hu_daemon_status(void);

/* Write/remove PID file for foreground service-loop instances.
 * Used by cmd_service_loop so hu_daemon_status() can detect running instances. */
hu_error_t hu_daemon_write_pid(void);
void hu_daemon_remove_pid(void);

/* Cron schedule matching: 5-field expression (min hour dom month dow).
   Supports star, exact, step, range, range-step, and comma lists. */
bool hu_cron_schedule_matches(const char *schedule, const struct tm *tm);

struct hu_agent;
struct hu_config;

typedef hu_error_t (*hu_channel_webhook_fn)(void *channel_ctx, hu_allocator_t *alloc,
                                            const char *body, size_t body_len);

typedef struct hu_service_channel {
    void *channel_ctx;
    hu_channel_t *channel; /* full channel vtable — used for sending replies */
    hu_channel_loop_poll_fn poll_fn;
    hu_channel_webhook_fn webhook_fn; /* optional — NULL for polling-only channels */
    uint32_t interval_ms;
    int64_t last_poll_ms;
    uint64_t last_contact_ms;
} hu_service_channel_t;

/**
 * Run the service loop: polls channels, dispatches messages to the agent,
 * sends responses back, and executes cron jobs.
 * agent may be NULL for cron-only mode.
 * config may be NULL; when non-NULL, used for per-channel persona overrides.
 * Blocks until SIGTERM/SIGINT. tick_interval_ms = 0 → default (1000ms).
 * In HU_IS_TEST mode: runs one tick and returns.
 */
hu_error_t hu_service_run(hu_allocator_t *alloc, uint32_t tick_interval_ms,
                          hu_service_channel_t *channels, size_t channel_count,
                          struct hu_agent *agent, const struct hu_config *config);

/* Run agent-type cron jobs that match the current time.
 * Iterates the agent's in-memory scheduler, fires hu_agent_turn for HU_CRON_JOB_AGENT entries,
 * and routes responses to the specified channel. */
hu_error_t hu_service_run_agent_cron(hu_allocator_t *alloc, struct hu_agent *agent,
                                     hu_service_channel_t *channels, size_t channel_count);

/* Proactive check-ins: iterate contacts with proactive_checkin=true,
 * check last interaction time, and initiate natural conversations.
 * No-op when agent has no persona loaded.
 *
 * 2026-05-26: `config` plumbed for M3 dispatch T4+T5 unified-path branch.
 * NULL is permitted — when NULL the unified-dispatch and other
 * config-gated branches fall back to the legacy hu_agent_turn path. */
void hu_service_run_proactive_checkins(hu_allocator_t *alloc, struct hu_agent *agent,
                                       hu_service_channel_t *channels, size_t channel_count,
                                       const struct hu_config *config);

/* Follow-up watcher scheduling tick (S2.1b) — schedules warmth-tiered
 * "bump" follow-ups for read-but-unreplied iMessages. Carved out of
 * hu_service_run_proactive_checkins; guarded by an in-memory msg-id dedup
 * ring AND a per-contact cooldown ledger (one bump per contact per 48h).
 * Implemented in src/daemon/daemon_followup_sched.c. */
struct hu_channel_daemon_config;

/* Per-channel daemon config block for a channel name (default block when the
 * name is NULL or unknown). Was a daemon.c static until the 2026-09-12 carve-outs
 * needed it from src/daemon/. */
const struct hu_channel_daemon_config *
hu_daemon_active_daemon_config(const struct hu_config *config, const char *ch_name);

/* Carved from hu_service_run (2026-09-12): see src/daemon/daemon_rich_media.c. */
void hu_daemon_rich_media_tick(hu_allocator_t *alloc, struct hu_agent *agent,
                               const struct hu_config *config, hu_service_channel_t *ch,
                               const char *batch_key, size_t key_len, const char *combined,
                               size_t combined_len, hu_channel_history_entry_t *history_entries,
                               size_t history_count, bool gif_sent_this_turn);

/* Carved from hu_service_run (2026-09-12): see src/daemon/daemon_voice_reply.c. */
bool hu_daemon_voice_reply(hu_allocator_t *alloc, struct hu_agent *agent,
                           const struct hu_config *config, hu_service_channel_t *ch,
                           const char *batch_key, size_t key_len, const char *combined,
                           size_t combined_len, const char *response, size_t response_len,
                           int bth_hour);

void hu_daemon_followup_sched_tick(struct hu_agent *agent, hu_service_channel_t *channels,
                                   size_t channel_count);

/* Send one due scheduled message and log the REAL outcome. Extracted from the
 * service loop (file-size ratchet); the unchecked send it replaces logged
 * "delivered" over a blue_guard HOLD (2026-07-27), so lost messages read as
 * successes. Failures log 'FAILED — entry dropped' and skip the send-recency
 * record. Implemented in src/daemon/daemon_followup_sched.c. */
void hu_daemon_sched_send_and_log(struct hu_agent *agent, struct hu_channel *channel,
                                  const char *channel_name, const char *contact, const char *msg,
                                  size_t msg_len);

hu_error_t hu_daemon_install(hu_allocator_t *alloc);
hu_error_t hu_daemon_uninstall(void);
hu_error_t hu_daemon_logs(void);

#ifdef HU_IS_TEST
struct hu_channel_daemon_config;
/* Test hook: compute photo viewing delay for batch (3–8 s when has_attachment). */
uint32_t hu_daemon_photo_viewing_delay_ms(const hu_channel_loop_msg_t *msgs, size_t batch_start,
                                          size_t batch_end, uint32_t seed);
/* Test hook: compute video viewing delay for batch (2–10 s when has_video). */
uint32_t hu_daemon_video_viewing_delay_ms(const hu_channel_loop_msg_t *msgs, size_t batch_start,
                                          size_t batch_end, uint32_t seed);
/* Test hook: per-channel daemon config lookup (see k_daemon_configs in daemon.c). */
const struct hu_channel_daemon_config *
hu_daemon_test_get_active_daemon_config(const struct hu_config *config, const char *ch_name);
#endif

/* Set the missed-message acknowledgment threshold in seconds (minimum 60s). Default: 1800 (30min)
 */
void hu_daemon_set_missed_msg_threshold(uint32_t secs);
void hu_daemon_set_missed_msg_ack_rate(double rate);
void hu_daemon_set_missed_msg_ack_cooldown(uint32_t secs);

/* Set the ceiling (seconds) above which NO missed-message acknowledgment is
 * emitted. Must exceed the threshold; otherwise rejected. Default: 86400 (24h). */
void hu_daemon_set_missed_msg_max_age(uint32_t secs);

/* Missed-message acknowledgment (F10): returns phrase or NULL if none needed.
 * NULL when delay <= threshold OR delay > max_age. */
const char *hu_missed_message_acknowledgment(int64_t delay_secs, int receive_hour, int current_hour,
                                             uint32_t seed);

/* Per-contact trust state entry (used by daemon trust cache) */
typedef struct hu_daemon_contact_trust {
    char contact_id[128];
    hu_trust_state_t state;
} hu_daemon_contact_trust_t;

/* Thread-safe per-contact trust state lookup with LRU eviction.
 * Copies existing or newly-created trust state into *out (under lock).
 * When the cache is full (4096 entries), evicts the least recently updated entry.
 * Persists mutations with hu_daemon_set_trust_state. */
hu_error_t hu_daemon_get_trust_state(const char *contact_id, size_t cid_len, hu_trust_state_t *out);

/* Write trust state for contact_id (find-or-create, same eviction rules as get). */
hu_error_t hu_daemon_set_trust_state(const char *contact_id, size_t cid_len,
                                     const hu_trust_state_t *state);

#ifdef HU_IS_TEST
/* Test helpers for trust cache */
size_t hu_daemon_trust_count(void);
void hu_daemon_trust_reset(void);
#endif

/* US-7.3 — Surface the local-inference honesty gate (INS-B).
 *
 * Emit a WARN-level log line when the daemon's personalization
 * bootstrap loads a LoRA adapter against a provider that does not
 * implement the load_adapter vtable hook (cloud providers return
 * HU_ERR_NOT_SUPPORTED). The emitted line contains the literal
 * substring "personalization adapter ignored" and the provider name,
 * so a user who configures personalization on a cloud provider is
 * never silently misled into believing the adapter is active.
 *
 * Per-process one-shot: fires once per daemon lifetime to avoid
 * log spam on reconnect or config reload. Tests that exercise the
 * warn path more than once in a single test binary must call
 * hu_daemon_personalization_warn_reset_for_test() between cases.
 *
 * provider_name may be NULL (treated as "(unknown)"). observer may
 * also be NULL — falls back to fprintf(stderr) per hu_log_warn. */
void hu_daemon_personalization_warn_adapter_ignored(struct hu_observer *observer,
                                                    const char *provider_name,
                                                    const char *adapter_id);

#ifdef HU_IS_TEST
/* Reset the per-process one-shot flag so a subsequent call to
 * hu_daemon_personalization_warn_adapter_ignored() fires again.
 * Test-only — production code must not depend on resetability. */
void hu_daemon_personalization_warn_reset_for_test(void);
#endif

/* Follow-up watcher daemon tick (US-48-3). Polls iMessage chat.db for
 * unresponded reads and schedules follow-ups via daemon_proactive. */
struct hu_follow_up_watcher_config;
struct hu_config;
struct hu_proactive_budget;
struct hu_autoresponder_config;
typedef struct hu_proactive_throttle hu_proactive_throttle_t; /* forward decl */

/* `gov_budget` and `ar_cfg` are the SHARED proactive governor state — the same
 * objects the check-in path passes at src/daemon.c:1587. They are not optional
 * here: without `gov_budget` a follow-up send is neither limited by nor counted
 * against the daily proactive budget (`hu_init_proposer_governor_check_only`
 * skips the gate on NULL, and `hu_daemon_proactive_send_and_record` only debits
 * `if (gov_budget)`), and without `ar_cfg` quiet hours are not enforced. If
 * either is NULL the `on` mode degrades to shadow and says which is missing. */
hu_error_t hu_daemon_tick_follow_up_watcher(
    const struct hu_follow_up_watcher_config *cfg, int64_t now_unix, int64_t *last_poll_unix_inout,
    int64_t *watermark_inout, struct hu_agent *agent, const struct hu_config *config,
    hu_service_channel_t *channels, size_t channel_count, hu_proactive_throttle_t *throttle,
    struct hu_proactive_budget *gov_budget, const struct hu_autoresponder_config *ar_cfg);

/* ── Follow-up watcher seams (Task 18) ──────────────────────────────────
 *
 * Gate: HU_FOLLOW_UP_WATCHER = off | shadow | on, default SHADOW.
 *
 * Minimum age of an unreplied INBOUND message before the watcher proposes
 * a follow-up. hu_follow_up_watcher_config_t carries only enabled +
 * interval_seconds, so this threshold is a compile-time constant rather
 * than a config field; it lives in the header so tests build candidates
 * relative to it instead of duplicating the number. */
#define HU_FOLLOW_UP_WATCHER_MIN_AGE_MS (6ULL * 60ULL * 60ULL * 1000ULL)

/* Finder seam. Contract matches hu_imessage_find_inbound_unreplied:
 * HU_OK with *out_msg_id == 0 means "nothing unreplied"; non-OK means the
 * query itself failed.
 *
 * This is dependency injection, NOT a test fork: the DEFAULT finder is the
 * real chat.db query and production never calls the setter. The seam
 * exists because hu_imessage_find_inbound_unreplied is compiled out under
 * HU_IS_TEST (src/channels/imessage.c:3235), so no chat.db fixture can
 * drive it from inside the test binary. */
typedef hu_error_t (*hu_follow_up_finder_fn)(void *ctx, const char *contact_id,
                                             size_t contact_id_len, int64_t *out_msg_id,
                                             uint64_t *out_inbound_at_ms);

/* Text seam for the `on` path. Writes a NUL-terminated follow-up message
 * into `out` (cap bytes) and returns HU_OK, or non-OK to decline.
 *
 * NULL by DEFAULT, and production never sets it — so `HU_FOLLOW_UP_WATCHER=on`
 * degrades to shadow plus one warning until a direction-correct text source
 * is wired. The repo has no such source today: hu_followup_compose_directive
 * (src/agent/followup_compose.c:66) and hu_followup_decide's template_text
 * both phrase the OUTBOUND case ("<contact> read your last message and
 * hasn't replied"). This watcher detects the INBOUND case (the contact
 * wrote, seth never replied), where that copy is exactly backwards and
 * would accuse a real person of ignoring a message they in fact sent. */
typedef hu_error_t (*hu_follow_up_text_fn)(void *ctx, const char *contact_id, uint64_t age_ms,
                                           char *out, size_t cap);

/* Pass fn=NULL to restore the built-in default (real finder / no text source). */
void hu_daemon_follow_up_watcher_set_finder(hu_follow_up_finder_fn fn, void *ctx);
void hu_daemon_follow_up_watcher_set_text_source(hu_follow_up_text_fn fn, void *ctx);

/* iMessage Action Surface Dispatcher (F2) — Phase A–E integration.
 *
 * Routes outbound iMessage replies through the predicate (Phase A) to choose
 * between threaded reply / flat send / tapback, with pacing (C5) and telemetry.
 * When action_surface_v2 is disabled, falls back to flat send (always-do-something).
 * Returns HU_ERR_INVALID_ARGUMENT if ch, target, body are NULL.
 * Returns HU_ERR_NOT_SUPPORTED if chosen style's vtable method is unavailable.
 * Otherwise returns the error from the chosen send path. */
struct hu_agent;
struct hu_persona;
struct hu_channel;
struct hu_conversation_snapshot;
hu_error_t hu_daemon_dispatch_imessage_reply(
    struct hu_channel *ch, const struct hu_persona *persona, const struct hu_agent *agent,
    const struct hu_config *config, const char *target, size_t target_len,
    const char *parent_msg_guid, size_t parent_guid_len, const char *body, size_t body_len,
    const struct hu_conversation_snapshot *snapshot, int64_t inferred_message_id_for_react);

#endif /* HU_DAEMON_H */
