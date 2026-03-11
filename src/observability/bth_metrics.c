#include "human/observability/bth_metrics.h"
#include <stdio.h>
#include <string.h>

void hu_bth_metrics_init(hu_bth_metrics_t *m) {
    if (!m)
        return;
    memset(m, 0, sizeof(*m));
}

void hu_bth_metrics_log(const hu_bth_metrics_t *m) {
    if (!m)
        return;

    /* Only log non-zero counters */
    if (m->emotions_surfaced)
        fprintf(stderr, "[bth] emotions_surfaced=%u\n", m->emotions_surfaced);
    if (m->facts_extracted)
        fprintf(stderr, "[bth] facts_extracted=%u\n", m->facts_extracted);
    if (m->commitment_followups)
        fprintf(stderr, "[bth] commitment_followups=%u\n", m->commitment_followups);
    if (m->pattern_insights)
        fprintf(stderr, "[bth] pattern_insights=%u\n", m->pattern_insights);
    if (m->emotions_promoted)
        fprintf(stderr, "[bth] emotions_promoted=%u\n", m->emotions_promoted);
    if (m->events_extracted)
        fprintf(stderr, "[bth] events_extracted=%u\n", m->events_extracted);
    if (m->mood_contexts_built)
        fprintf(stderr, "[bth] mood_contexts_built=%u\n", m->mood_contexts_built);
    if (m->silence_checkins)
        fprintf(stderr, "[bth] silence_checkins=%u\n", m->silence_checkins);
    if (m->event_followups)
        fprintf(stderr, "[bth] event_followups=%u\n", m->event_followups);
    if (m->starters_built)
        fprintf(stderr, "[bth] starters_built=%u\n", m->starters_built);
    if (m->typos_applied)
        fprintf(stderr, "[bth] typos_applied=%u\n", m->typos_applied);
    if (m->corrections_sent)
        fprintf(stderr, "[bth] corrections_sent=%u\n", m->corrections_sent);
    if (m->thinking_responses)
        fprintf(stderr, "[bth] thinking_responses=%u\n", m->thinking_responses);
    if (m->callbacks_triggered)
        fprintf(stderr, "[bth] callbacks_triggered=%u\n", m->callbacks_triggered);
    if (m->reactions_sent)
        fprintf(stderr, "[bth] reactions_sent=%u\n", m->reactions_sent);
    if (m->link_contexts)
        fprintf(stderr, "[bth] link_contexts=%u\n", m->link_contexts);
    if (m->attachment_contexts)
        fprintf(stderr, "[bth] attachment_contexts=%u\n", m->attachment_contexts);
    if (m->ab_evaluations)
        fprintf(stderr, "[bth] ab_evaluations=%u\n", m->ab_evaluations);
    if (m->ab_alternates_chosen)
        fprintf(stderr, "[bth] ab_alternates_chosen=%u\n", m->ab_alternates_chosen);
    if (m->replay_analyses)
        fprintf(stderr, "[bth] replay_analyses=%u\n", m->replay_analyses);
    if (m->egraph_contexts)
        fprintf(stderr, "[bth] egraph_contexts=%u\n", m->egraph_contexts);
    if (m->vision_descriptions)
        fprintf(stderr, "[bth] vision_descriptions=%u\n", m->vision_descriptions);
    if (m->skills_applied)
        fprintf(stderr, "[bth] skills_applied=%u\n", m->skills_applied);
    if (m->skills_evolved)
        fprintf(stderr, "[bth] skills_evolved=%u\n", m->skills_evolved);
    if (m->skills_retired)
        fprintf(stderr, "[bth] skills_retired=%u\n", m->skills_retired);
    if (m->reflections_daily)
        fprintf(stderr, "[bth] reflections_daily=%u\n", m->reflections_daily);
    if (m->reflections_weekly)
        fprintf(stderr, "[bth] reflections_weekly=%u\n", m->reflections_weekly);
    if (m->total_turns)
        fprintf(stderr, "[bth] total_turns=%u\n", m->total_turns);
}

#define HU_BTH_SUMMARY_LINE(field, name) \
    pos += (size_t)snprintf(buf + pos, cap > pos ? cap - pos : 0, "%s=%u\n", name, m->field)

char *hu_bth_metrics_summary(hu_allocator_t *alloc, const hu_bth_metrics_t *m, size_t *out_len) {
    if (!alloc || !alloc->alloc || !m || !out_len)
        return NULL;

    /* Estimate size: ~25 fields * ~30 chars = 750, round to 1024 */
    size_t cap = 1024;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf)
        return NULL;

    size_t pos = 0;
    HU_BTH_SUMMARY_LINE(emotions_surfaced, "emotions_surfaced");
    HU_BTH_SUMMARY_LINE(facts_extracted, "facts_extracted");
    HU_BTH_SUMMARY_LINE(commitment_followups, "commitment_followups");
    HU_BTH_SUMMARY_LINE(pattern_insights, "pattern_insights");
    HU_BTH_SUMMARY_LINE(emotions_promoted, "emotions_promoted");
    HU_BTH_SUMMARY_LINE(events_extracted, "events_extracted");
    HU_BTH_SUMMARY_LINE(mood_contexts_built, "mood_contexts_built");
    HU_BTH_SUMMARY_LINE(silence_checkins, "silence_checkins");
    HU_BTH_SUMMARY_LINE(event_followups, "event_followups");
    HU_BTH_SUMMARY_LINE(starters_built, "starters_built");
    HU_BTH_SUMMARY_LINE(typos_applied, "typos_applied");
    HU_BTH_SUMMARY_LINE(corrections_sent, "corrections_sent");
    HU_BTH_SUMMARY_LINE(thinking_responses, "thinking_responses");
    HU_BTH_SUMMARY_LINE(callbacks_triggered, "callbacks_triggered");
    HU_BTH_SUMMARY_LINE(reactions_sent, "reactions_sent");
    HU_BTH_SUMMARY_LINE(link_contexts, "link_contexts");
    HU_BTH_SUMMARY_LINE(attachment_contexts, "attachment_contexts");
    HU_BTH_SUMMARY_LINE(ab_evaluations, "ab_evaluations");
    HU_BTH_SUMMARY_LINE(ab_alternates_chosen, "ab_alternates_chosen");
    HU_BTH_SUMMARY_LINE(replay_analyses, "replay_analyses");
    HU_BTH_SUMMARY_LINE(egraph_contexts, "egraph_contexts");
    HU_BTH_SUMMARY_LINE(vision_descriptions, "vision_descriptions");
    HU_BTH_SUMMARY_LINE(skills_applied, "skills_applied");
    HU_BTH_SUMMARY_LINE(skills_evolved, "skills_evolved");
    HU_BTH_SUMMARY_LINE(skills_retired, "skills_retired");
    HU_BTH_SUMMARY_LINE(reflections_daily, "reflections_daily");
    HU_BTH_SUMMARY_LINE(reflections_weekly, "reflections_weekly");
    HU_BTH_SUMMARY_LINE(total_turns, "total_turns");

    *out_len = cap; /* must match allocation size for free() */
    return buf;
}
