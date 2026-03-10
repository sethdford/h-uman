#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/tool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HU_HARDWARE_INFO_NAME "hardware_info"
#define HU_HARDWARE_INFO_DESC "Get hardware board information and capabilities."
#define HU_HARDWARE_INFO_PARAMS \
    "{\"type\":\"object\",\"properties\":{\"board\":{\"type\":\"string\"}},\"required\":[]}"

static const struct {
    const char *name;
    const char *chip;
    const char *desc;
    const char *mem_map;
} boards[] = {
    {"nucleo-f401re", "STM32F401RET6",
     "ARM Cortex-M4, 84 MHz. Flash: 512 KB, RAM: 128 KB. User LED on PA5 (pin 13).",
     "Flash: 0x0800_0000 - 0x0807_FFFF (512 KB)\nRAM: 0x2000_0000 - 0x2001_FFFF (128 KB)"},
    {"nucleo-f411re", "STM32F411RET6",
     "ARM Cortex-M4, 100 MHz. Flash: 512 KB, RAM: 128 KB. User LED on PA5 (pin 13).",
     "Flash: 0x0800_0000 - 0x0807_FFFF (512 KB)\nRAM: 0x2000_0000 - 0x2001_FFFF (128 KB)"},
    {"arduino-uno", "ATmega328P",
     "8-bit AVR, 16 MHz. Flash: 16 KB, SRAM: 2 KB. Built-in LED on pin 13.",
     "Flash: 16 KB, SRAM: 2 KB, EEPROM: 1 KB"},
    {"arduino-uno-q", "STM32U585 + Qualcomm",
     "Dual-core: STM32 (MCU) + Linux (aarch64). GPIO via Bridge app on port 9999.", NULL},
    {"esp32", "ESP32",
     "Dual-core Xtensa LX6, 240 MHz. Flash: 4 MB typical. Built-in LED on GPIO 2.",
     "Flash: 4 MB, IRAM/DRAM per ESP-IDF layout"},
    {"rpi-gpio", "Raspberry Pi", "ARM Linux. Native GPIO via sysfs/rppal. No fixed LED pin.", NULL},
};
#define BOARDS_COUNT (sizeof(boards) / sizeof(boards[0]))

typedef struct hu_hardware_info_ctx {
    bool enabled;
} hu_hardware_info_ctx_t;

static hu_error_t hardware_info_execute(void *ctx, hu_allocator_t *alloc,
                                        const hu_json_value_t *args, hu_tool_result_t *out) {
    (void)ctx;
    if (!args || !out) {
        *out = hu_tool_result_fail("invalid args", 12);
        return HU_ERR_INVALID_ARGUMENT;
    }
    const char *board = hu_json_get_string(args, "board");
    if (board && board[0]) {
        for (size_t i = 0; i < BOARDS_COUNT; i++) {
            if (strcmp(boards[i].name, board) == 0) {
                size_t need = 64 + strlen(boards[i].name) + strlen(boards[i].chip) +
                              strlen(boards[i].desc) +
                              (boards[i].mem_map ? strlen(boards[i].mem_map) + 20 : 0);
                char *msg = (char *)alloc->alloc(alloc->ctx, need + 1);
                if (!msg) {
                    *out = hu_tool_result_fail("out of memory", 12);
                    return HU_ERR_OUT_OF_MEMORY;
                }
                int n;
                if (boards[i].mem_map) {
                    n = snprintf(
                        msg, need + 1,
                        "**Board:** %s\n**Chip:** %s\n**Description:** %s\n\n**Memory map:**\n%s",
                        boards[i].name, boards[i].chip, boards[i].desc, boards[i].mem_map);
                } else {
                    n = snprintf(msg, need + 1, "**Board:** %s\n**Chip:** %s\n**Description:** %s",
                                 boards[i].name, boards[i].chip, boards[i].desc);
                }
                size_t len = (n > 0 && (size_t)n <= need) ? (size_t)n : need;
                msg[len] = '\0';
                *out = hu_tool_result_ok_owned(msg, len);
                return HU_OK;
            }
        }
        size_t blen = strlen(board);
        size_t need = 52 + blen;
        char *msg = (char *)alloc->alloc(alloc->ctx, need + 1);
        if (!msg) {
            *out = hu_tool_result_fail("out of memory", 12);
            return HU_ERR_OUT_OF_MEMORY;
        }
        int n = snprintf(msg, need + 1, "Board '%s' configured. No static info available.", board);
        size_t len = (n > 0 && (size_t)n <= need) ? (size_t)n : need;
        msg[len] = '\0';
        *out = hu_tool_result_ok_owned(msg, len);
        return HU_OK;
    }
    hu_json_buf_t buf;
    if (hu_json_buf_init(&buf, alloc) != HU_OK) {
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    if (hu_json_buf_append_raw(&buf, "[", 1) != HU_OK)
        goto fail;
    for (size_t i = 0; i < BOARDS_COUNT; i++) {
        if (i > 0)
            hu_json_buf_append_raw(&buf, ",", 1);
        if (hu_json_buf_append_raw(&buf, "{\"name\":\"", 9) != HU_OK)
            goto fail;
        hu_json_append_string(&buf, boards[i].name, strlen(boards[i].name));
        if (hu_json_buf_append_raw(&buf, "\",\"chip\":\"", 10) != HU_OK)
            goto fail;
        hu_json_append_string(&buf, boards[i].chip, strlen(boards[i].chip));
        if (hu_json_buf_append_raw(&buf, "\",\"description\":\"", 17) != HU_OK)
            goto fail;
        hu_json_append_string(&buf, boards[i].desc, strlen(boards[i].desc));
        if (hu_json_buf_append_raw(&buf, "\"}", 2) != HU_OK)
            goto fail;
    }
    if (hu_json_buf_append_raw(&buf, "]", 1) != HU_OK)
        goto fail;
    char *msg = (char *)alloc->alloc(alloc->ctx, buf.len + 1);
    if (!msg) {
    fail:
        hu_json_buf_free(&buf);
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(msg, buf.ptr, buf.len);
    msg[buf.len] = '\0';
    hu_json_buf_free(&buf);
    *out = hu_tool_result_ok_owned(msg, buf.len);
    return HU_OK;
}

static const char *hardware_info_name(void *ctx) {
    (void)ctx;
    return HU_HARDWARE_INFO_NAME;
}
static const char *hardware_info_description(void *ctx) {
    (void)ctx;
    return HU_HARDWARE_INFO_DESC;
}
static const char *hardware_info_parameters_json(void *ctx) {
    (void)ctx;
    return HU_HARDWARE_INFO_PARAMS;
}
static void hardware_info_deinit(void *ctx, hu_allocator_t *alloc) {
    if (ctx)
        alloc->free(alloc->ctx, ctx, sizeof(hu_hardware_info_ctx_t));
}

static const hu_tool_vtable_t hardware_info_vtable = {
    .execute = hardware_info_execute,
    .name = hardware_info_name,
    .description = hardware_info_description,
    .parameters_json = hardware_info_parameters_json,
    .deinit = hardware_info_deinit,
};

hu_error_t hu_hardware_info_create(hu_allocator_t *alloc, bool enabled, hu_tool_t *out) {
    hu_hardware_info_ctx_t *c = (hu_hardware_info_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->enabled = enabled;
    out->ctx = c;
    out->vtable = &hardware_info_vtable;
    return HU_OK;
}
