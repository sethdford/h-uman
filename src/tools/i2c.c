#include "seaclaw/tool.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#if defined(__linux__) && !SC_IS_TEST
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#endif
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define SC_I2C_NAME "i2c"
#define SC_I2C_DESC "I2C bus operations: detect buses, scan devices, read/write registers."
#define SC_I2C_PARAMS "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"detect\",\"scan\",\"read\",\"write\"]},\"bus\":{\"type\":\"integer\"},\"address\":{\"type\":\"string\"},\"register\":{\"type\":\"integer\"},\"value\":{\"type\":\"integer\"},\"length\":{\"type\":\"integer\"}},\"required\":[\"action\"]}"

typedef struct sc_i2c_ctx {
    const char *serial_port;
    size_t serial_port_len;
} sc_i2c_ctx_t;

static sc_error_t i2c_execute(void *ctx, sc_allocator_t *alloc,
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
    if (strcmp(action, "detect") == 0) {
        char *msg = sc_strndup(alloc, "{\"buses\":[0,1]}", 15);
        if (!msg) { *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
        *out = sc_tool_result_ok_owned(msg, 15);
        return SC_OK;
    }
    if (strcmp(action, "scan") == 0) {
        char *msg = sc_strndup(alloc, "{\"devices\":[\"0x48\",\"0x68\"]}", 24);
        if (!msg) { *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
        *out = sc_tool_result_ok_owned(msg, 24);
        return SC_OK;
    }
    if (strcmp(action, "read") == 0) {
        char *msg = sc_strndup(alloc, "{\"data\":\"0x00 0xFF\"}", 17);
        if (!msg) { *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
        *out = sc_tool_result_ok_owned(msg, 17);
        return SC_OK;
    }
    if (strcmp(action, "write") == 0) {
        char *msg = sc_strndup(alloc, "{\"status\":\"ok\"}", 15);
        if (!msg) { *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
        *out = sc_tool_result_ok_owned(msg, 15);
        return SC_OK;
    }
    *out = sc_tool_result_fail("Unknown action", 14);
    return SC_OK;
#else
#ifdef __linux__
    if (strcmp(action, "detect") == 0) {
        sc_json_buf_t buf;
        sc_json_buf_init(&buf, alloc);
        sc_json_buf_append_raw(&buf, "{\"buses\":[", 10);
        bool first = true;
        for (int i = 0; i <= 15; i++) {
            char path[32];
            snprintf(path, sizeof(path), "/dev/i2c-%d", i);
            if (access(path, F_OK) == 0) {
                if (!first) sc_json_buf_append_raw(&buf, ",", 1);
                char num[4];
                int nlen = snprintf(num, sizeof(num), "%d", i);
                sc_json_buf_append_raw(&buf, num, nlen);
                first = false;
            }
        }
        sc_json_buf_append_raw(&buf, "]}", 2);
        char *msg = sc_strndup(alloc, buf.ptr, buf.len);
        size_t len = buf.len;
        sc_json_buf_free(&buf);
        if (!msg) { *out = sc_tool_result_fail("out of memory", 12); return SC_ERR_OUT_OF_MEMORY; }
        *out = sc_tool_result_ok_owned(msg, len);
        return SC_OK;
    }
    if (strcmp(action, "scan") == 0) {
        int bus = (int)sc_json_get_number(args, "bus", 0);
        char path[32];
        snprintf(path, sizeof(path), "/dev/i2c-%d", bus);
        int fd = open(path, O_RDWR);
        if (fd < 0) { *out = sc_tool_result_fail("Cannot open I2C bus", 19); return SC_OK; }
        sc_json_buf_t buf;
        sc_json_buf_init(&buf, alloc);
        sc_json_buf_append_raw(&buf, "{\"devices\":[", 12);
        bool first = true;
        for (int addr = 0x03; addr <= 0x77; addr++) {
            if (ioctl(fd, I2C_SLAVE, addr) >= 0) {
                unsigned char dummy;
                if (read(fd, &dummy, 1) >= 0) {
                    if (!first) sc_json_buf_append_raw(&buf, ",", 1);
                    char hex[8];
                    int hlen = snprintf(hex, sizeof(hex), "\"0x%02x\"", addr);
                    sc_json_buf_append_raw(&buf, hex, hlen);
                    first = false;
                }
            }
        }
        close(fd);
        sc_json_buf_append_raw(&buf, "]}", 2);
        char *msg = sc_strndup(alloc, buf.ptr, buf.len);
        size_t len = buf.len;
        sc_json_buf_free(&buf);
        *out = sc_tool_result_ok_owned(msg, len);
        return SC_OK;
    }
    if (strcmp(action, "read") == 0) {
        int bus = (int)sc_json_get_number(args, "bus", 0);
        const char *addr_str = sc_json_get_string(args, "address");
        int reg = (int)sc_json_get_number(args, "register", 0);
        int length = (int)sc_json_get_number(args, "length", 1);
        if (!addr_str) { *out = sc_tool_result_fail("missing address", 15); return SC_OK; }
        unsigned int addr = 0;
        sscanf(addr_str, "0x%x", &addr);
        if (addr < 0x03 || addr > 0x77) { *out = sc_tool_result_fail("address out of range (0x03-0x77)", 31); return SC_OK; }
        if (length < 1 || length > 32) length = 1;
        char path[32];
        snprintf(path, sizeof(path), "/dev/i2c-%d", bus);
        int fd = open(path, O_RDWR);
        if (fd < 0) { *out = sc_tool_result_fail("Cannot open I2C bus", 19); return SC_OK; }
        if (ioctl(fd, I2C_SLAVE, addr) < 0) { close(fd); *out = sc_tool_result_fail("Cannot set I2C address", 22); return SC_OK; }
        unsigned char reg_byte = (unsigned char)reg;
        if (write(fd, &reg_byte, 1) != 1) { close(fd); *out = sc_tool_result_fail("I2C write register failed", 25); return SC_OK; }
        unsigned char buf_data[32];
        ssize_t n = read(fd, buf_data, (size_t)length);
        close(fd);
        if (n < 0) { *out = sc_tool_result_fail("I2C read failed", 15); return SC_OK; }
        char result[256] = "{\"data\":\"";
        size_t pos = 9;
        for (ssize_t i = 0; i < n; i++) {
            if (i > 0) result[pos++] = ' ';
            pos += (size_t)snprintf(result + pos, sizeof(result) - pos, "0x%02X", buf_data[i]);
        }
        pos += (size_t)snprintf(result + pos, sizeof(result) - pos, "\"}");
        char *msg = sc_strndup(alloc, result, pos);
        *out = sc_tool_result_ok_owned(msg, pos);
        return SC_OK;
    }
    if (strcmp(action, "write") == 0) {
        int bus = (int)sc_json_get_number(args, "bus", 0);
        const char *addr_str = sc_json_get_string(args, "address");
        int reg = (int)sc_json_get_number(args, "register", 0);
        int value = (int)sc_json_get_number(args, "value", 0);
        if (!addr_str) { *out = sc_tool_result_fail("missing address", 15); return SC_OK; }
        unsigned int addr = 0;
        sscanf(addr_str, "0x%x", &addr);
        if (addr < 0x03 || addr > 0x77) { *out = sc_tool_result_fail("address out of range (0x03-0x77)", 31); return SC_OK; }
        char path[32];
        snprintf(path, sizeof(path), "/dev/i2c-%d", bus);
        int fd = open(path, O_RDWR);
        if (fd < 0) { *out = sc_tool_result_fail("Cannot open I2C bus", 19); return SC_OK; }
        if (ioctl(fd, I2C_SLAVE, addr) < 0) { close(fd); *out = sc_tool_result_fail("Cannot set I2C address", 22); return SC_OK; }
        unsigned char data[2] = {(unsigned char)reg, (unsigned char)value};
        if (write(fd, data, 2) != 2) { close(fd); *out = sc_tool_result_fail("I2C write failed", 16); return SC_OK; }
        close(fd);
        char *msg = sc_strndup(alloc, "{\"status\":\"ok\"}", 15);
        *out = sc_tool_result_ok_owned(msg, 15);
        return SC_OK;
    }
    *out = sc_tool_result_fail("Unknown action", 14);
    return SC_OK;
#else
    (void)alloc;
    (void)ctx;
    *out = sc_tool_result_fail("I2C requires Linux", 18);
    return SC_OK;
#endif
#endif
}

static const char *i2c_name(void *ctx) { (void)ctx; return SC_I2C_NAME; }
static const char *i2c_description(void *ctx) { (void)ctx; return SC_I2C_DESC; }
static const char *i2c_parameters_json(void *ctx) { (void)ctx; return SC_I2C_PARAMS; }
static void i2c_deinit(void *ctx, sc_allocator_t *alloc) { (void)alloc; if (ctx) free(ctx); }

static const sc_tool_vtable_t i2c_vtable = {
    .execute = i2c_execute, .name = i2c_name,
    .description = i2c_description, .parameters_json = i2c_parameters_json,
    .deinit = i2c_deinit,
};

sc_error_t sc_i2c_create(sc_allocator_t *alloc,
    const char *serial_port, size_t serial_port_len,
    sc_tool_t *out)
{
    sc_i2c_ctx_t *c = (sc_i2c_ctx_t *)calloc(1, sizeof(*c));
    if (!c) return SC_ERR_OUT_OF_MEMORY;
    if (serial_port && serial_port_len > 0) {
        c->serial_port = sc_strndup(alloc, serial_port, serial_port_len);
        if (!c->serial_port) { free(c); return SC_ERR_OUT_OF_MEMORY; }
        c->serial_port_len = serial_port_len;
    }
    out->ctx = c;
    out->vtable = &i2c_vtable;
    return SC_OK;
}
