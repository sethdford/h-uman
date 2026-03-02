#include "seaclaw/tool.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#if defined(__linux__) && !SC_IS_TEST
#include <fcntl.h>
#include <unistd.h>
#endif
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define SC_SPI_NAME "spi"
#define SC_SPI_DESC "SPI bus operations: list devices, transfer data, read bytes."
#define SC_SPI_PARAMS "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"list\",\"transfer\",\"read\"]},\"device\":{\"type\":\"string\"},\"data\":{\"type\":\"string\"},\"speed_hz\":{\"type\":\"integer\"},\"mode\":{\"type\":\"integer\"},\"bits_per_word\":{\"type\":\"integer\"}},\"required\":[\"action\"]}"

typedef struct sc_spi_ctx {
    const char *device;
    size_t device_len;
} sc_spi_ctx_t;

static sc_error_t spi_execute(void *ctx, sc_allocator_t *alloc,
    const sc_json_value_t *args,
    sc_tool_result_t *out)
{
    (void)ctx;
    if (!args || !out) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_ERR_INVALID_ARGUMENT;
    }
    const char *action = sc_json_get_string(args, "action");
    if (!action || strlen(action) == 0) {
        *out = sc_tool_result_fail("missing action", 14);
        return SC_OK;
    }
#if SC_IS_TEST
    if (strcmp(action, "list") == 0) {
        char *msg = sc_strndup(alloc, "{\"devices\":[\"/dev/spidev0.0\"]}", 29);
        if (!msg) { *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
        *out = sc_tool_result_ok_owned(msg, 29);
        return SC_OK;
    }
    if (strcmp(action, "transfer") == 0 || strcmp(action, "read") == 0) {
        char *msg = sc_strndup(alloc, "{\"rx_data\":\"00 FF\"}", 19);
        if (!msg) { *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
        *out = sc_tool_result_ok_owned(msg, 19);
        return SC_OK;
    }
    *out = sc_tool_result_fail("Unknown action", 14);
    return SC_OK;
#else
#ifdef __linux__
    if (strcmp(action, "list") == 0) {
        sc_json_buf_t buf;
        sc_json_buf_init(&buf, alloc);
        sc_json_buf_append_raw(&buf, "{\"devices\":[", 12);
        bool first = true;
        for (int b = 0; b <= 2; b++) {
            for (int d = 0; d <= 1; d++) {
                char path[32];
                snprintf(path, sizeof(path), "/dev/spidev%d.%d", b, d);
                if (access(path, F_OK) == 0) {
                    if (!first) sc_json_buf_append_raw(&buf, ",", 1);
                    sc_json_buf_append_raw(&buf, "\"", 1);
                    sc_json_buf_append_raw(&buf, path, strlen(path));
                    sc_json_buf_append_raw(&buf, "\"", 1);
                    first = false;
                }
            }
        }
        sc_json_buf_append_raw(&buf, "]}", 2);
        char *msg = sc_strndup(alloc, buf.ptr, buf.len);
        size_t len = buf.len;
        sc_json_buf_free(&buf);
        *out = sc_tool_result_ok_owned(msg, len);
        return SC_OK;
    }
    *out = sc_tool_result_fail("SPI ioctl transfer not yet available", 35);
    return SC_OK;
#else
    (void)alloc;
    *out = sc_tool_result_fail("SPI requires Linux", 18);
    return SC_OK;
#endif
#endif
}

static const char *spi_name(void *ctx) { (void)ctx; return SC_SPI_NAME; }
static const char *spi_description(void *ctx) { (void)ctx; return SC_SPI_DESC; }
static const char *spi_parameters_json(void *ctx) { (void)ctx; return SC_SPI_PARAMS; }
static void spi_deinit(void *ctx, sc_allocator_t *alloc) { (void)alloc; if (ctx) free(ctx); }

static const sc_tool_vtable_t spi_vtable = {
    .execute = spi_execute, .name = spi_name,
    .description = spi_description, .parameters_json = spi_parameters_json,
    .deinit = spi_deinit,
};

sc_error_t sc_spi_create(sc_allocator_t *alloc,
    const char *device, size_t device_len,
    sc_tool_t *out)
{
    sc_spi_ctx_t *c = (sc_spi_ctx_t *)calloc(1, sizeof(*c));
    if (!c) return SC_ERR_OUT_OF_MEMORY;
    if (device && device_len > 0) {
        c->device = sc_strndup(alloc, device, device_len);
        if (!c->device) { free(c); return SC_ERR_OUT_OF_MEMORY; }
        c->device_len = device_len;
    }
    out->ctx = c;
    out->vtable = &spi_vtable;
    return SC_OK;
}
