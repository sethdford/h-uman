#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/tool.h"
#if defined(__linux__) && !HU_IS_TEST
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HU_SPI_NAME "spi"
#define HU_SPI_DESC "SPI bus operations: list devices, transfer data, read bytes."
#define HU_SPI_PARAMS                                                                          \
    "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"list\"," \
    "\"transfer\",\"read\"]},\"device\":{\"type\":\"string\"},\"data\":{\"type\":\"string\"}," \
    "\"speed_hz\":{\"type\":\"integer\"},\"mode\":{\"type\":\"integer\"},\"bits_per_word\":{"  \
    "\"type\":\"integer\"}},\"required\":[\"action\"]}"

typedef struct hu_spi_ctx {
    const char *device;
    size_t device_len;
} hu_spi_ctx_t;

static hu_error_t spi_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                              hu_tool_result_t *out) {
    (void)ctx;
    if (!args || !out) {
        *out = hu_tool_result_fail("invalid args", 12);
        return HU_ERR_INVALID_ARGUMENT;
    }
    const char *action = hu_json_get_string(args, "action");
    if (!action || strlen(action) == 0) {
        *out = hu_tool_result_fail("missing action", 14);
        return HU_OK;
    }
#if HU_IS_TEST
    if (strcmp(action, "list") == 0) {
        char *msg = hu_strndup(alloc, "{\"devices\":[\"/dev/spidev0.0\"]}", 29);
        if (!msg) {
            *out = hu_tool_result_fail("out of memory", 12);
            return HU_ERR_OUT_OF_MEMORY;
        }
        *out = hu_tool_result_ok_owned(msg, 29);
        return HU_OK;
    }
    if (strcmp(action, "transfer") == 0 || strcmp(action, "read") == 0) {
        char *msg = hu_strndup(alloc, "{\"rx_data\":\"00 FF\"}", 19);
        if (!msg) {
            *out = hu_tool_result_fail("out of memory", 12);
            return HU_ERR_OUT_OF_MEMORY;
        }
        *out = hu_tool_result_ok_owned(msg, 19);
        return HU_OK;
    }
    *out = hu_tool_result_fail("Unknown action", 14);
    return HU_OK;
#else
#ifdef __linux__
    if (strcmp(action, "list") == 0) {
        hu_json_buf_t buf;
        hu_json_buf_init(&buf, alloc);
        hu_json_buf_append_raw(&buf, "{\"devices\":[", 12);
        bool first = true;
        for (int b = 0; b <= 2; b++) {
            for (int d = 0; d <= 1; d++) {
                char path[32];
                snprintf(path, sizeof(path), "/dev/spidev%d.%d", b, d);
                if (access(path, F_OK) == 0) {
                    if (!first)
                        hu_json_buf_append_raw(&buf, ",", 1);
                    hu_json_buf_append_raw(&buf, "\"", 1);
                    hu_json_buf_append_raw(&buf, path, strlen(path));
                    hu_json_buf_append_raw(&buf, "\"", 1);
                    first = false;
                }
            }
        }
        hu_json_buf_append_raw(&buf, "]}", 2);
        char *msg = hu_strndup(alloc, buf.ptr, buf.len);
        size_t len = buf.len;
        hu_json_buf_free(&buf);
        *out = hu_tool_result_ok_owned(msg, len);
        return HU_OK;
    }
    const char *device = hu_json_get_string(args, "device");
    if (!device || device[0] == '\0')
        device = "/dev/spidev0.0";
    uint32_t speed = (uint32_t)hu_json_get_number(args, "speed_hz", 1000000);
    uint8_t mode = (uint8_t)hu_json_get_number(args, "mode", 0);
    uint8_t bits = (uint8_t)hu_json_get_number(args, "bits_per_word", 8);

    int fd = open(device, O_RDWR);
    if (fd < 0) {
        *out = hu_tool_result_fail("Cannot open SPI device", 22);
        return HU_OK;
    }
    ioctl(fd, SPI_IOC_WR_MODE, &mode);
    ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
    ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed);

    if (strcmp(action, "transfer") == 0) {
        const char *hex_data = hu_json_get_string(args, "data");
        if (!hex_data || hex_data[0] == '\0') {
            close(fd);
            *out = hu_tool_result_fail("missing data for transfer", 25);
            return HU_OK;
        }
        uint8_t tx[128], rx[128];
        size_t tx_len = 0;
        for (const char *p = hex_data; *p && tx_len < sizeof(tx);) {
            while (*p == ' ')
                p++;
            if (!*p)
                break;
            char byte_str[3] = {p[0], p[1] ? p[1] : '\0', '\0'};
            tx[tx_len++] = (uint8_t)strtoul(byte_str, NULL, 16);
            p += (p[1] ? 2 : 1);
        }
        memset(rx, 0, sizeof(rx));

        struct spi_ioc_transfer tr;
        memset(&tr, 0, sizeof(tr));
        tr.tx_buf = (unsigned long)tx;
        tr.rx_buf = (unsigned long)rx;
        tr.len = (uint32_t)tx_len;
        tr.speed_hz = speed;
        tr.bits_per_word = bits;

        int ret = ioctl(fd, SPI_IOC_MESSAGE(1), &tr);
        close(fd);
        if (ret < 0) {
            *out = hu_tool_result_fail("SPI transfer failed", 19);
            return HU_OK;
        }
        char hex_buf[512];
        size_t hp = 0;
        hp += (size_t)snprintf(hex_buf + hp, sizeof(hex_buf) - hp, "{\"rx_data\":\"");
        for (size_t i = 0; i < tx_len && hp + 4 < sizeof(hex_buf); i++) {
            if (i > 0)
                hex_buf[hp++] = ' ';
            hp += (size_t)snprintf(hex_buf + hp, sizeof(hex_buf) - hp, "%02X", rx[i]);
        }
        hp += (size_t)snprintf(hex_buf + hp, sizeof(hex_buf) - hp, "\"}");
        char *msg = hu_strndup(alloc, hex_buf, hp);
        if (!msg) {
            *out = hu_tool_result_fail("out of memory", 12);
            return HU_ERR_OUT_OF_MEMORY;
        }
        *out = hu_tool_result_ok_owned(msg, hp);
        return HU_OK;
    }
    if (strcmp(action, "read") == 0) {
        double len_val = hu_json_get_number(args, "length", 16);
        size_t read_len = (size_t)len_val;
        if (read_len > 128)
            read_len = 128;
        uint8_t tx[128], rx[128];
        memset(tx, 0xFF, sizeof(tx));
        memset(rx, 0, sizeof(rx));

        struct spi_ioc_transfer tr;
        memset(&tr, 0, sizeof(tr));
        tr.tx_buf = (unsigned long)tx;
        tr.rx_buf = (unsigned long)rx;
        tr.len = (uint32_t)read_len;
        tr.speed_hz = speed;
        tr.bits_per_word = bits;

        int ret = ioctl(fd, SPI_IOC_MESSAGE(1), &tr);
        close(fd);
        if (ret < 0) {
            *out = hu_tool_result_fail("SPI read failed", 15);
            return HU_OK;
        }
        char hex_buf[512];
        size_t hp = 0;
        hp += (size_t)snprintf(hex_buf + hp, sizeof(hex_buf) - hp, "{\"rx_data\":\"");
        for (size_t i = 0; i < read_len && hp + 4 < sizeof(hex_buf); i++) {
            if (i > 0)
                hex_buf[hp++] = ' ';
            hp += (size_t)snprintf(hex_buf + hp, sizeof(hex_buf) - hp, "%02X", rx[i]);
        }
        hp += (size_t)snprintf(hex_buf + hp, sizeof(hex_buf) - hp, "\"}");
        char *msg = hu_strndup(alloc, hex_buf, hp);
        if (!msg) {
            *out = hu_tool_result_fail("out of memory", 12);
            return HU_ERR_OUT_OF_MEMORY;
        }
        *out = hu_tool_result_ok_owned(msg, hp);
        return HU_OK;
    }
    close(fd);
    *out = hu_tool_result_fail("unknown SPI action", 18);
    return HU_OK;
