#include "human/tools/hardware_memory.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/process_util.h"
#include "human/core/string.h"
#include "human/tool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HARDWARE_MEMORY_PARAMS                                                                    \
    "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"read\","    \
    "\"write\"]},\"address\":{\"type\":\"string\"},\"length\":{\"type\":\"integer\"},\"value\":{" \
    "\"type\":\"string\"},\"board\":{\"type\":\"string\"}},\"required\":[\"action\"]}"
#define HARDWARE_MEMORY_ADDR_DEFAULT "0x20000000"
#define HARDWARE_MEMORY_LEN_DEFAULT  128
#define HARDWARE_MEMORY_LEN_MAX      256

typedef struct hu_hardware_memory_ctx {
    hu_allocator_t *alloc;
    char **boards;
    size_t boards_count;
} hu_hardware_memory_ctx_t;

static hu_error_t hardware_memory_execute(void *ctx, hu_allocator_t *alloc,
                                          const hu_json_value_t *args, hu_tool_result_t *out) {
    hu_hardware_memory_ctx_t *c = (hu_hardware_memory_ctx_t *)ctx;
    if (!args || !out) {
        *out = hu_tool_result_fail("invalid args", 12);
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (!c->boards || c->boards_count == 0) {
        *out = hu_tool_result_fail("No peripherals configured", 27);
        return HU_OK;
    }
    const char *action = hu_json_get_string(args, "action");
    if (!action || action[0] == '\0') {
        *out = hu_tool_result_fail("Missing 'action' parameter (read or write)", 43);
        return HU_OK;
    }
    const char *address = hu_json_get_string(args, "address");
    double length_val = hu_json_get_number(args, "length", HARDWARE_MEMORY_LEN_DEFAULT);
    const char *value = hu_json_get_string(args, "value");
    const char *board = hu_json_get_string(args, "board");
    if (!address || !address[0])
        address = HARDWARE_MEMORY_ADDR_DEFAULT;
    size_t length = (size_t)length_val;
    if (length < 1)
        length = HARDWARE_MEMORY_LEN_DEFAULT;
    if (length > HARDWARE_MEMORY_LEN_MAX)
        length = HARDWARE_MEMORY_LEN_MAX;
#if HU_IS_TEST
    (void)value;
    if (board && board[0]) {
        bool found = false;
        for (size_t i = 0; i < c->boards_count; i++) {
            if (c->boards[i] && strcmp(c->boards[i], board) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            *out = hu_tool_result_fail("Board not configured in peripherals list", 40);
            return HU_OK;
        }
    }
    if (strcmp(action, "read") == 0) {
        char *msg = hu_strndup(alloc, "00 11 22 33 44 55 66 77 88 99 AA BB CC DD EE FF", 41);
        if (!msg) {
            *out = hu_tool_result_fail("out of memory", 12);
            return HU_ERR_OUT_OF_MEMORY;
        }
        *out = hu_tool_result_ok_owned(msg, 41);
        return HU_OK;
    }
    if (strcmp(action, "write") == 0) {
        char *msg = hu_strndup(alloc, "Write OK", 8);
        if (!msg) {
            *out = hu_tool_result_fail("out of memory", 12);
            return HU_ERR_OUT_OF_MEMORY;
        }
        *out = hu_tool_result_ok_owned(msg, 8);
        return HU_OK;
    }
    *out = hu_tool_result_fail("Unknown action", 14);
    return HU_OK;
#else
    if (!board || !board[0])
        board = (c->boards_count > 0) ? c->boards[0] : NULL;
    if (!board) {
        *out = hu_tool_result_fail("No board specified", 18);
        return HU_OK;
    }
    bool supported = false;
    for (size_t i = 0; i < c->boards_count; i++) {
        if (c->boards[i] && strcmp(c->boards[i], board) == 0) {
            supported = true;
            break;
        }
    }
    if (!supported) {
        *out = hu_tool_result_fail("Board not configured in peripherals list", 40);
        return HU_OK;
    }

    const char *chip = NULL;
    if (strcmp(board, "nucleo-f401re") == 0)
        chip = "STM32F401RETx";
    else if (strcmp(board, "nucleo-f411re") == 0)
        chip = "STM32F411RETx";
    else if (strcmp(board, "nucleo-l476rg") == 0)
        chip = "STM32L476RGTx";
    else if (strcmp(board, "nucleo-f446re") == 0)
        chip = "STM32F446RETx";

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
        if (chip) {
            argv[ai++] = "--chip";
            argv[ai++] = chip;
        }
        argv[ai] = NULL;

        hu_run_result_t run = {0};
        hu_error_t err = hu_process_run(alloc, argv, NULL, 65536, &run);
        if (err != HU_OK || !run.success) {
            if (run.stderr_buf && run.stderr_len > 0) {
                size_t elen = run.stderr_len > 256 ? 256 : run.stderr_len;
                char *emsg_copy = hu_strndup(alloc, run.stderr_buf, elen);
                hu_run_result_free(alloc, &run);
                if (emsg_copy)
                    *out = hu_tool_result_fail_owned(emsg_copy, elen);
                else
                    *out = hu_tool_result_fail("probe-rs read failed", 20);
            } else {
                hu_run_result_free(alloc, &run);
                *out = hu_tool_result_fail("probe-rs read failed", 20);
            }
            return HU_OK;
        }
        char *msg = hu_strndup(alloc, run.stdout_buf, run.stdout_len);
        size_t mlen = run.stdout_len;
        hu_run_result_free(alloc, &run);
        if (!msg) {
            *out = hu_tool_result_fail("out of memory", 12);
            return HU_ERR_OUT_OF_MEMORY;
        }
        *out = hu_tool_result_ok_owned(msg, mlen);
        return HU_OK;
    }
    if (strcmp(action, "write") == 0) {
        if (!value || strlen(value) == 0) {
            *out = hu_tool_result_fail("missing value for write", 23);
            return HU_OK;
        }
        char addr_buf[32];
        snprintf(addr_buf, sizeof(addr_buf), "%s", address);

        const char *argv[10];
        int ai = 0;
        argv[ai++] = "probe-rs";
        argv[ai++] = "write";
        argv[ai++] = addr_buf;
        argv[ai++] = value;
        if (chip) {
            argv[ai++] = "--chip";
            argv[ai++] = chip;
        }
        argv[ai] = NULL;

        hu_run_result_t run = {0};
        hu_error_t err = hu_process_run(alloc, argv, NULL, 65536, &run);
        if (err != HU_OK || !run.success) {
            if (run.stderr_buf && run.stderr_len > 0) {
                size_t elen = run.stderr_len > 256 ? 256 : run.stderr_len;
                char *emsg_copy = hu_strndup(alloc, run.stderr_buf, elen);
                hu_run_result_free(alloc, &run);
                if (emsg_copy)
                    *out = hu_tool_result_fail_owned(emsg_copy, elen);
                else
                    *out = hu_tool_result_fail("probe-rs write failed", 21);
            } else {
                hu_run_result_free(alloc, &run);
                *out = hu_tool_result_fail("probe-rs write failed", 21);
            }
            return HU_OK;
        }
        hu_run_result_free(alloc, &run);
        char *msg = hu_strndup(alloc, "Write OK", 8);
        if (!msg) {
            *out = hu_tool_result_fail("out of memory", 12);
            return HU_ERR_OUT_OF_MEMORY;
        }
        *out = hu_tool_result_ok_owned(msg, 8);
        return HU_OK;
    }
    *out = hu_tool_result_fail("Unknown action", 14);
    return HU_OK;
#endif
}

static const char *hardware_memory_name(void *ctx) {
    (void)ctx;
    return "hardware_memory";
}
static const char *hardware_memory_desc(void *ctx) {
    (void)ctx;
    return "Read/write hardware memory maps via probe-rs or serial.";
}
static const char *hardware_memory_params(void *ctx) {
    (void)ctx;
    return HARDWARE_MEMORY_PARAMS;
}
static void hardware_memory_deinit(void *ctx, hu_allocator_t *alloc) {
    hu_hardware_memory_ctx_t *c = (hu_hardware_memory_ctx_t *)ctx;
    if (!c || !alloc)
        return;
    if (c->boards) {
        for (size_t i = 0; i < c->boards_count; i++)
            if (c->boards[i])
                alloc->free(alloc->ctx, c->boards[i], strlen(c->boards[i]) + 1);
        alloc->free(alloc->ctx, c->boards, c->boards_count * sizeof(char *));
    }
    alloc->free(alloc->ctx, c, sizeof(*c));
}

static const hu_tool_vtable_t hardware_memory_vtable = {
    .execute = hardware_memory_execute,
    .name = hardware_memory_name,
    .description = hardware_memory_desc,
    .parameters_json = hardware_memory_params,
    .deinit = hardware_memory_deinit,
};

hu_error_t hu_hardware_memory_create(hu_allocator_t *alloc, const char *const *boards,
                                     size_t boards_count, hu_tool_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_hardware_memory_ctx_t *c = (hu_hardware_memory_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;
    c->boards_count = boards_count;
    if (boards && boards_count > 0) {
        c->boards = (char **)alloc->alloc(alloc->ctx, boards_count * sizeof(char *));
        if (!c->boards) {
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        memset(c->boards, 0, boards_count * sizeof(char *));
        for (size_t i = 0; i < boards_count; i++) {
            size_t len = strlen(boards[i]);
            c->boards[i] = (char *)alloc->alloc(alloc->ctx, len + 1);
            if (!c->boards[i]) {
                for (size_t j = 0; j < i; j++)
                    alloc->free(alloc->ctx, c->boards[j], strlen(c->boards[j]) + 1);
                alloc->free(alloc->ctx, c->boards, boards_count * sizeof(char *));
                alloc->free(alloc->ctx, c, sizeof(*c));
                return HU_ERR_OUT_OF_MEMORY;
            }
            memcpy(c->boards[i], boards[i], len + 1);
        }
    }
    out->ctx = c;
    out->vtable = &hardware_memory_vtable;
    return HU_OK;
}
