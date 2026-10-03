#ifndef HU_DAEMON_THREAD_CONTEXT_H
#define HU_DAEMON_THREAD_CONTEXT_H

#include "human/channel.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/gate_mode.h"
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/* Recent-thread block for reactive turns (HU_THREAD_CONTEXT, 2026-10-01).
 *
 * Under llm_decides the reply model's only history was the daemon-written
 * session store: no replies Seth typed himself, and 37% of 943 provider calls
 * carried msgs=2 (system prompt + the current message). The daemon already
 * loads the last 25 chat.db messages for this contact (load_conversation_history)
 * and used them only for emotion, quality and prospective memory. This module
 * renders the newest of them into conversation_context as a compact block:
 *
 *   ## Recent thread (you and Mike, oldest first)
 *   [2d ago]
 *   Mike: you around saturday?
 *   you: ya should be
 *   [5h later]
 *   Mike: [photo]
 *   ## End of recent thread
 *
 * The block is delimited by the local-only markers (providers/local_only.h),
 * so the reliable provider strips it before any cloud fallback attempt.
 *
 * Gate HU_THREAD_CONTEXT (hu_gate_mode_parse, unset -> OFF):
 *   OFF    no work, conversation_context byte-identical.
 *   SHADOW renders and logs one aggregate line; conversation_context unchanged.
 *   LIVE   appends the block to conversation_context.
 * Activation to LIVE is gated on the n=40 blind A/B (scripts/blind_ab/); see
 * docs/guides/thread-context.md for the promotion measurement. */

#define HU_THREAD_CONTEXT_MAX_LINES 15
#define HU_THREAD_CONTEXT_BUDGET    1536 /* bytes, header + lines + footer */
#define HU_THREAD_CONTEXT_TEXT_CAP  240  /* bytes of one message's text */
#define HU_THREAD_CONTEXT_GAP_SECS  1800 /* a "[Nh later]" marker above this */
#define HU_THREAD_CONTEXT_LABEL_CAP 32

typedef struct hu_thread_context_stats {
    size_t lines;         /* message lines rendered */
    size_t bytes;         /* block size; 0 when no block */
    size_t seth_lines;    /* lines labelled "you" */
    size_t dropped;       /* eligible messages cut by the line cap or byte budget */
    bool skipped_current; /* the trailing current inbound was left out */
} hu_thread_context_stats_t;

/* HU_THREAD_CONTEXT per hu_gate_mode_parse; unset -> OFF. */
hu_gate_mode_t hu_thread_context_mode(void);

/* Pure render. `entries` are chronological (oldest first), as
 * load_conversation_history returns them. `contact_name` may be NULL (label
 * "them"); only its first word is used. `current` is the inbound text the
 * model already receives as the user message: trailing contact entries whose
 * text it contains are left out. `now` anchors the relative times.
 * `*out` is NULL when nothing remains to render. */
hu_error_t hu_thread_context_render(hu_allocator_t *alloc,
                                    const hu_channel_history_entry_t *entries, size_t count,
                                    const char *contact_name, size_t contact_name_len,
                                    const char *current, size_t current_len, time_t now,
                                    size_t budget, char **out, size_t *out_len,
                                    hu_thread_context_stats_t *stats);

/* Reactive-turn entry point. OFF returns at once. `provider_local` false
 * (the reply provider's primary is a cloud model) skips the render: the block
 * is never built for a turn whose first attempt leaves the machine. SHADOW
 * logs `[HU_THREAD_CONTEXT shadow] lines= bytes= seth_lines= dropped=` and
 * leaves `*convo_ctx` untouched; LIVE appends the block (after a blank line)
 * and logs the same counts under `[HU_THREAD_CONTEXT live]`. Counts only:
 * never message text or names. `stats` may be NULL. */
void hu_daemon_thread_context_apply(hu_allocator_t *alloc, hu_gate_mode_t mode, bool provider_local,
                                    const hu_channel_history_entry_t *entries, size_t count,
                                    const char *contact_name, size_t contact_name_len,
                                    const char *current, size_t current_len, time_t now,
                                    char **convo_ctx, size_t *convo_ctx_len,
                                    hu_thread_context_stats_t *stats);

#endif /* HU_DAEMON_THREAD_CONTEXT_H */
