#include "seaclaw/tool.h"
#include "seaclaw/tools/hardware_memory.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/process_util.h"
#include "seaclaw/core/string.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define HARDWARE_MEMORY_PARAMS "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"read\",\"write\"]},\"address\":{\"type\":\"string\"},\"length\":{\"type\":\"integer\"},\"value\":{\"type\":\"string\"},\"board\":{\"type\":\"string\"}},\"required\":[\"action\"]}"
#define HARDWARE_MEMORY_ADDR_DEFAULT "0x20000000"
#define HARDWARE_MEMORY_LEN_DEFAULT 128
#define HARDWARE_MEMORY_LEN_MAX 256

typedef struct sc_hardware_memory_ctx {
    sc_allocator_t *alloc;
    char **boards;
    size_t boards_count;
} sc_hardware_memory_ctx_t;

static sc_error_t hardware_memory_execute(void *ctx, sc_allocator_t *alloc,
    const sc_json_value_t *args, sc_tool_result_t *out)
{
    sc_hardware_memory_ctx_t *c = (sc_hardware_memory_ctx_t *)ctx;
    if (!args || !out) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_ERR_INVALID_ARGUMENT;
    }
    if (!c->boards || c->boards_count == 0) {
        *out = sc_tool_result_fail("No peripherals configured", 27);
        return SC_OK;
    }
    const char *action = sc_json_get_string(args, "action");
    if (!action || action[0] == '\0') {
        *out = sc_tool_result_fail("Missing 'action' parameter (read or write)", 43);
        return SC_OK;
    }
    const char *address = sc_json_get_string(args, "address");
    double length_val = sc_json_get_number(args, "length", HARDWARE_MEMORY_LEN_DEFAULT);
    const char *value = sc_json_get_string(args, "value");
    const char *board = sc_json_get_string(args, "board");
    if (!address || !address[0]) address = HARDWARE_MEMORY_ADDR_DEFAULT;
    size_t length = (size_t)length_val;
    if (length < 1) length = HARDWARE_MEMORY_LEN_DEFAULT;
    if (length > HARDWARE_MEMORY_LEN_MAX) length = HARDWARE_MEMORY_LEN_MAX;
