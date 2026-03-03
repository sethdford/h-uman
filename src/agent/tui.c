/*
 * Split-pane TUI for seaclaw agent sessions.
 * Uses termbox2 for terminal rendering and event handling.
 * Guarded by SC_ENABLE_TUI -- compiles to stubs otherwise.
 */
#include "seaclaw/agent/tui.h"
#include "seaclaw/core/string.h"
#include "seaclaw/design_tokens.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(SC_ENABLE_TUI) && defined(SC_GATEWAY_POSIX) && !defined(SC_IS_TEST)

#define TB_IMPL
#include "termbox2.h"
#include <pthread.h>
#include <time.h>

/* ── Colors ──────────────────────────────────────────────────────────── */
#define FG_TITLE     (TB_WHITE | TB_BOLD)
#define BG_TITLE     TB_BLUE
#define FG_STATUS    TB_WHITE
#define BG_STATUS    TB_BLUE
#define FG_OUTPUT    TB_DEFAULT
#define BG_OUTPUT    TB_DEFAULT
#define FG_INPUT     TB_DEFAULT
#define BG_INPUT     TB_DEFAULT
#define FG_PROMPT    (TB_GREEN | TB_BOLD)
#define FG_SPINNER   (TB_CYAN | TB_BOLD)
#define FG_TOOL_OK   (TB_GREEN)
#define FG_TOOL_FAIL (TB_RED)
#define FG_DIM       (TB_WHITE)

/* Markdown highlight colors */
#define FG_MD_BOLD    (TB_WHITE | TB_BOLD)
#define FG_MD_ITALIC  (TB_WHITE)
#define FG_MD_CODE    (TB_YELLOW)
#define FG_MD_HEADING (TB_CYAN | TB_BOLD)

#define FG_MD_CODEBLOCK (TB_YELLOW)
#define BG_MD_CODEBLOCK (TB_BLACK)
#define FG_DIFF_ADD     (TB_GREEN)
#define FG_DIFF_DEL     (TB_RED)

/* Approval colors */
#define FG_APPROVAL (TB_YELLOW | TB_BOLD)
#define BG_APPROVAL TB_BLACK

/* ── UTF-8 helpers ──────────────────────────────────────────────────── */
static int utf8_decode(const char *s, size_t remaining, uint32_t *out) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) {
        *out = c;
        return 1;
    }
    if ((c & 0xE0) == 0xC0 && remaining >= 2) {
        *out = ((uint32_t)(c & 0x1F) << 6) | (s[1] & 0x3F);
        return 2;
    }
    if ((c & 0xF0) == 0xE0 && remaining >= 3) {
        *out = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        return 3;
    }
    if ((c & 0xF8) == 0xF0 && remaining >= 4) {
        *out = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
               ((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        return 4;
    }
    *out = 0xFFFD;
    return 1;
}

/* ── Drawing helpers ─────────────────────────────────────────────────── */
static void draw_hline(int y, int w, uintattr_t fg, uintattr_t bg) {
    for (int x = 0; x < w; x++)
        tb_set_cell(x, y, ' ', fg, bg);
}

static void draw_text(int x, int y, const char *s, int max_w, uintattr_t fg, uintattr_t bg) {
    if (!s || max_w <= 0)
        return;
    tb_print(x, y, fg, bg, s);
    (void)max_w;
}

static const char *spinner_utf8[] = {"\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9", "\xe2\xa0\xb8",
                                     "\xe2\xa0\xbc", "\xe2\xa0\xb4", "\xe2\xa0\xa6", "\xe2\xa0\xa7",
                                     "\xe2\xa0\x87", "\xe2\xa0\x8f"};
#define SPINNER_COUNT 10

/* ── Layout calculations ─────────────────────────────────────────────── */
typedef struct tui_layout {
    int w, h;
    int title_y;
    int output_y;
    int output_h;
    int status_y;
    int input_y;
} tui_layout_t;

static tui_layout_t calc_layout(void) {
    tui_layout_t l;
    l.w = tb_width();
    l.h = tb_height();
    l.title_y = 0;
    l.output_y = 1;
    l.status_y = l.h - 2;
    l.input_y = l.h - 1;
    l.output_h = l.status_y - l.output_y;
    if (l.output_h < 1)
        l.output_h = 1;
    return l;
}

/* ── Count lines in output buffer ────────────────────────────────────── */
static int count_wrapped_lines(const char *buf, size_t len, int width) {
    if (width < 1)
        width = 1;
    int lines = 0;
    int col = 0;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            lines++;
            col = 0;
        } else {
            col++;
            if (col >= width) {
                lines++;
                col = 0;
            }
        }
    }
    if (col > 0)
        lines++;
    return lines;
}

/* ── Markdown state machine for output rendering ─────────────────────── */
typedef enum md_ctx_state {
    MD_NORMAL = 0,
    MD_HEADING,
    MD_BOLD,
    MD_ITALIC,
    MD_INLINE_CODE,
    MD_CODEBLOCK,
    MD_DIFF_ADD,
    MD_DIFF_DEL,
} md_ctx_state_t;

typedef struct md_ctx {
    md_ctx_state_t state;
    bool line_start;
    int backtick_fence;
} md_ctx_t;

