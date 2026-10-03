/* src/core/post_send_defer.c — see include/human/core/post_send_defer.h. */

#include "human/core/post_send_defer.h"

#include "human/core/llm_purpose.h"
#include "human/core/log.h"

#include <stdint.h>
#include <time.h>

typedef struct psd_job {
    hu_post_send_job_kind_t kind;
    hu_post_send_job_fn run;
    hu_post_send_job_fn free_arg;
    void *arg;
} psd_job_t;

typedef struct psd_state {
    hu_gate_mode_t mode; /* mode of the armed window; OFF = not armed */
    bool draining;       /* flush is running the queued jobs */
    psd_job_t jobs[HU_POST_SEND_DEFER_MAX_JOBS];
    size_t count;
    size_t by_kind[HU_POST_SEND_JOB__COUNT]; /* offered this window (deferred or would-defer) */
    size_t inline_full;                      /* LIVE offers refused because the queue was full */
    size_t facts;
    size_t facts_literal;
} psd_state_t;

static _Thread_local psd_state_t s_psd;
static _Thread_local hu_post_send_defer_stats_t s_last;

hu_post_send_defer_stats_t hu_post_send_defer_last_stats(void) {
    return s_last;
}

static uint64_t psd_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

hu_gate_mode_t hu_post_send_defer_mode(void) {
    return hu_gate_mode_from_env("HU_POST_SEND_DEFER", HU_GATE_OFF);
}

bool hu_post_send_defer_armed(void) {
    return s_psd.mode != HU_GATE_OFF;
}

hu_gate_mode_t hu_post_send_defer_window_mode(void) {
    return s_psd.mode;
}

/* Run and release every queued job, FIFO, inside the background LLM lane.
 * Jobs may store rows or ingest messages: the window is already disarmed, so
 * their own offers run inline. */
static size_t psd_drain(void) {
    size_t n = s_psd.count;
    if (n == 0)
        return 0;
    psd_job_t jobs[HU_POST_SEND_DEFER_MAX_JOBS];
    for (size_t i = 0; i < n; i++)
        jobs[i] = s_psd.jobs[i];
    s_psd.count = 0;
    hu_llm_background_enter();
    for (size_t i = 0; i < n; i++) {
        jobs[i].run(jobs[i].arg);
        if (jobs[i].free_arg)
            jobs[i].free_arg(jobs[i].arg);
    }
    hu_llm_background_exit();
    return n;
}

static void psd_reset_counters(void) {
    for (size_t k = 0; k < HU_POST_SEND_JOB__COUNT; k++)
        s_psd.by_kind[k] = 0;
    s_psd.inline_full = 0;
    s_psd.facts = 0;
    s_psd.facts_literal = 0;
}

void hu_post_send_defer_begin(void) {
    /* A window whose flush was skipped: its jobs still run, late, before
     * the next turn starts. */
    if (s_psd.mode != HU_GATE_OFF || s_psd.count > 0)
        (void)hu_post_send_defer_flush();
    psd_reset_counters();
    s_psd.mode = hu_post_send_defer_mode();
}

bool hu_post_send_defer_offer(hu_post_send_job_kind_t kind, hu_post_send_job_fn run,
                              hu_post_send_job_fn free_arg, void *arg) {
    if (s_psd.mode == HU_GATE_OFF || !run || (unsigned)kind >= (unsigned)HU_POST_SEND_JOB__COUNT)
        return false;
    s_psd.by_kind[kind]++;
    if (s_psd.mode != HU_GATE_LIVE)
        return false;
    if (s_psd.count >= HU_POST_SEND_DEFER_MAX_JOBS) {
        s_psd.inline_full++;
        return false;
    }
    s_psd.jobs[s_psd.count++] =
        (psd_job_t){.kind = kind, .run = run, .free_arg = free_arg, .arg = arg};
    return true;
}

void hu_post_send_defer_note_extract(size_t facts, size_t literal) {
    if (s_psd.mode == HU_GATE_OFF && !s_psd.draining)
        return;
    s_psd.facts += facts;
    s_psd.facts_literal += literal;
}

size_t hu_post_send_defer_flush(void) {
    hu_gate_mode_t mode = s_psd.mode;
    s_psd.mode = HU_GATE_OFF; /* disarm first: jobs run inline from here */
    uint64_t t0 = psd_now_ms();
    s_psd.draining = true; /* deferred extractions still count toward facts= */
    size_t ran = psd_drain();
    s_psd.draining = false;
    if (mode == HU_GATE_OFF)
        return ran;
    s_last = (hu_post_send_defer_stats_t){.mode = mode,
                                          .extract = s_psd.by_kind[HU_POST_SEND_JOB_EXTRACT],
                                          .embed = s_psd.by_kind[HU_POST_SEND_JOB_EMBED],
                                          .ran = ran,
                                          .inline_full = s_psd.inline_full,
                                          .facts = s_psd.facts,
                                          .facts_literal = s_psd.facts_literal};
    hu_log_info("post_send_defer", NULL,
                "[HU_POST_SEND_DEFER %s] jobs=%zu extract=%zu embed=%zu ran=%zu inline_full=%zu "
                "facts=%zu facts_literal=%zu flush_ms=%llu",
                mode == HU_GATE_LIVE ? "live" : "shadow",
                s_psd.by_kind[HU_POST_SEND_JOB_EXTRACT] + s_psd.by_kind[HU_POST_SEND_JOB_EMBED],
                s_psd.by_kind[HU_POST_SEND_JOB_EXTRACT], s_psd.by_kind[HU_POST_SEND_JOB_EMBED], ran,
                s_psd.inline_full, s_psd.facts, s_psd.facts_literal,
                (unsigned long long)(psd_now_ms() - t0));
    return ran;
}
