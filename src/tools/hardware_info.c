#include "seaclaw/tool.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define SC_HARDWARE_INFO_NAME "hardware_info"
#define SC_HARDWARE_INFO_DESC "Get hardware board information and capabilities."
#define SC_HARDWARE_INFO_PARAMS "{\"type\":\"object\",\"properties\":{\"board\":{\"type\":\"string\"}},\"required\":[]}"

static const struct { const char *name; const char *chip; const char *desc; const char *mem_map; } boards[] = {
    {"nucleo-f401re", "STM32F401RET6", "ARM Cortex-M4, 84 MHz. Flash: 512 KB, RAM: 128 KB. User LED on PA5 (pin 13).",
     "Flash: 0x0800_0000 - 0x0807_FFFF (512 KB)\nRAM: 0x2000_0000 - 0x2001_FFFF (128 KB)"},
    {"nucleo-f411re", "STM32F411RET6", "ARM Cortex-M4, 100 MHz. Flash: 512 KB, RAM: 128 KB. User LED on PA5 (pin 13).",
     "Flash: 0x0800_0000 - 0x0807_FFFF (512 KB)\nRAM: 0x2000_0000 - 0x2001_FFFF (128 KB)"},
    {"arduino-uno", "ATmega328P", "8-bit AVR, 16 MHz. Flash: 16 KB, SRAM: 2 KB. Built-in LED on pin 13.",
     "Flash: 16 KB, SRAM: 2 KB, EEPROM: 1 KB"},
    {"arduino-uno-q", "STM32U585 + Qualcomm", "Dual-core: STM32 (MCU) + Linux (aarch64). GPIO via Bridge app on port 9999.",
     NULL},
    {"esp32", "ESP32", "Dual-core Xtensa LX6, 240 MHz. Flash: 4 MB typical. Built-in LED on GPIO 2.",
     "Flash: 4 MB, IRAM/DRAM per ESP-IDF layout"},
    {"rpi-gpio", "Raspberry Pi", "ARM Linux. Native GPIO via sysfs/rppal. No fixed LED pin.",
     NULL},
};
#define BOARDS_COUNT (sizeof(boards) / sizeof(boards[0]))

typedef struct sc_hardware_info_ctx {
    bool enabled;
} sc_hardware_info_ctx_t;

static sc_error_t hardware_info_execute(void *ctx, sc_allocator_t *alloc,
    const sc_json_value_t *args,
    sc_tool_result_t *out)
{
    (void)ctx;
    if (!args || !out) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_ERR_INVALID_ARGUMENT;
    }
    const char *board = sc_json_get_string(args, "board");
    if (board && board[0]) {
        for (size_t i = 0; i < BOARDS_COUNT; i++) {
            if (strcmp(boards[i].name, board) == 0) {
                size_t need = 64 + strlen(boards[i].name) + strlen(boards[i].chip)
                    + strlen(boards[i].desc)
                    + (boards[i].mem_map ? strlen(boards[i].mem_map) + 20 : 0);
                char *msg = (char *)alloc->alloc(alloc->ctx, need + 1);
                if (!msg) { *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
                int n;
                if (boards[i].mem_map) {
                    n = snprintf(msg, need + 1,
                        "**Board:** %s\n**Chip:** %s\n**Description:** %s\n\n**Memory map:**\n%s",
                        boards[i].name, boards[i].chip, boards[i].desc, boards[i].mem_map);
                } else {
                    n = snprintf(msg, need + 1,
                        "**Board:** %s\n**Chip:** %s\n**Description:** %s",
                        boards[i].name, boards[i].chip, boards[i].desc);
                }
                size_t len = (n > 0 && (size_t)n <= need) ? (size_t)n : need;
                msg[len] = '\0';
                *out = sc_tool_result_ok_owned(msg, len);
                return SC_OK;
            }
        }
        size_t blen = strlen(board);
        size_t need = 52 + blen;
        char *msg = (char *)alloc->alloc(alloc->ctx, need + 1);
        if (!msg) { *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
        int n = snprintf(msg, need + 1, "Board '%s' configured. No static info available.", board);
        size_t len = (n > 0 && (size_t)n <= need) ? (size_t)n : need;
        msg[len] = '\0';
        *out = sc_tool_result_ok_owned(msg, len);
        return SC_OK;
    }
    sc_json_buf_t buf;
    if (sc_json_buf_init(&buf, alloc) != SC_OK) {
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
    if (sc_json_buf_append_raw(&buf, "[", 1) != SC_OK) goto fail;
    for (size_t i = 0; i < BOARDS_COUNT; i++) {
        if (i > 0) sc_json_buf_append_raw(&buf, ",", 1);
        if (sc_json_buf_append_raw(&buf, "{\"name\":\"", 9) != SC_OK) goto fail;
        sc_json_append_string(&buf, boards[i].name, strlen(boards[i].name));
        if (sc_json_buf_append_raw(&buf, "\",\"chip\":\"", 9) != SC_OK) goto fail;
        sc_json_append_string(&buf, boards[i].chip, strlen(boards[i].chip));
        if (sc_json_buf_append_raw(&buf, "\",\"description\":\"", 17) != SC_OK) goto fail;
        sc_json_append_string(&buf, boards[i].desc, strlen(boards[i].desc));
        if (sc_json_buf_append_raw(&buf, "\"}", 2) != SC_OK) goto fail;
    }
    if (sc_json_buf_append_raw(&buf, "]", 1) != SC_OK) goto fail;
    char *msg = (char *)alloc->alloc(alloc->ctx, buf.len + 1);
    if (!msg) { fail: sc_json_buf_free(&buf); *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
    memcpy(msg, buf.ptr, buf.len);
    msg[buf.len] = '\0';
    sc_json_buf_free(&buf);
    *out = sc_tool_result_ok_owned(msg, buf.len);
    return SC_OK;
}

static const char *hardware_info_name(void *ctx) { (void)ctx; return SC_HARDWARE_INFO_NAME; }
static const char *hardware_info_description(void *ctx) { (void)ctx; return SC_HARDWARE_INFO_DESC; }
static const char *hardware_info_parameters_json(void *ctx) { (void)ctx; return SC_HARDWARE_INFO_PARAMS; }
static void hardware_info_deinit(void *ctx, sc_allocator_t *alloc) { (void)alloc; if (ctx) free(ctx); }

static const sc_tool_vtable_t hardware_info_vtable = {
    .execute = hardware_info_execute, .name = hardware_info_name,
    .description = hardware_info_description, .parameters_json = hardware_info_parameters_json,
    .deinit = hardware_info_deinit,
};

sc_error_t sc_hardware_info_create(sc_allocator_t *alloc, bool enabled, sc_tool_t *out) {
    (void)alloc;
    sc_hardware_info_ctx_t *c = (sc_hardware_info_ctx_t *)calloc(1, sizeof(*c));
    if (!c) return SC_ERR_OUT_OF_MEMORY;
    c->enabled = enabled;
    out->ctx = c;
    out->vtable = &hardware_info_vtable;
    return SC_OK;
}