static void md_fg_bg(const md_ctx_t *md, uintattr_t *fg, uintattr_t *bg) {
    switch (md->state) {
    case MD_HEADING:
        *fg = FG_MD_HEADING;
        *bg = BG_OUTPUT;
        break;
    case MD_BOLD:
        *fg = FG_MD_BOLD;
        *bg = BG_OUTPUT;
        break;
    case MD_ITALIC:
        *fg = FG_MD_ITALIC;
        *bg = BG_OUTPUT;
        break;
    case MD_INLINE_CODE:
        *fg = FG_MD_CODE;
        *bg = BG_OUTPUT;
        break;
    case MD_CODEBLOCK:
        *fg = FG_MD_CODEBLOCK;
        *bg = BG_MD_CODEBLOCK;
        break;
    case MD_DIFF_ADD:
        *fg = FG_DIFF_ADD;
        *bg = BG_OUTPUT;
        break;
    case MD_DIFF_DEL:
        *fg = FG_DIFF_DEL;
        *bg = BG_OUTPUT;
        break;
    default:
        *fg = FG_OUTPUT;
        *bg = BG_OUTPUT;
        break;
    }
}

static size_t md_advance(md_ctx_t *md, const char *buf, size_t pos, size_t len) {
    char c = buf[pos];

    if (c == '\n') {
        if (md->state == MD_HEADING || md->state == MD_DIFF_ADD || md->state == MD_DIFF_DEL)
            md->state = MD_NORMAL;
        md->line_start = true;
        return 0;
    }

    if (md->line_start) {
        md->line_start = false;

        if (md->state != MD_CODEBLOCK && pos + 2 < len && buf[pos] == '`' && buf[pos + 1] == '`' &&
            buf[pos + 2] == '`') {
            md->state = MD_CODEBLOCK;
            md->backtick_fence = 3;
            size_t skip = 3;
            while (pos + skip < len && buf[pos + skip] != '\n')
                skip++;
            return skip;
        }
        if (md->state == MD_CODEBLOCK && pos + 2 < len && buf[pos] == '`' && buf[pos + 1] == '`' &&
            buf[pos + 2] == '`') {
            md->state = MD_NORMAL;
            return 3;
        }

        if (md->state != MD_CODEBLOCK) {
            if (c == '#') {
                md->state = MD_HEADING;
                return 0;
            }
            if (c == '+' && pos + 1 < len && buf[pos + 1] == ' ') {
                md->state = MD_DIFF_ADD;
                return 0;
            }
            if (c == '-' && pos + 1 < len && buf[pos + 1] == ' ' &&
                !(pos + 2 < len && buf[pos + 2] == '-')) {
                md->state = MD_DIFF_DEL;
                return 0;
            }
            if ((c == '*' || c == '-') && pos + 1 < len && buf[pos + 1] == ' ') {
                return 0;
            }
        }
    }

    if (md->state == MD_CODEBLOCK)
        return 0;

    if (c == '`') {
        md->state = (md->state == MD_INLINE_CODE) ? MD_NORMAL : MD_INLINE_CODE;
        return 1;
    }
    if (c == '*' && pos + 1 < len && buf[pos + 1] == '*') {
        md->state = (md->state == MD_BOLD) ? MD_NORMAL : MD_BOLD;
        return 2;
    }
    if (c == '*' && md->state != MD_BOLD) {
        md->state = (md->state == MD_ITALIC) ? MD_NORMAL : MD_ITALIC;
        return 1;
    }

    return 0;
}