#if SC_IS_TEST
    (void)value;
    (void)board;
    if (strcmp(action, "read") == 0) {
        char *msg = sc_strndup(alloc, "00 11 22 33 44 55 66 77 88 99 AA BB CC DD EE FF", 41);
        if (!msg) { *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
        *out = sc_tool_result_ok_owned(msg, 41);
        return SC_OK;
    }
    if (strcmp(action, "write") == 0) {
        char *msg = sc_strndup(alloc, "Write OK", 8);
        if (!msg) { *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
        *out = sc_tool_result_ok_owned(msg, 8);
        return SC_OK;
    }
    *out = sc_tool_result_fail("Unknown action", 14);
    return SC_OK;
#else
    if (!board || !board[0]) board = (c->boards_count > 0) ? c->boards[0] : NULL;
    if (!board) {
        *out = sc_tool_result_fail("No board specified", 18);
        return SC_OK;
    }
    bool supported = false;
    for (size_t i = 0; i < c->boards_count; i++) {
        if (c->boards[i] && strcmp(c->boards[i], board) == 0) {
            supported = true;
            break;
        }
    }
    if (!supported) {
        *out = sc_tool_result_fail("Board not configured in peripherals list", 40);
        return SC_OK;
    }

    const char *chip = NULL;
    if (strcmp(board, "nucleo-f401re") == 0) chip = "STM32F401RETx";
    else if (strcmp(board, "nucleo-f411re") == 0) chip = "STM32F411RETx";
    else if (strcmp(board, "nucleo-l476rg") == 0) chip = "STM32L476RGTx";
    else if (strcmp(board, "nucleo-f446re") == 0) chip = "STM32F446RETx";

    if (strcmp(action, "read") == 0) {
        char addr_buf[32], len_buf[16];
        snprintf(addr_buf, sizeof(addr_buf), "%s", address);
        snprintf(len_buf, sizeof(len_buf), "%zu", length);

        const char *argv[10];
        int ai = 0;
        argv[ai++] = "probe-rs";
        argv[ai++] = "read";
        argv[ai++] = addr_buf;
        argv[ai++] = len_buf;
        if (chip) { argv[ai++] = "--chip"; argv[ai++] = chip; }
        argv[ai] = NULL;

        sc_run_result_t run = {0};
        sc_error_t err = sc_process_run(alloc, argv, NULL, 65536, &run);
        if (err != SC_OK || !run.success) {
            const char *emsg = (run.stderr_buf && run.stderr_len > 0) ?
                run.stderr_buf : "probe-rs read failed";
            size_t elen = strlen(emsg);
            if (elen > 256) elen = 256;
            *out = sc_tool_result_fail(emsg, elen);
            sc_run_result_free(alloc, &run);
            return SC_OK;
        }
        char *msg = sc_strndup(alloc, run.stdout_buf, run.stdout_len);
        size_t mlen = run.stdout_len;
        sc_run_result_free(alloc, &run);
        if (!msg) { *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
        *out = sc_tool_result_ok_owned(msg, mlen);
        return SC_OK;
    }
    if (strcmp(action, "write") == 0) {
        if (!value || strlen(value) == 0) {
            *out = sc_tool_result_fail("missing value for write", 23);
            return SC_OK;
        }
        char addr_buf[32];
        snprintf(addr_buf, sizeof(addr_buf), "%s", address);

        const char *argv[10];
        int ai = 0;
        argv[ai++] = "probe-rs";
        argv[ai++] = "write";
        argv[ai++] = addr_buf;
        argv[ai++] = value;
        if (chip) { argv[ai++] = "--chip"; argv[ai++] = chip; }
        argv[ai] = NULL;

        sc_run_result_t run = {0};
        sc_error_t err = sc_process_run(alloc, argv, NULL, 65536, &run);
        if (err != SC_OK || !run.success) {
            const char *emsg = (run.stderr_buf && run.stderr_len > 0) ?
                run.stderr_buf : "probe-rs write failed";
            size_t elen = strlen(emsg);
            if (elen > 256) elen = 256;
            *out = sc_tool_result_fail(emsg, elen);
            sc_run_result_free(alloc, &run);
            return SC_OK;
        }
        sc_run_result_free(alloc, &run);
        char *msg = sc_strndup(alloc, "Write OK", 8);
        if (!msg) { *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
        *out = sc_tool_result_ok_owned(msg, 8);
        return SC_OK;
    }
    *out = sc_tool_result_fail("Unknown action", 14);
    return SC_OK;
#endif
}

static const char *hardware_memory_name(void *ctx) { (void)ctx; return "hardware_memory"; }
static const char *hardware_memory_desc(void *ctx) {
    (void)ctx;
    return "Read/write hardware memory maps via probe-rs or serial.";
}
static const char *hardware_memory_params(void *ctx) { (void)ctx; return HARDWARE_MEMORY_PARAMS; }
static void hardware_memory_deinit(void *ctx, sc_allocator_t *alloc) {
    sc_hardware_memory_ctx_t *c = (sc_hardware_memory_ctx_t *)ctx;
    if (c && c->boards) {
        for (size_t i = 0; i < c->boards_count; i++)
            if (c->boards[i]) alloc->free(alloc->ctx, c->boards[i], strlen(c->boards[i]) + 1);
        alloc->free(alloc->ctx, c->boards, c->boards_count * sizeof(char *));
    }
    free(ctx);
}

static const sc_tool_vtable_t hardware_memory_vtable = {
    .execute = hardware_memory_execute, .name = hardware_memory_name,
    .description = hardware_memory_desc, .parameters_json = hardware_memory_params,
    .deinit = hardware_memory_deinit,
};

sc_error_t sc_hardware_memory_create(sc_allocator_t *alloc,
    const char *const *boards, size_t boards_count,
    sc_tool_t *out)
{
    if (!alloc || !out) return SC_ERR_INVALID_ARGUMENT;
    sc_hardware_memory_ctx_t *c = (sc_hardware_memory_ctx_t *)calloc(1, sizeof(*c));
    if (!c) return SC_ERR_OUT_OF_MEMORY;
    c->alloc = alloc;
    c->boards_count = boards_count;
    if (boards && boards_count > 0) {
        c->boards = (char **)alloc->alloc(alloc->ctx, boards_count * sizeof(char *));
        if (!c->boards) { free(c); return SC_ERR_OUT_OF_MEMORY; }
        memset(c->boards, 0, boards_count * sizeof(char *));
        for (size_t i = 0; i < boards_count; i++) {
            size_t len = strlen(boards[i]);
            c->boards[i] = (char *)alloc->alloc(alloc->ctx, len + 1);
            if (!c->boards[i]) {
                for (size_t j = 0; j < i; j++)
                    alloc->free(alloc->ctx, c->boards[j], strlen(c->boards[j]) + 1);
                alloc->free(alloc->ctx, c->boards, boards_count * sizeof(char *));
                free(c);
                return SC_ERR_OUT_OF_MEMORY;
            }
            memcpy(c->boards[i], boards[i], len + 1);
        }
    }
    out->ctx = c;
    out->vtable = &hardware_memory_vtable;
    return SC_OK;
}
