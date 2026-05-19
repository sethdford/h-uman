#ifndef HU_TOOLS_FACTORY_H
#define HU_TOOLS_FACTORY_H

#include "human/agent/mailbox.h"
#include "human/agent/spawn.h"
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/cron.h"
#include "human/memory.h"
#include "human/security.h"
#include "human/skillforge.h"
#include "human/tool.h"
#include <stddef.h>

hu_error_t hu_tools_create_default(hu_allocator_t *alloc, const char *workspace_dir,
                                   size_t workspace_dir_len, hu_security_policy_t *policy,
                                   const hu_config_t *config, hu_memory_t *memory,
                                   hu_cron_scheduler_t *cron, hu_agent_pool_t *agent_pool,
                                   hu_mailbox_t *mailbox, hu_skillforge_t *skillforge,
                                   hu_agent_registry_t *agent_registry, hu_tool_t **out_tools,
                                   size_t *out_count);

void hu_tools_destroy_default(hu_allocator_t *alloc, hu_tool_t *tools, size_t count);

/* Test-only observability hooks for the tool-registry honesty contract.
 *
 * hu_tools_factory_twitter_skipped_count() returns the number of times the
 * default factory has SKIPPED registering the twitter tool because no
 * credentials were configured. Process-lifetime counter, incremented by
 * hu_tools_create_default. Reset by hu_tools_factory_reset_honesty_counters
 * (test seam — production never resets).
 *
 * hu_tools_factory_lsp_skipped_count() returns the number of times the
 * default factory has SKIPPED registering the lsp tool. lsp is currently a
 * canned stub with no real implementation, so this counter increases on
 * every hu_tools_create_default call until a real LSP client lands.
 *
 * These functions exist to make the silent-config-gated subsystem rule
 * (see ~/.claude/rules/silent-config-gated-subsystems.md) testable without
 * scraping stderr or requiring an observer plumbed through the factory.
 */
unsigned hu_tools_factory_twitter_skipped_count(void);
unsigned hu_tools_factory_lsp_skipped_count(void);
void hu_tools_factory_reset_honesty_counters(void);

#endif /* HU_TOOLS_FACTORY_H */
