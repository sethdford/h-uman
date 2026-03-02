#ifndef SC_AGENT_TUI_H
#define SC_AGENT_TUI_H

#include "seaclaw/core/allocator.h"
#include <stdint.h>
#include "seaclaw/core/error.h"
#include "seaclaw/observer.h"
#include "seaclaw/agent.h"
#include <stdbool.h>
#include <stddef.h>
#include <signal.h>

#define SC_TUI_INPUT_MAX     4096
#define SC_TUI_OUTPUT_MAX    65536
#define SC_TUI_TOOL_MAX      64
#define SC_TUI_HISTORY_MAX   128
#define SC_TUI_TAB_MAX       8

typedef struct sc_tui_tool_entry {
    char name[64];
    uint64_t duration_ms;
    bool done;
    bool success;
} sc_tui_tool_entry_t;

typedef enum sc_tui_approval_state {
    SC_TUI_APPROVAL_NONE = 0,
    SC_TUI_APPROVAL_PENDING,
    SC_TUI_APPROVAL_GRANTED,
    SC_TUI_APPROVAL_DENIED,
} sc_tui_approval_state_t;

/* Per-tab snapshot: output buffer + agent history saved/restored on tab switch */
typedef struct sc_tui_tab_snapshot {
    char output_buf[SC_TUI_OUTPUT_MAX];
    size_t output_len;
    int output_scroll;
    sc_tui_tool_entry_t tool_log[SC_TUI_TOOL_MAX];
    size_t tool_log_count;
    sc_owned_message_t *history;    /* saved agent history (owned) */
    size_t history_count;
    size_t history_cap;
    uint64_t total_tokens;
} sc_tui_tab_snapshot_t;

typedef struct sc_tui_state {
    sc_allocator_t *alloc;
    sc_agent_t *agent;
    const char *provider_name;
    const char *model_name;
    size_t tools_count;

    char input_buf[SC_TUI_INPUT_MAX];
    size_t input_len;
    size_t input_cursor;

    char output_buf[SC_TUI_OUTPUT_MAX];
    size_t output_len;
    int output_scroll;

    sc_tui_tool_entry_t tool_log[SC_TUI_TOOL_MAX];
    size_t tool_log_count;

    int spinner_frame;
    bool agent_running;
    volatile sig_atomic_t quit_requested;

    /* Input history (Tier 4.2) */
    char *input_history[SC_TUI_HISTORY_MAX];
    size_t input_history_count;
    int input_history_pos;

    /* Approval prompt (Tier 2.3) */
    sc_tui_approval_state_t approval;
    char approval_tool[64];
    char approval_args[256];

    /* Cost tracking (Tier 3.3) */
    double session_cost_usd;
    double daily_limit_usd;

    /* Multi-tab (Tier 3.2) */
    int active_tab;
    int tab_count;
    sc_tui_tab_snapshot_t *tabs;    /* heap array of tab snapshots */
} sc_tui_state_t;

sc_error_t sc_tui_init(sc_tui_state_t *state, sc_allocator_t *alloc,
    sc_agent_t *agent, const char *provider_name,
    const char *model_name, size_t tools_count);

sc_error_t sc_tui_run(sc_tui_state_t *state);

void sc_tui_deinit(sc_tui_state_t *state);

/* Observer that bridges events to TUI tool_log for live status display */
sc_observer_t sc_tui_observer_create(sc_tui_state_t *state);

#endif /* SC_AGENT_TUI_H */
