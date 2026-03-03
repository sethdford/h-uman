#include "seaclaw/agent/profile.h"
#include <string.h>

static const char *coding_tools[] = {
    "shell", "file_read", "file_write", "file_edit", "git",
    "web_search", "apply_patch", "notebook",
};

static const char *ops_tools[] = {
    "shell", "file_read", "file_write", "web_fetch",
    "cron_add", "cron_list", "schedule", "hardware_info",
};

static const char *messaging_tools[] = {
    "message", "memory_store", "memory_recall", "web_search",
};

static const char *minimal_tools[] = {
    "web_search", "memory_store", "memory_recall",
};

static const sc_agent_profile_t profiles[] = {
    {
        .type = SC_PROFILE_CODING,
        .name = "coding",
        .description = "Full-featured coding agent with file ops, git, shell, and patch tools",
        .system_prompt = "You are an expert software engineer. Write clean, tested code. "
                         "Use tools to read and edit files, run commands, and manage git.",
        .default_tools = coding_tools,
        .default_tools_count = sizeof(coding_tools) / sizeof(coding_tools[0]),
        .preferred_model = "gpt-4o",
        .temperature = 0.3,
        .max_iterations = 25,
        .max_history = 50,
        .autonomy_level = 2,
    },
    {
        .type = SC_PROFILE_OPS,
        .name = "ops",
        .description = "Operations agent for system administration, monitoring, and scheduling",
        .system_prompt = "You are a systems operations specialist. "
                         "Monitor services, schedule tasks, and maintain infrastructure.",
        .default_tools = ops_tools,
        .default_tools_count = sizeof(ops_tools) / sizeof(ops_tools[0]),
        .preferred_model = "gpt-4o-mini",
        .temperature = 0.5,
        .max_iterations = 15,
        .max_history = 30,
        .autonomy_level = 1,
    },
    {
        .type = SC_PROFILE_MESSAGING,
        .name = "messaging",
        .description = "Conversational agent for chat channels with memory and search",
        .system_prompt = "You are a helpful assistant in a team chat. "
                         "Answer questions, remember context, and search when needed.",
        .default_tools = messaging_tools,
        .default_tools_count = sizeof(messaging_tools) / sizeof(messaging_tools[0]),
        .preferred_model = "gpt-4o-mini",
        .temperature = 0.7,
        .max_iterations = 10,
        .max_history = 100,
        .autonomy_level = 1,
    },
    {
        .type = SC_PROFILE_MINIMAL,
        .name = "minimal",
        .description = "Lightweight agent with minimal tools and low resource usage",
        .system_prompt = "You are a helpful assistant. Be concise.",
        .default_tools = minimal_tools,
        .default_tools_count = sizeof(minimal_tools) / sizeof(minimal_tools[0]),
        .preferred_model = "gpt-4o-mini",
        .temperature = 0.7,
        .max_iterations = 5,
        .max_history = 20,
        .autonomy_level = 0,
    },
};

#define PROFILE_COUNT (sizeof(profiles) / sizeof(profiles[0]))

const sc_agent_profile_t *sc_agent_profile_get(sc_agent_profile_type_t type) {
    for (size_t i = 0; i < PROFILE_COUNT; i++)
        if (profiles[i].type == type) return &profiles[i];
    return NULL;
}

const sc_agent_profile_t *sc_agent_profile_by_name(const char *name, size_t name_len) {
    if (!name || name_len == 0) return NULL;
    for (size_t i = 0; i < PROFILE_COUNT; i++) {
        if (strlen(profiles[i].name) == name_len &&
            memcmp(profiles[i].name, name, name_len) == 0)
            return &profiles[i];
    }
    return NULL;
}

sc_error_t sc_agent_profile_list(const sc_agent_profile_t **out, size_t *count) {
    if (!out || !count) return SC_ERR_INVALID_ARGUMENT;
    *out = profiles;
    *count = PROFILE_COUNT;
    return SC_OK;
}
