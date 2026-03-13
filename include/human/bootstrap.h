#ifndef HU_BOOTSTRAP_H
#define HU_BOOTSTRAP_H

#include "human/agent.h"
#include "human/bus.h"
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/daemon.h"
#include "human/provider.h"
#include "human/security.h"
#include "human/tool.h"
#include <stdbool.h>
#include <stddef.h>

/* ──────────────────────────────────────────────────────────────────────────
 * App bootstrap context — holds all subsystems created by hu_app_bootstrap.
 * Caller uses these pointers; hu_app_teardown frees everything.
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct hu_app_ctx {
    hu_allocator_t *alloc;
    hu_config_t *cfg;
    void *plugin_reg; /* hu_plugin_registry_t * */
    hu_security_policy_t *policy;
    hu_sandbox_storage_t *sb_storage;
    hu_sandbox_alloc_t sb_alloc;

    hu_tool_t *tools;
    size_t tools_count;

    /* When with_agent: provider, memory, agent, retrieval, etc. */
    hu_provider_t *provider;
    hu_memory_t *memory;
    hu_agent_t *agent;
    void *embedder;      /* hu_embedder_t * */
    void *vector_store;  /* hu_vector_store_t * */
    void *retrieval;     /* hu_retrieval_engine_t * */
    void *session_store; /* hu_session_store_t * */
    void *agent_pool;      /* hu_agent_pool_t * */
    void *mailbox;         /* hu_mailbox_t * */
    void *cron;            /* hu_cron_scheduler_t * */
    void *agent_registry;  /* hu_agent_registry_t * */
    void *skillforge;      /* hu_skillforge_t * */

    /* When with_channels: service channels for polling */
    hu_service_channel_t *channels;
    size_t channel_count;
    void *channel_instances; /* opaque storage for channel vtables; teardown uses it */

    /* Flags set by bootstrap */
    bool provider_ok;
    bool agent_ok;
} hu_app_ctx_t;

/* Initialize the full app context: load config, create provider, tools, security,
 * optionally channels and agent. config_path may be NULL (use default).
 * with_agent: create provider, memory, agent, retrieval.
 * with_channels: create service channels from config (for service-loop mode). */
hu_error_t hu_app_bootstrap(hu_app_ctx_t *ctx, hu_allocator_t *alloc, const char *config_path,
                            bool with_agent, bool with_channels);

/* Clean up everything in reverse order. Safe to call on partially-initialized ctx. */
void hu_app_teardown(hu_app_ctx_t *ctx);

#endif /* HU_BOOTSTRAP_H */