/* ── Draw everything ─────────────────────────────────────────────────── */
static void draw(sc_tui_state_t *state) {
    tb_clear();
    tui_layout_t l = calc_layout();

    /* Title bar */
    draw_hline(l.title_y, l.w, FG_TITLE, BG_TITLE);
    {
        char title[256];
        int n;
        int off = 0;
        off = snprintf(title, sizeof(title),
                       " SeaClaw " SC_BOX_VERT " %s/%s " SC_BOX_VERT " %zu tools",
                       state->provider_name ? state->provider_name : "?",
                       state->model_name ? state->model_name : "?", state->tools_count);
        if (state->session_cost_usd > 0.001 && off > 0 && (size_t)off < sizeof(title) - 20) {
            off += snprintf(title + off, sizeof(title) - (size_t)off, " " SC_BOX_VERT " $%.2f",
                            state->session_cost_usd);
        }
        if (state->tab_count > 1 && off > 0 && (size_t)off < sizeof(title) - 20) {
            off += snprintf(title + off, sizeof(title) - (size_t)off, " " SC_BOX_VERT " tab %d/%d",
                            state->active_tab + 1, state->tab_count);
        }
        n = off;
        if (n > 0)
            draw_text(0, l.title_y, title, l.w, FG_TITLE, BG_TITLE);
    }

    /* Output area with markdown rendering */
    {
        int total_lines = count_wrapped_lines(state->output_buf, state->output_len, l.w);
        int start_line = total_lines - l.output_h - state->output_scroll;
        if (start_line < 0)
            start_line = 0;

        int cur_line = 0;
        int draw_row = l.output_y;
        int col = 0;

        md_ctx_t md = {.state = MD_NORMAL, .line_start = true, .backtick_fence = 0};

        for (size_t i = 0; i < state->output_len && draw_row < l.status_y;) {
            char c = state->output_buf[i];

            size_t skip = md_advance(&md, state->output_buf, i, state->output_len);
            if (skip > 0) {
                i += skip;
                continue;
            }

            uintattr_t fg, bg;
            md_fg_bg(&md, &fg, &bg);

            if (cur_line >= start_line && cur_line < start_line + l.output_h) {
                if (c == '\n') {
                    draw_row++;
                    col = 0;
                    i++;
                } else {
                    uint32_t cp;
                    int cplen = utf8_decode(state->output_buf + i, state->output_len - i, &cp);
                    if (col < l.w)
                        tb_set_cell(col, draw_row, cp, fg, bg);
                    col++;
                    if (col >= l.w) {
                        draw_row++;
                        col = 0;
                    }
                    i += (size_t)cplen;
                }
            } else {
                if (c == '\n') {
                    cur_line++;
                    col = 0;
                    i++;
                } else {
                    uint32_t cp;
                    int cplen = utf8_decode(state->output_buf + i, state->output_len - i, &cp);
                    (void)cp;
                    col++;
                    if (col >= l.w) {
                        cur_line++;
                        col = 0;
                    }
                    i += (size_t)cplen;
                }
            }
        }
    }

    /* Status bar */
    draw_hline(l.status_y, l.w, FG_STATUS, BG_STATUS);

    if (state->approval == SC_TUI_APPROVAL_PENDING) {
        char status[256];
        int n = snprintf(status, sizeof(status), " \xe2\x9a\xa0 Allow %s? [y/n] args: %.80s",
                         state->approval_tool, state->approval_args);
        if (n > 0)
            draw_text(0, l.status_y, status, l.w, FG_APPROVAL, BG_STATUS);
    } else if (state->agent_running) {
        const char *frame = spinner_utf8[state->spinner_frame % SPINNER_COUNT];
        char status[256];
        int n = snprintf(status, sizeof(status), " %s Thinking...", frame);

        if (state->tool_log_count > 0) {
            sc_tui_tool_entry_t *last = &state->tool_log[state->tool_log_count - 1];
            if (last->done) {
                n = snprintf(status, sizeof(status), " %s %s (%llums)",
                             last->success ? SC_CHECK : SC_CROSS, last->name,
                             (unsigned long long)last->duration_ms);
            } else {
                n = snprintf(status, sizeof(status), " %s %s...", frame, last->name);
            }
        }
        if (n > 0)
            draw_text(0, l.status_y, status, l.w, FG_STATUS, BG_STATUS);

        char hint[] = " Ctrl+C to cancel";
        int hint_len = (int)sizeof(hint) - 1;
        if (l.w > hint_len + 2)
            draw_text(l.w - hint_len - 1, l.status_y, hint, hint_len, FG_DIM, BG_STATUS);
    } else {
        char status[128];
        int n = snprintf(status, sizeof(status), " Ready " SC_BOX_VERT " %zu messages",
                         state->agent ? state->agent->history_count : 0);
        if (n > 0)
            draw_text(0, l.status_y, status, l.w, FG_STATUS, BG_STATUS);

        char hint[] = " /help for commands";
        int hint_len = (int)sizeof(hint) - 1;
        if (l.w > hint_len + 2)
            draw_text(l.w - hint_len - 1, l.status_y, hint, hint_len, FG_DIM, BG_STATUS);
    }

    /* Input line */
    draw_hline(l.input_y, l.w, FG_INPUT, BG_INPUT);
    draw_text(0, l.input_y, "> ", 2, FG_PROMPT, BG_INPUT);
    draw_text(2, l.input_y, state->input_buf, l.w - 3, FG_INPUT, BG_INPUT);
    tb_set_cursor(2 + (int)state->input_cursor, l.input_y);

    tb_present();
}

/* ── Append text to output buffer ────────────────────────────────────── */
static void output_append(sc_tui_state_t *state, const char *text, size_t len) {
    if (!text || len == 0)
        return;
    size_t avail = SC_TUI_OUTPUT_MAX - state->output_len;
    if (len > avail) {
        size_t shift = len - avail + SC_TUI_OUTPUT_MAX / 4;
        if (shift > state->output_len)
            shift = state->output_len;
        memmove(state->output_buf, state->output_buf + shift, state->output_len - shift);
        state->output_len -= shift;
        avail = SC_TUI_OUTPUT_MAX - state->output_len;
    }
    size_t copy = len < avail ? len : avail;
    memcpy(state->output_buf + state->output_len, text, copy);
    state->output_len += copy;
    state->output_scroll = 0;
}

/* ── TUI Observer (Tier 1.1) ─────────────────────────────────────────── */
static void tui_obs_record_event(void *ctx, const sc_observer_event_t *event) {
    sc_tui_state_t *state = (sc_tui_state_t *)ctx;
    if (!state || !event)
        return;

    if (event->tag == SC_OBSERVER_EVENT_TOOL_CALL_START) {
        if (state->tool_log_count < SC_TUI_TOOL_MAX) {
            sc_tui_tool_entry_t *entry = &state->tool_log[state->tool_log_count];
            memset(entry, 0, sizeof(*entry));
            const char *name = event->data.tool_call_start.tool;
            if (name) {
                size_t n = strlen(name);
                if (n >= sizeof(entry->name))
                    n = sizeof(entry->name) - 1;
                memcpy(entry->name, name, n);
                entry->name[n] = '\0';
            }
            entry->done = false;
            state->tool_log_count++;
        }
    } else if (event->tag == SC_OBSERVER_EVENT_TOOL_CALL) {
        const char *name = event->data.tool_call.tool;
        for (size_t i = state->tool_log_count; i > 0; i--) {
            sc_tui_tool_entry_t *entry = &state->tool_log[i - 1];
            if (!entry->done && name && strncmp(entry->name, name, sizeof(entry->name)) == 0) {
                entry->done = true;
                entry->success = event->data.tool_call.success;
                entry->duration_ms = event->data.tool_call.duration_ms;

                char line[256];
                int n =
                    snprintf(line, sizeof(line), "[%s %s %llums]\n", entry->success ? "ok" : "FAIL",
                             entry->name, (unsigned long long)entry->duration_ms);
                if (n > 0)
                    output_append(state, line, (size_t)n);
                break;
            }
        }
    }
}

