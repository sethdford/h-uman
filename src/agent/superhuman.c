/*
 * Superhuman services registry — build_context and observe_all.
 */
#include "human/agent/superhuman.h"
#include "human/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

hu_error_t hu_superhuman_registry_init(hu_superhuman_registry_t *reg) {
    if (!reg)
        return HU_ERR_INVALID_ARGUMENT;
    memset(reg, 0, sizeof(*reg));
    return HU_OK;
}

hu_error_t hu_superhuman_register(hu_superhuman_registry_t *reg, hu_superhuman_service_t service) {
    if (!reg)
        return HU_ERR_INVALID_ARGUMENT;
    if (reg->count >= HU_SUPERHUMAN_MAX_SERVICES)
        return HU_ERR_OUT_OF_MEMORY;
    reg->services[reg->count] = service;
    reg->count++;
    return HU_OK;
}

hu_error_t hu_superhuman_build_context(hu_superhuman_registry_t *reg, hu_allocator_t *alloc,
                                        char **out, size_t *out_len) {
    if (!reg || !alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    size_t cap = 128;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    size_t len = 0;
    buf[0] = '\0';

    static const char HEADER[] = "### Superhuman Insights\n\n";
    size_t hlen = sizeof(HEADER) - 1;
    while (len + hlen + 1 > cap) {
        size_t new_cap = cap * 2;
        char *nb = (char *)alloc->realloc(alloc->ctx, buf, cap, new_cap);
        if (!nb) {
            alloc->free(alloc->ctx, buf, cap);
            return HU_ERR_OUT_OF_MEMORY;
        }
        buf = nb;
        cap = new_cap;
    }
    memcpy(buf, HEADER, hlen);
    len = hlen;

    for (size_t i = 0; i < reg->count; i++) {
        const hu_superhuman_service_t *svc = &reg->services[i];
        if (!svc->build_context || !svc->name)
            continue;

        char *ctx = NULL;
        size_t ctx_len = 0;
        hu_error_t err = svc->build_context(svc->ctx, alloc, &ctx, &ctx_len);
        if (err != HU_OK || !ctx || ctx_len == 0) {
            if (ctx)
                alloc->free(alloc->ctx, ctx, ctx_len + 1);
            continue;
        }

        size_t name_len = strlen(svc->name);
        size_t need = len + 6 + name_len + 1 + ctx_len + 3; /* "#### " + name + "\n" + ctx + "\n\n" */
        while (need > cap) {
            size_t new_cap = cap * 2;
            char *nb = (char *)alloc->realloc(alloc->ctx, buf, cap, new_cap);
            if (!nb) {
                alloc->free(alloc->ctx, ctx, ctx_len + 1);
                alloc->free(alloc->ctx, buf, cap);
                return HU_ERR_OUT_OF_MEMORY;
            }
            buf = nb;
            cap = new_cap;
        }
        memcpy(buf + len, "#### ", 5);
        len += 5;
        memcpy(buf + len, svc->name, name_len);
        len += name_len;
        buf[len++] = '\n';
        memcpy(buf + len, ctx, ctx_len);
        len += ctx_len;
        buf[len++] = '\n';
        buf[len++] = '\n';
        alloc->free(alloc->ctx, ctx, ctx_len + 1);
    }

    buf[len] = '\0';
    if (len == hlen) {
        alloc->free(alloc->ctx, buf, cap);
        *out = NULL;
        *out_len = 0;
        return HU_OK;
    }
    *out = buf;
    *out_len = len;
    return HU_OK;
}

hu_error_t hu_superhuman_observe_all(hu_superhuman_registry_t *reg, hu_allocator_t *alloc,
                                      const char *text, size_t text_len, const char *role,
                                      size_t role_len) {
    if (!reg)
        return HU_ERR_INVALID_ARGUMENT;
    for (size_t i = 0; i < reg->count; i++) {
        const hu_superhuman_service_t *svc = &reg->services[i];
        if (svc->observe)
            (void)svc->observe(svc->ctx, alloc, text, text_len, role ? role : "", role_len);
    }
    return HU_OK;
}
