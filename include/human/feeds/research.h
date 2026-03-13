#ifndef HU_FEEDS_RESEARCH_H
#define HU_FEEDS_RESEARCH_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Get the research agent system prompt for analyzing feed items and
 * proposing h-uman improvements. The prompt instructs the agent to:
 * - Scan recent feed items for AI developments
 * - Cross-reference with h-uman's architecture
 * - Identify concrete improvement opportunities
 * - Prioritize by impact and feasibility
 *
 * Returns a static string (caller does not own).
 */
const char *hu_research_agent_prompt(void);

/**
 * Get the cron expression for the daily research job.
 * Default: "0 6 * * *" (6 AM daily).
 */
const char *hu_research_cron_expression(void);

/**
 * Build a research prompt that includes recent feed items as context.
 * Caller owns the returned string and must free it.
 */
hu_error_t hu_research_build_prompt(hu_allocator_t *alloc,
    const char *feed_summary, size_t feed_summary_len,
    char **out, size_t *out_len);

/**
 * Get the self-improvement system prompt used after research analysis.
 * Instructs the agent to create implementation branches, make changes,
 * and open PRs for human review. Never auto-merges.
 *
 * Returns a static string (caller does not own).
 */
const char *hu_research_self_improve_prompt(void);

/**
 * Build an improvement action prompt from a research finding.
 * Output is suitable for feeding to hu_planner_generate() or hu_agent_turn().
 * Caller owns the returned string and must free it.
 */
hu_error_t hu_research_build_action_prompt(hu_allocator_t *alloc,
    const char *finding, size_t finding_len,
    const char *suggested_action, size_t action_len,
    char **out, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif
