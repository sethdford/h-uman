#ifndef HU_DAEMON_REACTIVE_TURN_H
#define HU_DAEMON_REACTIVE_TURN_H

/* Batch-scoped state of one reactive reply turn in hu_service_run.
 *
 * The reactive batch-reply body of hu_service_run (src/daemon.c) is one ~9,000
 * line scope; every stretch of it shares dozens of locals with the code around
 * it, which is what kept it from being carved. This struct owns the locals that
 * cross the history/context-loading and prompt-building boundaries so those
 * stretches can move out as pure functions. Plan and measurements:
 * docs/plans/2026-09-02-daemon-batch-reply-carveout.md.
 *
 * Slice A (hu_daemon_reactive_context_load) fills the "outputs" below. The
 * remaining daemon.c code still reads the historical locals, which the caller
 * unpacks from this struct right after the call; migrating those use sites to
 * rt.* is a later mechanical pass. */

#include "human/agent/inner_thoughts.h" /* hu_inner_thought_store_t */
#include "human/channel.h"              /* hu_channel_history_entry_t */
#include "human/context/repair.h"       /* hu_repair_signal_t */
#include "human/daemon.h"               /* hu_service_channel_t */
#include "human/daemon_proactive.h"     /* hu_proactive_context_t */
#include "human/persona.h"              /* hu_contact_profile_t */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct hu_agent;
struct hu_config;

/* F27 comfort follow-through: a comfort reply we sent whose next inbound message
 * from the same contact is their reaction. Written after a comfort send, consumed
 * (scored + cleared) when that contact's next batch loads its context. Was an
 * anonymous static local of hu_service_run; named here so it can be passed by
 * pointer. Layout unchanged. */
#define HU_COMFORT_PENDING_MAX 32
typedef struct hu_daemon_comfort_pending {
    char key[64];
    char emotion[64];
    char response_type[32];
} hu_daemon_comfort_pending_t;

typedef struct hu_reactive_turn_ctx {
    /* ── Inputs: set by hu_service_run for each batch ──────────────────── */
    hu_service_channel_t *ch; /* channel the batch arrived on */
    const char *batch_key;    /* sender session key */
    size_t key_len;
    const char *combined; /* the batched inbound text */
    size_t combined_len;
    bool llm_decides; /* channels.<ch>.daemon.llm_decides */

    /* ── Loop-lifetime state the slices read/write in place ───────────── */
    hu_daemon_comfort_pending_t *comfort_pending;  /* HU_COMFORT_PENDING_MAX slots */
    hu_proactive_context_t *proactive_ctx;         /* contact activity recording */
    hu_inner_thought_store_t *inner_thought_store; /* Phase 3 inner-thought store */
    bool inner_thought_store_ok;                   /* store initialised? */
    uint32_t daemon_turn_counter;                  /* anti-sycophancy contrarian budget */
    hu_repair_signal_t *repair_signal;             /* Phase 4 repair signal (consumed here) */

    /* ── Outputs of hu_daemon_reactive_context_load ───────────────────── */
    char *contact_ctx; /* persona contact profile (+ inner world, + style notes) */
    size_t contact_ctx_len;
    char *convo_ctx; /* declared here, produced by the prompt-build slice */
    size_t convo_ctx_len;
    hu_channel_history_entry_t *history_entries; /* channel history, owned */
    size_t history_count;
    char *cross_channel_ctx; /* other platforms for the same contact */
    size_t cross_channel_ctx_len;
    const hu_contact_profile_t *contact_for_tapback;
    const hu_channel_history_entry_t *ctx_entries; /* primary-channel view */
    size_t ctx_count;
} hu_reactive_turn_ctx_t;

/* Slice A: clear the agent's history, select the active channel and persona
 * override, restore the sender's prior conversation from the session store,
 * then (outside HU_IS_TEST) load the per-contact profile, run BTH style
 * learning, load channel history, consume a pending comfort record and gather
 * cross-channel context. Fills the output fields of `rt`; every output starts
 * NULL/0. Pure move of the former daemon.c body — no behavior change. */
void hu_daemon_reactive_context_load(hu_allocator_t *alloc, struct hu_agent *agent,
                                     const struct hu_config *config, hu_service_channel_t *channels,
                                     size_t channel_count, hu_reactive_turn_ctx_t *rt);

/* Slice B: build the prompt context for this turn — the Phase 6 prefix
 * (life sim, mood, ToM, anticipatory, self-awareness, life chapter, social
 * graph, timezone, humor, inner thoughts, anti-sycophancy, repair, evolved
 * opinions, feeds, visual, relationship dynamics), then the awareness merge
 * that produces convo_ctx, then F21 topic-switch consolidation. Reads the
 * slice A outputs, writes convo_ctx/convo_ctx_len, consumes repair_signal.
 * Compiled out under HU_IS_TEST exactly as the daemon.c body was. */
void hu_daemon_reactive_prompt_build(hu_allocator_t *alloc, struct hu_agent *agent,
                                     const struct hu_config *config, hu_reactive_turn_ctx_t *rt);

#endif /* HU_DAEMON_REACTIVE_TURN_H */
