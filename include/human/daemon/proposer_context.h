#ifndef HU_DAEMON_PROPOSER_CONTEXT_H
#define HU_DAEMON_PROPOSER_CONTEXT_H
/*
 * Per-contact proposer context (HU_PROPOSER_CONTEXT=off|shadow|live, default off).
 *
 * The per-contact proactive proposer (daemon.c -> hu_init_proposer_tick_with_provider_ex)
 * decides "reach out to this person now?" without ever seeing the thread with
 * them: the daemon loads the last 15 messages, uses only the inbound text for
 * event extraction, and frees them. The contact's profile beyond relationship +
 * dunbar layer, how long it has been, and the contact_insights stream are absent
 * too. This module adds them as one pre-rendered block (budget
 * HU_PROPOSER_CTX_BUDGET bytes):
 *
 *   contact       — hu_contact_profile_build_context, trimmed at a line boundary
 *   conversation  — last HU_PROPOSER_CTX_THREAD_TURNS messages, "Seth:" / "them:",
 *                   relative times, days since last contact
 *   memory        — the contact_insights stream (hu_contact_insights_render),
 *                   filtered by the caller's content_is_safe predicate
 *
 * Privacy (hard rule): real message text may only reach a LOCAL model. The block
 * is built only when the agent's provider resolves to a loopback OpenAI-compatible
 * backend (the reliable wrapper's primary, circuit closed), and the enriched call
 * is made on THAT provider directly — no fallback chain. A failed local call is
 * never retried elsewhere with the block attached.
 *
 *   off    — today's call, byte-identical; nothing captured.
 *   shadow — today's call decides. Then, at most once per contact per proposer
 *            cycle and only if the production call reached the model, the
 *            enriched prompt is run once more on the local provider; ONE line
 *            "[HU_PROPOSER_CONTEXT shadow] ..." logs byte counts, should_propose,
 *            confidence and the reason LENGTH. Never sends, never records.
 *   live   — the enriched prompt on the local provider decides. On a local
 *            failure (error / empty / unparseable) the call falls back to
 *            today's un-enriched call.
 */
#include "human/agent/init_proposer.h"
#include "human/channel.h"
#include "human/core/allocator.h"
#include "human/core/gate_mode.h"
#include "human/memory.h"
#include "human/provider.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct hu_agent;
struct hu_contact_profile;

#define HU_PROPOSER_CTX_BUDGET       3072 /* whole block, headers included */
#define HU_PROPOSER_CTX_CONTACT_MAX  640
#define HU_PROPOSER_CTX_THREAD_MAX   1600
#define HU_PROPOSER_CTX_MEMORY_MAX   640
#define HU_PROPOSER_CTX_THREAD_TURNS 10
#define HU_PROPOSER_CTX_LINE_MAX     200 /* bytes of one message's text */

/* Outcome of the enriched call, for the shadow/live log line and tests. */
typedef enum hu_proposer_ctx_outcome {
    HU_PROPOSER_CTX_NONE = 0,          /* OFF, or nothing attempted */
    HU_PROPOSER_CTX_OK,                /* enriched call answered */
    HU_PROPOSER_CTX_LOCAL_UNAVAILABLE, /* no loopback provider / circuit open / call failed */
    HU_PROPOSER_CTX_RATE_LIMITED,      /* shadow already ran for this contact this cycle */
    HU_PROPOSER_CTX_NOT_REACHED,       /* production call never reached the model (gated) */
    HU_PROPOSER_CTX_EMPTY,             /* nothing to add (no thread, profile or insights) */
} hu_proposer_ctx_outcome_t;

typedef struct hu_proposer_context {
    hu_gate_mode_t mode;
    bool local_ok;              /* a loopback provider was resolved at begin() */
    hu_provider_t local;        /* borrowed; valid only when local_ok */
    int64_t days_since_last;    /* -1 = unknown */
    int64_t days_since_inbound; /* -1 = unknown */
    char contact[HU_PROPOSER_CTX_CONTACT_MAX];
    size_t contact_len;
    char thread[HU_PROPOSER_CTX_THREAD_MAX];
    size_t thread_len;
    char memory[HU_PROPOSER_CTX_MEMORY_MAX];
    size_t memory_len;
    char block[HU_PROPOSER_CTX_BUDGET + 1];
    size_t block_len;
    /* What happened on the last decide(). */
    hu_proposer_ctx_outcome_t outcome;
    bool shadow_should_propose;
    double shadow_confidence;
} hu_proposer_context_t;