static void tui_obs_record_metric(void *ctx, const sc_observer_metric_t *m) {
    (void)ctx;
    (void)m;
}
static void tui_obs_flush(void *ctx) {
    (void)ctx;
}
static const char *tui_obs_name(void *ctx) {
    (void)ctx;
    return "tui";
}
static void tui_obs_deinit(void *ctx) {
    (void)ctx;
}

static const sc_observer_vtable_t tui_observer_vtable = {
    .record_event = tui_obs_record_event,
    .record_metric = tui_obs_record_metric,
    .flush = tui_obs_flush,
    .name = tui_obs_name,
    .deinit = tui_obs_deinit,
};

sc_observer_t sc_tui_observer_create(sc_tui_state_t *state) {
    sc_observer_t obs = {.ctx = state, .vtable = &tui_observer_vtable};
    return obs;
}

/* ── Streaming callback (called from agent thread) ───────────────────── */
static volatile sig_atomic_t tui_stream_started = 0;

static void tui_stream_token(const char *delta, size_t len, void *ctx) {
    sc_tui_state_t *state = (sc_tui_state_t *)ctx;
    if (!delta || len == 0 || !state)
        return;
    tui_stream_started = 1;
    output_append(state, delta, len);
}

/* ── Approval callback (called from agent thread, blocks until TUI answers) ── */
static bool tui_approval_cb(void *ctx, const char *tool_name, const char *args) {
    sc_tui_state_t *state = (sc_tui_state_t *)ctx;
    if (!state)
        return false;

    snprintf(state->approval_tool, sizeof(state->approval_tool), "%s", tool_name ? tool_name : "?");
    snprintf(state->approval_args, sizeof(state->approval_args), "%s", args ? args : "");
    state->approval = SC_TUI_APPROVAL_PENDING;

    /* Spin-wait for the TUI event loop to set GRANTED or DENIED */
    while (state->approval == SC_TUI_APPROVAL_PENDING && !state->quit_requested) {
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 50000000L};
        nanosleep(&ts, NULL);
    }

    bool granted = (state->approval == SC_TUI_APPROVAL_GRANTED);
    state->approval = SC_TUI_APPROVAL_NONE;
    return granted;
}

/* ── Agent turn thread ───────────────────────────────────────────────── */
typedef struct tui_turn_ctx {
    sc_tui_state_t *state;
    char *msg;
    size_t msg_len;
    char *response;
    size_t response_len;
    sc_error_t err;
    volatile int done;
} tui_turn_ctx_t;

static void *tui_turn_thread(void *arg) {
    tui_turn_ctx_t *ctx = (tui_turn_ctx_t *)arg;
    ctx->err = sc_agent_turn_stream(ctx->state->agent, ctx->msg, ctx->msg_len, tui_stream_token,
                                    ctx->state, &ctx->response, &ctx->response_len);
    ctx->done = 1;
    return NULL;
}

/* ── Input handling ──────────────────────────────────────────────────── */
static void input_insert(sc_tui_state_t *state, uint32_t ch) {
    if (state->input_len + 4 >= SC_TUI_INPUT_MAX)
        return;
    if (ch < 128) {
        memmove(state->input_buf + state->input_cursor + 1, state->input_buf + state->input_cursor,
                state->input_len - state->input_cursor);
        state->input_buf[state->input_cursor] = (char)ch;
        state->input_len++;
        state->input_cursor++;
        state->input_buf[state->input_len] = '\0';
    }
}

static void input_backspace(sc_tui_state_t *state) {
    if (state->input_cursor == 0)
        return;
    memmove(state->input_buf + state->input_cursor - 1, state->input_buf + state->input_cursor,
            state->input_len - state->input_cursor);
    state->input_len--;
    state->input_cursor--;
    state->input_buf[state->input_len] = '\0';
}

static void input_delete(sc_tui_state_t *state) {
    if (state->input_cursor >= state->input_len)
        return;
    memmove(state->input_buf + state->input_cursor, state->input_buf + state->input_cursor + 1,
            state->input_len - state->input_cursor - 1);
    state->input_len--;
    state->input_buf[state->input_len] = '\0';
}

static void input_clear(sc_tui_state_t *state) {
    state->input_len = 0;
    state->input_cursor = 0;
    state->input_buf[0] = '\0';
}

