#ifndef HU_AGENT_FRONTIER_PROMPT_H
#define HU_AGENT_FRONTIER_PROMPT_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>

struct hu_agent;

typedef struct hu_frontier_prompt_bundle {
    char *humanness_ctx;
    size_t humanness_ctx_len;
    char *imperfect_dir;
    size_t imperfect_dir_len;
    char *residue_dir;
    size_t residue_dir_len;
    char *presence_ctx;
    size_t presence_ctx_len;
    char *micro_expr_ctx;
    size_t micro_expr_ctx_len;
    char *novelty_ctx;
    size_t novelty_ctx_len;
    char *attachment_ctx;
    size_t attachment_ctx_len;
    char *rupture_ctx;
    size_t rupture_ctx_len;
    char *narrative_self_ctx;
    size_t narrative_self_ctx_len;
    char *creative_voice_ctx;
    size_t creative_voice_ctx_len;
    char *growth_ctx;
    size_t growth_ctx_len;
    char *boundary_ctx;
    size_t boundary_ctx_len;
    char *rel_episode_ctx;
    size_t rel_episode_ctx_len;
} hu_frontier_prompt_bundle_t;

hu_error_t hu_frontier_prompt_build(hu_allocator_t *alloc, struct hu_agent *agent,
                                    const char *msg, size_t msg_len,
                                    const char *memory_ctx, size_t memory_ctx_len,
                                    hu_frontier_prompt_bundle_t *out);

void hu_frontier_prompt_free(hu_allocator_t *alloc, hu_frontier_prompt_bundle_t *b);

#endif /* HU_AGENT_FRONTIER_PROMPT_H */
