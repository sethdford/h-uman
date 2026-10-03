#ifndef HU_CORE_POST_SEND_DEFER_H
#define HU_CORE_POST_SEND_DEFER_H

/* HU_POST_SEND_DEFER=off|shadow|live (default off): move reply-path work whose
 * result the reply does not need until AFTER the reply is sent.
 *
 * The daemon arms a window on its reply thread right before the agent turn
 * (hu_post_send_defer_begin) and flushes it after the send or pre-send abort
 * (hu_post_send_defer_flush). Inside the armed window, work sites OFFER a job:
 *   - the LLM fact-extraction fallback in hu_personal_model_ingest (~1.4 s);
 *   - the semantic-index embedding of rows stored into SQLite memory (the
 *     duplicate per-fact embeddings of the turn's fact stores).
 *
 *   OFF    offer() returns false: the caller runs the work inline. Byte-identical.
 *   SHADOW offer() counts the job and returns false (work stays inline); flush
 *          logs one aggregate line per turn.
 *   LIVE   offer() takes ownership; flush runs the jobs FIFO on the same thread
 *          inside the background LLM lane (X-HU-Priority: batch), then logs one
 *          aggregate line.
 *
 * Every job still runs exactly once: outside an armed window, or when the
 * queue is full, offer() returns false and the caller runs it inline; begin()
 * flushes anything a skipped flush left behind before arming again.
 *
 * Thread-local: only the arming thread defers; any other thread's work runs
 * inline. Log lines carry counts only (no text, names or handles). */

#include "human/core/gate_mode.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum hu_post_send_job_kind {
    HU_POST_SEND_JOB_EXTRACT = 0,
    HU_POST_SEND_JOB_EMBED,
    HU_POST_SEND_JOB__COUNT
} hu_post_send_job_kind_t;

typedef void (*hu_post_send_job_fn)(void *arg);

#define HU_POST_SEND_DEFER_MAX_JOBS 32u

/* getenv("HU_POST_SEND_DEFER") through hu_gate_mode_from_env, default OFF. */
hu_gate_mode_t hu_post_send_defer_mode(void);

/* Arm the window for one reply turn (reads the gate once). Runs any jobs a
 * previous window left behind first. No-op arming when the gate is OFF. */
void hu_post_send_defer_begin(void);

/* True while this thread has an armed window (SHADOW or LIVE). */
bool hu_post_send_defer_armed(void);

/* Mode of this thread's armed window (OFF when not armed). A work site checks
 * it before copying its inputs: under SHADOW offer() never keeps the job, so
 * the site offers a NULL arg (counted only) and allocates nothing. */
hu_gate_mode_t hu_post_send_defer_window_mode(void);

/* Offer a job. LIVE + armed + room: the queue takes ownership of `arg`, `run`
 * is called once at flush, then `free_arg` (may be NULL); returns true and the
 * caller must NOT run the work. Otherwise returns false, `arg` stays the
 * caller's, and the caller runs the work inline (SHADOW counts it first). */
bool hu_post_send_defer_offer(hu_post_send_job_kind_t kind, hu_post_send_job_fn run,
                              hu_post_send_job_fn free_arg, void *arg);

/* Measurement for SHADOW->LIVE: an LLM extraction (inline or deferred) handed
 * `facts` facts to the personal model, `literal` of which already appear
 * verbatim in the inbound message. Counted only inside an armed window. */
void hu_post_send_defer_note_extract(size_t facts, size_t literal);

/* Disarm and run the queued jobs (LIVE). Logs one aggregate line when the
 * window was armed. Returns the number of jobs run. */
size_t hu_post_send_defer_flush(void);

/* Counts of the last flushed window: the numbers its log line carried. */
typedef struct hu_post_send_defer_stats {
    hu_gate_mode_t mode;
    size_t extract; /* extraction jobs offered (deferred, or would-defer under SHADOW) */
    size_t embed;   /* embedding jobs offered */
    size_t ran;     /* jobs the flush ran */
    size_t inline_full;
    size_t facts;         /* facts LLM extraction handed to the model this window */
    size_t facts_literal; /* ... of which the inbound already states verbatim */
} hu_post_send_defer_stats_t;

hu_post_send_defer_stats_t hu_post_send_defer_last_stats(void);

#endif /* HU_CORE_POST_SEND_DEFER_H */