/* ── Input history ───────────────────────────────────────────────────── */
static void history_push(sc_tui_state_t *state, const char *msg, size_t len) {
    if (len == 0 || !msg)
        return;
    if (state->input_history_count >= SC_TUI_HISTORY_MAX) {
        free(state->input_history[0]);
        memmove(state->input_history, state->input_history + 1,
                (SC_TUI_HISTORY_MAX - 1) * sizeof(char *));
        state->input_history_count--;
    }
    state->input_history[state->input_history_count] = strndup(msg, len);
    if (state->input_history[state->input_history_count])
        state->input_history_count++;
    state->input_history_pos = (int)state->input_history_count;
}

static void history_navigate(sc_tui_state_t *state, int direction) {
    int new_pos = state->input_history_pos + direction;
    if (new_pos < 0)
        new_pos = 0;
    if (new_pos > (int)state->input_history_count)
        new_pos = (int)state->input_history_count;
    state->input_history_pos = new_pos;

    if (new_pos < (int)state->input_history_count && state->input_history[new_pos]) {
        size_t len = strlen(state->input_history[new_pos]);
        if (len >= SC_TUI_INPUT_MAX)
            len = SC_TUI_INPUT_MAX - 1;
        memcpy(state->input_buf, state->input_history[new_pos], len);
        state->input_buf[len] = '\0';
        state->input_len = len;
        state->input_cursor = len;
    } else {
        input_clear(state);
    }
}

/* ── Background task (Tier 3.4) ──────────────────────────────────────── */
typedef struct tui_bg_task {
    sc_tui_state_t *state;
    char *msg;
    size_t msg_len;
    char *response;
    size_t response_len;
    sc_error_t err;
    volatile int done;
} tui_bg_task_t;

static tui_bg_task_t *g_bg_task = NULL;
static pthread_t g_bg_tid;

static void *bg_task_thread(void *arg) {
    tui_bg_task_t *task = (tui_bg_task_t *)arg;
    task->err = sc_agent_turn(task->state->agent, task->msg, task->msg_len, &task->response,
                              &task->response_len);
    task->done = 1;
    return NULL;
}

/* ── Handle slash commands locally (Tier 1.2) ────────────────────────── */
static bool handle_slash_command(sc_tui_state_t *state) {
    if (state->input_len == 0 || state->input_buf[0] != '/')
        return false;

    /* /background command (Tier 3.4) */
    if (state->input_len > 12 && strncmp(state->input_buf, "/background ", 12) == 0) {
        if (g_bg_task && !g_bg_task->done) {
            output_append(state, "A background task is already running.\n\n", 39);
            return true;
        }
        const char *task_msg = state->input_buf + 12;
        size_t task_len = state->input_len - 12;
        g_bg_task = (tui_bg_task_t *)calloc(1, sizeof(tui_bg_task_t));
        if (g_bg_task) {
            g_bg_task->state = state;
            g_bg_task->msg = sc_strndup(state->alloc, task_msg, task_len);
            g_bg_task->msg_len = task_len;
            g_bg_task->done = 0;
            pthread_create(&g_bg_tid, NULL, bg_task_thread, g_bg_task);
            char line[256];
            int n =
                snprintf(line, sizeof(line), "[Background task started: %.80s...]\n\n", task_msg);
            if (n > 0)
                output_append(state, line, (size_t)n);
        }
        return true;
    }

    char *resp = sc_agent_handle_slash_command(state->agent, state->input_buf, state->input_len);
    if (!resp)
        return false;

    size_t resp_len = strlen(resp);
    output_append(state, resp, resp_len);
    output_append(state, "\n\n", 2);
    state->alloc->free(state->alloc->ctx, resp, resp_len + 1);

    if (state->input_len >= 4 && (strncmp(state->input_buf, "/quit", 5) == 0 ||
                                  strncmp(state->input_buf, "/exit", 5) == 0)) {
        state->quit_requested = 1;
    }

    if (state->input_len >= 6 && strncmp(state->input_buf, "/model", 6) == 0) {
        state->model_name = state->agent->model_name;
    }

    return true;
}

/* ── Tab save/restore helpers ────────────────────────────────────────── */
static void tab_save_current(sc_tui_state_t *state) {
    if (!state->tabs || state->active_tab >= state->tab_count)
        return;
    sc_tui_tab_snapshot_t *snap = &state->tabs[state->active_tab];
    memcpy(snap->output_buf, state->output_buf, state->output_len);
    snap->output_len = state->output_len;
    snap->output_scroll = state->output_scroll;
    memcpy(snap->tool_log, state->tool_log, state->tool_log_count * sizeof(sc_tui_tool_entry_t));
    snap->tool_log_count = state->tool_log_count;
    /* Save agent history by stealing the pointer */
    snap->history = state->agent->history;
    snap->history_count = state->agent->history_count;
    snap->history_cap = state->agent->history_cap;
    snap->total_tokens = state->agent->total_tokens;
    state->agent->history = NULL;
    state->agent->history_count = 0;
    state->agent->history_cap = 0;
}

static void tab_restore(sc_tui_state_t *state, int tab_idx) {
    if (!state->tabs || tab_idx >= state->tab_count)
        return;
    sc_tui_tab_snapshot_t *snap = &state->tabs[tab_idx];
    memcpy(state->output_buf, snap->output_buf, snap->output_len);
    state->output_len = snap->output_len;
    state->output_scroll = snap->output_scroll;
    memcpy(state->tool_log, snap->tool_log, snap->tool_log_count * sizeof(sc_tui_tool_entry_t));
    state->tool_log_count = snap->tool_log_count;
    /* Restore agent history */
    state->agent->history = snap->history;
    state->agent->history_count = snap->history_count;
    state->agent->history_cap = snap->history_cap;
    state->agent->total_tokens = snap->total_tokens;
    snap->history = NULL;
    snap->history_count = 0;
    snap->history_cap = 0;
    state->active_tab = tab_idx;
}

