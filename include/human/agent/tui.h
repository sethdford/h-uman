#ifndef HU_AGENT_TUI_H
#define HU_AGENT_TUI_H

#include "human/agent.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/observer.h"
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HU_TUI_INPUT_MAX   4096
#define HU_TUI_OUTPUT_MAX  65536
#define HU_TUI_TOOL_MAX    64
#define HU_TUI_HISTORY_MAX 128
#define HU_TUI_TAB_MAX     8

typedef struct hu_tui_tool_entry {
    char name[64];
    uint64_t duration_ms;
    bool done;
    bool success;
} hu_tui_tool_entry_t;

typedef enum hu_tui_approval_state {
    HU_TUI_APPROVAL_NONE = 0,
    HU_TUI_APPROVAL_PENDING,
    HU_TUI_APPROVAL_GRANTED,
    HU_TUI_APPROVAL_DENIED,
} hu_tui_approval_state_t;

/* Per-tab snapshot: output buffer + agent history saved/restored on tab switch */
typedef struct hu_tui_tab_snapshot {
    char output_buf[HU_TUI_OUTPUT_MAX];
    size_t output_len;
    int output_scroll;
    hu_tui_tool_entry_t tool_log[HU_TUI_TOOL_MAX];
    size_t tool_log_count;
    hu_owned_message_t *history; /* saved agent history (owned) */
    size_t history_count;
    size_t history_cap;
    uint64_t total_tokens;
} hu_tui_tab_snapshot_t;

typedef struct hu_tui_state {
    hu_allocator_t *alloc;
    hu_agent_t *agent;
    const char *provider_name;
    const char *model_name;
    size_t tools_count;

    char input_buf[HU_TUI_INPUT_MAX];
    size_t input_len;
    size_t input_cursor;

    char output_buf[HU_TUI_OUTPUT_MAX];
    size_t output_len;
    int output_scroll;

    hu_tui_tool_entry_t tool_log[HU_TUI_TOOL_MAX];
    size_t tool_log_count;

    int spinner_frame;
    bool agent_running;
    volatile sig_atomic_t quit_requested;

    /* Input history (Tier 4.2) */
    char *input_history[HU_TUI_HISTORY_MAX];
    size_t input_history_count;
    int input_history_pos;

    /* Approval prompt (Tier 2.3) */
    hu_tui_approval_state_t approval;
    char approval_tool[64];
    char approval_args[256];

    /* Cost tracking (Tier 3.3) */
    double session_cost_usd;
    double daily_limit_usd;

    /* Multi-tab (Tier 3.2) */
    int active_tab;
    int tab_count;
    hu_tui_tab_snapshot_t *tabs; /* heap array of tab snapshots */
} hu_tui_state_t;

hu_error_t hu_tui_init(hu_tui_state_t *state, hu_allocator_t *alloc, hu_agent_t *agent,
                       const char *provider_name, const char *model_name, size_t tools_count);

hu_error_t hu_tui_run(hu_tui_state_t *state);

void hu_tui_deinit(hu_tui_state_t *state);

/* Observer that bridges events to TUI tool_log for live status display */
hu_observer_t hu_tui_observer_create(hu_tui_state_t *state);

#endif /* HU_AGENT_TUI_H */