#else
    (void)alloc;
    *out = hu_tool_result_fail("SPI requires Linux", 18);
    return HU_OK;
#endif
#endif
}

static const char *spi_name(void *ctx) {
    (void)ctx;
    return HU_SPI_NAME;
}
static const char *spi_description(void *ctx) {
    (void)ctx;
    return HU_SPI_DESC;
}
static const char *spi_parameters_json(void *ctx) {
    (void)ctx;
    return HU_SPI_PARAMS;
}
static void spi_deinit(void *ctx, hu_allocator_t *alloc) {
    if (ctx && alloc)
        alloc->free(alloc->ctx, ctx, sizeof(hu_spi_ctx_t));
}

static const hu_tool_vtable_t spi_vtable = {
    .execute = spi_execute,
    .name = spi_name,
    .description = spi_description,
    .parameters_json = spi_parameters_json,
    .deinit = spi_deinit,
};

hu_error_t hu_spi_create(hu_allocator_t *alloc, const char *device, size_t device_len,
                         hu_tool_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_spi_ctx_t *c = (hu_spi_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    if (device && device_len > 0) {
        c->device = hu_strndup(alloc, device, device_len);
        if (!c->device) {
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        c->device_len = device_len;
    }
    out->ctx = c;
    out->vtable = &spi_vtable;
    return HU_OK;
}