/* ── Main TUI loop ───────────────────────────────────────────────────── */
sc_error_t sc_tui_init(sc_tui_state_t *state, sc_allocator_t *alloc, sc_agent_t *agent,
                       const char *provider_name, const char *model_name, size_t tools_count) {
    if (!state || !alloc || !agent)
        return SC_ERR_INVALID_ARGUMENT;
    memset(state, 0, sizeof(*state));
    state->alloc = alloc;
    state->agent = agent;
    state->provider_name = provider_name;
    state->model_name = model_name;
    state->tools_count = tools_count;
    state->input_history_pos = 0;
    state->tab_count = 1;
    state->tabs = (sc_tui_tab_snapshot_t *)alloc->alloc(
        alloc->ctx, SC_TUI_TAB_MAX * sizeof(sc_tui_tab_snapshot_t));
    if (state->tabs)
        memset(state->tabs, 0, SC_TUI_TAB_MAX * sizeof(sc_tui_tab_snapshot_t));
    return SC_OK;
}

sc_error_t sc_tui_run(sc_tui_state_t *state) {
    if (!state)
        return SC_ERR_INVALID_ARGUMENT;

    int rc = tb_init();
    if (rc != 0)
        return SC_ERR_IO;

    /* Install TUI observer and approval callback on the agent */
    sc_observer_t tui_obs = sc_tui_observer_create(state);
    sc_observer_t *prev_observer = state->agent->observer;
    state->agent->observer = &tui_obs;
    state->agent->approval_cb = tui_approval_cb;
    state->agent->approval_ctx = state;

    {
        const char *welcome = "Welcome to SeaClaw TUI. Type a message and press Enter.\n"
                              "Ctrl+C cancels a running turn. Ctrl+D or /quit to exit.\n"
                              "Use /help for commands, arrow keys to scroll.\n\n";
        output_append(state, welcome, strlen(welcome));
    }

    tui_turn_ctx_t *active_turn = NULL;
    pthread_t turn_tid;
    int tick = 0;

    while (!state->quit_requested) {
        draw(state);

        struct tb_event ev;
        int peek_rc = tb_peek_event(&ev, 80);

        if (peek_rc == TB_OK) {
            if (ev.type == TB_EVENT_KEY) {

                /* Approval prompt handling (Tier 2.3) */
                if (state->approval == SC_TUI_APPROVAL_PENDING) {
                    if (ev.ch == 'y' || ev.ch == 'Y') {
                        state->approval = SC_TUI_APPROVAL_GRANTED;
                        output_append(state, "[Approved]\n", 11);
                    } else if (ev.ch == 'n' || ev.ch == 'N' || ev.key == TB_KEY_ESC) {
                        state->approval = SC_TUI_APPROVAL_DENIED;
                        output_append(state, "[Denied]\n", 9);
                    }
                    continue;
                }

                if (ev.key == TB_KEY_CTRL_D) {
                    if (!state->agent_running) {
                        state->quit_requested = 1;
                        continue;
                    }
                }
                if (ev.key == TB_KEY_CTRL_C) {
                    if (state->agent_running && state->agent) {
                        state->agent->cancel_requested = 1;
                        output_append(state, "\n[Cancelled]\n", 13);
                    } else {
                        state->quit_requested = 1;
                    }
                    continue;
                }

                /* Ctrl+T: new tab (Tier 3.2) */
                if (ev.key == TB_KEY_CTRL_T) {
                    if (state->tabs && state->tab_count < SC_TUI_TAB_MAX && !state->agent_running) {
                        tab_save_current(state);
                        state->tab_count++;
                        state->active_tab = state->tab_count - 1;
                        state->output_len = 0;
                        state->output_scroll = 0;
                        state->tool_log_count = 0;
                        state->agent->total_tokens = 0;
                        char msg[64];
                        int n = snprintf(msg, sizeof(msg), "New session (tab %d)\n\n",
                                         state->active_tab + 1);
                        if (n > 0)
                            output_append(state, msg, (size_t)n);
                    }
                    continue;

                    /* Alt+Left / Alt+Right: switch tabs */
                } else if (ev.key == TB_KEY_ARROW_LEFT && (ev.mod & TB_MOD_ALT)) {
                    if (state->tabs && state->tab_count > 1 && !state->agent_running) {
                        tab_save_current(state);
                        int next =
                            (state->active_tab > 0) ? state->active_tab - 1 : state->tab_count - 1;
                        tab_restore(state, next);
                    }
                    continue;
                } else if (ev.key == TB_KEY_ARROW_RIGHT && (ev.mod & TB_MOD_ALT)) {
                    if (state->tabs && state->tab_count > 1 && !state->agent_running) {
                        tab_save_current(state);
                        int next =
                            (state->active_tab < state->tab_count - 1) ? state->active_tab + 1 : 0;
                        tab_restore(state, next);
                    }
                    continue;
                }

                if (!state->agent_running) {
                    if (ev.key == TB_KEY_ENTER) {
                        if (state->input_len == 0)
                            continue;

                        if ((state->input_len == 4 && strncmp(state->input_buf, "exit", 4) == 0) ||
                            (state->input_len == 4 && strncmp(state->input_buf, "quit", 4) == 0)) {
                            state->quit_requested = 1;
                            continue;
                        }

                        char prompt_line[SC_TUI_INPUT_MAX + 8];
                        int pn = snprintf(prompt_line, sizeof(prompt_line), "> %.*s\n",
                                          (int)state->input_len, state->input_buf);
                        if (pn > 0)
                            output_append(state, prompt_line, (size_t)pn);

                        history_push(state, state->input_buf, state->input_len);

                        /* Slash command handling (Tier 1.2) */
                        if (handle_slash_command(state)) {
                            input_clear(state);
                            continue;
                        }

                        active_turn = (tui_turn_ctx_t *)calloc(1, sizeof(*active_turn));
                        if (active_turn) {
                            active_turn->state = state;
                            active_turn->msg =
                                sc_strndup(state->alloc, state->input_buf, state->input_len);
                            active_turn->msg_len = state->input_len;
                            active_turn->done = 0;
                            state->agent_running = true;
                            state->agent->cancel_requested = 0;
                            state->tool_log_count = 0;
                            tui_stream_started = 0;

                            pthread_create(&turn_tid, NULL, tui_turn_thread, active_turn);
                        }
                        input_clear(state);
                    } else if (ev.key == TB_KEY_BACKSPACE || ev.key == TB_KEY_BACKSPACE2) {
                        input_backspace(state);
                    } else if (ev.key == TB_KEY_DELETE) {
                        input_delete(state);
                    } else if (ev.key == TB_KEY_ARROW_LEFT) {
                        if (state->input_cursor > 0)
                            state->input_cursor--;
                    } else if (ev.key == TB_KEY_ARROW_RIGHT) {
                        if (state->input_cursor < state->input_len)
                            state->input_cursor++;
                    } else if (ev.key == TB_KEY_CTRL_A) {
                        state->input_cursor = 0;
                    } else if (ev.key == TB_KEY_CTRL_E) {
                        state->input_cursor = state->input_len;
                    } else if (ev.key == TB_KEY_CTRL_U) {
                        input_clear(state);
                    } else if (ev.ch != 0) {
                        input_insert(state, ev.ch);
                    }
                }

                /* Arrow up/down: scroll when agent running, history when idle */
                if (ev.key == TB_KEY_ARROW_UP) {
                    if (state->agent_running) {
                        tui_layout_t l = calc_layout();
                        int total = count_wrapped_lines(state->output_buf, state->output_len, l.w);
                        if (state->output_scroll < total - l.output_h)
                            state->output_scroll++;
                    } else {
                        history_navigate(state, -1);
                    }
                } else if (ev.key == TB_KEY_ARROW_DOWN) {
                    if (state->agent_running) {
                        if (state->output_scroll > 0)
                            state->output_scroll--;
                    } else {
                        history_navigate(state, 1);
                    }
                }
                /* Page up/down always scroll */
                if (ev.key == TB_KEY_PGUP) {
                    tui_layout_t l = calc_layout();
                    int total = count_wrapped_lines(state->output_buf, state->output_len, l.w);
                    state->output_scroll += l.output_h / 2;
                    if (state->output_scroll > total - l.output_h)
                        state->output_scroll = total - l.output_h;
                    if (state->output_scroll < 0)
                        state->output_scroll = 0;
                } else if (ev.key == TB_KEY_PGDN) {
                    state->output_scroll -= calc_layout().output_h / 2;
                    if (state->output_scroll < 0)
                        state->output_scroll = 0;
                }
            }

            if (ev.type == TB_EVENT_RESIZE) {
                /* Handled by redraw */
            }
        }

        /* Check if agent turn completed */
        if (active_turn && active_turn->done) {
            state->agent_running = false;
            pthread_join(turn_tid, NULL);

            if (active_turn->err == SC_ERR_CANCELLED) {
                output_append(state, "\n[Turn cancelled]\n\n", 19);
            } else if (active_turn->err != SC_OK) {
                const char *estr = sc_error_string(active_turn->err);
                char ebuf[256];
                int en = snprintf(ebuf, sizeof(ebuf), "\n[Error: %s]\n\n", estr);
                if (en > 0)
                    output_append(state, ebuf, (size_t)en);
            } else {
                if (!tui_stream_started && active_turn->response && active_turn->response_len > 0) {
                    output_append(state, active_turn->response, active_turn->response_len);
                }
                output_append(state, "\n\n", 2);
            }

            if (active_turn->response)
                state->alloc->free(state->alloc->ctx, active_turn->response,
                                   active_turn->response_len + 1);
            if (active_turn->msg)
                state->alloc->free(state->alloc->ctx, active_turn->msg, active_turn->msg_len + 1);
            free(active_turn);
            active_turn = NULL;
        }

        /* Check if background task completed (Tier 3.4) */
        if (g_bg_task && g_bg_task->done) {
            pthread_join(g_bg_tid, NULL);
            if (g_bg_task->err == SC_OK && g_bg_task->response) {
                output_append(state, "\n[Background complete]\n", 22);
                output_append(state, g_bg_task->response, g_bg_task->response_len);
                output_append(state, "\n\n", 2);
                state->alloc->free(state->alloc->ctx, g_bg_task->response,
                                   g_bg_task->response_len + 1);
            } else {
                const char *estr = sc_error_string(g_bg_task->err);
                char ebuf[256];
                int en = snprintf(ebuf, sizeof(ebuf), "\n[Background failed: %s]\n\n", estr);
                if (en > 0)
                    output_append(state, ebuf, (size_t)en);
            }
            if (g_bg_task->msg)
                state->alloc->free(state->alloc->ctx, g_bg_task->msg, g_bg_task->msg_len + 1);
            free(g_bg_task);
            g_bg_task = NULL;
        }

        /* Animate spinner */
        tick++;
        if (tick % 2 == 0 && state->agent_running) {
            state->spinner_frame++;
        }
    }

    if (active_turn && !active_turn->done) {
        state->agent->cancel_requested = 1;
        pthread_join(turn_tid, NULL);
        if (active_turn->msg)
            state->alloc->free(state->alloc->ctx, active_turn->msg, active_turn->msg_len + 1);
        if (active_turn->response)
            state->alloc->free(state->alloc->ctx, active_turn->response,
                               active_turn->response_len + 1);
        free(active_turn);
    }

    /* Clean up background task */
    if (g_bg_task) {
        if (!g_bg_task->done) {
            state->agent->cancel_requested = 1;
            pthread_join(g_bg_tid, NULL);
        }
        if (g_bg_task->msg)
            state->alloc->free(state->alloc->ctx, g_bg_task->msg, g_bg_task->msg_len + 1);
        if (g_bg_task->response)
            state->alloc->free(state->alloc->ctx, g_bg_task->response, g_bg_task->response_len + 1);
        free(g_bg_task);
        g_bg_task = NULL;
    }

    /* Restore previous observer */
    state->agent->observer = prev_observer;

    tb_shutdown();
    return SC_OK;
}