/* HU_PROPOSER_CONTEXT per hu_gate_mode_parse; unset -> OFF. */
hu_gate_mode_t hu_proposer_context_mode(void);
void hu_proposer_context_set_mode_for_test(int mode); /* -1 = read env again */

/* The provider an enriched call may use: `p` itself when it is a loopback
 * OpenAI-compatible provider, or the primary of a reliable wrapper when that
 * primary is loopback and its circuit is closed. False otherwise (cloud
 * provider, cloud primary, fallback path active, NULL). */
bool hu_proposer_context_local_provider(const hu_provider_t *p, hu_provider_t *out);

/* Start one contact/channel iteration: reads the gate, resolves the local
 * provider (never in HU_IS_TEST builds — tests use begin_with_local) and, when
 * both allow it, keeps the thread. Call before the history entries are freed;
 * entries may be NULL (history load failed). */
void hu_proposer_context_begin(hu_proposer_context_t *pc, const hu_provider_t *agent_provider,
                               const hu_channel_history_entry_t *entries, size_t n,
                               int64_t now_unix);
/* Test seam: explicit mode and local provider (NULL = none). */
void hu_proposer_context_begin_with_local(hu_proposer_context_t *pc, hu_gate_mode_t mode,
                                          const hu_provider_t *local);

/* Render the thread. Pure. Entries are oldest-first (channel history order);
 * timestamps "YYYY-MM-DD HH:MM" local time. Keeps the newest lines that fit.
 * Writes days since the last message / last inbound (-1 if unknown). */
size_t hu_proposer_context_render_thread(const hu_channel_history_entry_t *entries, size_t n,
                                         int64_t now_unix, char *buf, size_t cap,
                                         int64_t *out_days_since_last,
                                         int64_t *out_days_since_inbound);

/* Keep the thread for this iteration — a no-op unless the gate is on AND a
 * local provider was resolved (the text is never captured otherwise). */
void hu_proposer_context_capture_thread(hu_proposer_context_t *pc,
                                        const hu_channel_history_entry_t *entries, size_t n,
                                        int64_t now_unix);

/* Fill contact + memory and assemble pc->block (<= HU_PROPOSER_CTX_BUDGET).
 * No-op unless the gate is on and a local provider was resolved. `memory` may
 * be NULL; `content_is_safe` (may be NULL) filters the insights. */
void hu_proposer_context_build(hu_proposer_context_t *pc, hu_allocator_t *alloc,
                               hu_memory_t *memory, const struct hu_contact_profile *cp,
                               bool (*content_is_safe)(const char *, size_t));

/* Signature of hu_init_proposer_tick_with_provider_ex (the production call). */
typedef hu_error_t (*hu_proposer_tick_fn)(
    const struct hu_initiative_config *cfg, const struct hu_autoresponder_config *ar_cfg,
    int32_t tz_offset_seconds, struct hu_proactive_budget *budget, const struct hu_agent *agent,
    struct hu_provider *provider, hu_allocator_t *alloc,
    const hu_proactive_compose_inputs_t *inputs, int64_t last_inbound_unix, int64_t now_unix,
    int64_t *last_tick_unix_inout, uint64_t *tick_id_inout, hu_init_proposer_result_t *out_result,
    hu_init_decision_t *out_decision);
void hu_proposer_context_set_tick_fn_for_test(hu_proposer_tick_fn fn); /* NULL = production */
void hu_proposer_context_reset_rate_limit_for_test(void);

/* The per-contact proposer decision. Replaces the direct _ex call in the
 * proactive loop; OFF is exactly that call. `cycle_unix` identifies the
 * proposer cycle (the proactive pass's `now`) for the shadow rate limit. */
void hu_proposer_context_decide(hu_proposer_context_t *pc, const struct hu_initiative_config *cfg,
                                const struct hu_autoresponder_config *ar_cfg, int32_t tz_offset_s,
                                struct hu_proactive_budget *budget, struct hu_agent *agent,
                                hu_provider_t *provider, hu_allocator_t *alloc,
                                const struct hu_contact_profile *cp,
                                const hu_proactive_compose_inputs_t *inputs, int64_t now_unix,
                                hu_init_proposer_result_t *out_result,
                                hu_init_decision_t *out_decision);

#endif /* HU_DAEMON_PROPOSER_CONTEXT_H */
