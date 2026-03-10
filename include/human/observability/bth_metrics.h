#ifndef HU_BTH_METRICS_H
#define HU_BTH_METRICS_H

#include "human/core/allocator.h"
#include <stddef.h>
#include <stdint.h>

typedef struct hu_bth_metrics {
    /* Counters — increment per occurrence */
    uint32_t emotions_surfaced;        /* Phase 1a: emotion context injected */
    uint32_t facts_extracted;          /* Phase 1b: deep extract facts found */
    uint32_t commitment_followups;     /* Phase 1c: commitment follow-ups triggered */
    uint32_t pattern_insights;         /* Phase 1c: pattern insights triggered */
    uint32_t emotions_promoted;        /* Phase 2a: emotions stored to LTM */
    uint32_t events_extracted;         /* Phase 2b: date/events found */
    uint32_t mood_contexts_built;      /* Phase 2c: mood trend injected */
    uint32_t silence_checkins;        /* Phase 3a: silence-based check-ins */
    uint32_t event_followups;          /* Phase 3b: event-triggered outreach */
    uint32_t starters_built;           /* Phase 3c: contextual starters used */
    uint32_t typos_applied;            /* Phase 4a: typos introduced */
    uint32_t corrections_sent;         /* Phase 4b: self-corrections sent */
    uint32_t thinking_responses;       /* Phase 4c: thinking fillers sent */
    uint32_t callbacks_triggered;       /* Phase 4d: thread callbacks injected */
    uint32_t reactions_sent;           /* Phase 5: tapback reactions sent */
    uint32_t link_contexts;             /* Phase 6: link sharing contexts */
    uint32_t attachment_contexts;      /* Phase 6: attachment contexts */
    uint32_t ab_evaluations;           /* Phase 7a: A/B evaluations run */
    uint32_t ab_alternates_chosen;     /* Phase 7a: times alternate was better */
    uint32_t replay_analyses;          /* Phase 7b: replay analyses run */
    uint32_t egraph_contexts;          /* Phase 7c: emotional graph contexts */
    uint32_t vision_descriptions;       /* vision: image descriptions generated */
    uint32_t total_turns;              /* total agent turns */
} hu_bth_metrics_t;

void hu_bth_metrics_init(hu_bth_metrics_t *m);

/* Log a summary of metrics to stderr. */
void hu_bth_metrics_log(const hu_bth_metrics_t *m);

/* Build a metrics summary string. Caller owns returned string.
 * out_len is set to the byte length (including null); free with alloc->free(ctx, s, *out_len). */
char *hu_bth_metrics_summary(hu_allocator_t *alloc, const hu_bth_metrics_t *m, size_t *out_len);

#endif