void sc_tui_deinit(sc_tui_state_t *state) {
    if (!state)
        return;
    for (size_t i = 0; i < state->input_history_count; i++)
        free(state->input_history[i]);
    if (state->tabs) {
        for (int t = 0; t < state->tab_count; t++) {
            sc_tui_tab_snapshot_t *snap = &state->tabs[t];
            if (snap->history) {
                for (size_t h = 0; h < snap->history_count; h++) {
                    if (snap->history[h].content)
                        state->alloc->free(state->alloc->ctx, snap->history[h].content,
                                           snap->history[h].content_len + 1);
                    if (snap->history[h].name)
                        state->alloc->free(state->alloc->ctx, snap->history[h].name,
                                           snap->history[h].name_len + 1);
                    if (snap->history[h].tool_call_id)
                        state->alloc->free(state->alloc->ctx, snap->history[h].tool_call_id,
                                           snap->history[h].tool_call_id_len + 1);
                }
                state->alloc->free(state->alloc->ctx, snap->history,
                                   snap->history_cap * sizeof(sc_owned_message_t));
            }
        }
        state->alloc->free(state->alloc->ctx, state->tabs,
                           SC_TUI_TAB_MAX * sizeof(sc_tui_tab_snapshot_t));
    }
}

#else /* !SC_ENABLE_TUI || !SC_GATEWAY_POSIX || SC_IS_TEST */

sc_error_t sc_tui_init(sc_tui_state_t *state, sc_allocator_t *alloc, sc_agent_t *agent,
                       const char *provider_name, const char *model_name, size_t tools_count) {
    if (!state || !alloc)
        return SC_ERR_INVALID_ARGUMENT;
    memset(state, 0, sizeof(*state));
    state->alloc = alloc;
    state->agent = agent;
    state->provider_name = provider_name;
    state->model_name = model_name;
    state->tools_count = tools_count;
    state->tab_count = 1;
    state->tabs = (sc_tui_tab_snapshot_t *)alloc->alloc(
        alloc->ctx, SC_TUI_TAB_MAX * sizeof(sc_tui_tab_snapshot_t));
    if (state->tabs)
        memset(state->tabs, 0, SC_TUI_TAB_MAX * sizeof(sc_tui_tab_snapshot_t));
    return SC_OK;
}

sc_error_t sc_tui_run(sc_tui_state_t *state) {
    (void)state;
    return SC_ERR_NOT_SUPPORTED;
}

void sc_tui_deinit(sc_tui_state_t *state) {
    if (!state || !state->alloc)
        return;
    for (int i = 0; i < SC_TUI_HISTORY_MAX; i++) {
        if (state->input_history[i]) {
            state->alloc->free(state->alloc->ctx, state->input_history[i],
                               strlen(state->input_history[i]) + 1);
        }
    }
    if (state->tabs) {
        state->alloc->free(state->alloc->ctx, state->tabs,
                           SC_TUI_TAB_MAX * sizeof(sc_tui_tab_snapshot_t));
    }
}

static const sc_observer_vtable_t tui_stub_observer_vtable = {
    .record_event = NULL,
    .record_metric = NULL,
    .name = NULL,
};

sc_observer_t sc_tui_observer_create(sc_tui_state_t *state) {
    sc_observer_t obs = {.ctx = state, .vtable = &tui_stub_observer_vtable};
    return obs;
}

#endif
