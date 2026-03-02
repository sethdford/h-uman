#include "seaclaw/doctor.h"
#include "seaclaw/channel_catalog.h"
#include "seaclaw/core/string.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

unsigned long sc_doctor_parse_df_available_mb(const char *df_output, size_t len) {
    if (!df_output || len == 0) return 0;
    const char *last_line = NULL;
    const char *p = df_output;
    const char *end = df_output + len;
    while (p < end) {
        const char *line = p;
        while (p < end && *p != '\n') p++;
        if (p > line) {
            while (p > line && (p[-1] == ' ' || p[-1] == '\r')) p--;
            if (p > line) last_line = line;
        }
        if (p < end) p++;
    }
    if (!last_line) return 0;
    const char *col = last_line;
    for (int i = 0; i < 4 && col < end; i++) {
        while (col < end && (*col == ' ' || *col == '\t')) col++;
        if (col >= end) return 0;
        const char *start = col;
        while (col < end && *col != ' ' && *col != '\t') col++;
        if (i == 3) {
            unsigned long v = 0;
            for (const char *q = start; q < col; q++) {
                if (*q >= '0' && *q <= '9') v = v * 10 + (unsigned long)(*q - '0');
            }
            return v;
        }
    }
    return 0;
}

sc_error_t sc_doctor_truncate_for_display(sc_allocator_t *alloc,
    const char *s, size_t len, size_t max_len, char **out) {
    if (!alloc || !out) return SC_ERR_INVALID_ARGUMENT;
    if (!s) { *out = NULL; return SC_OK; }
    if (len == 0) len = strlen(s);
    if (len <= max_len) {
        *out = sc_strndup(alloc, s, len);
        return *out ? SC_OK : SC_ERR_OUT_OF_MEMORY;
    }
    size_t i = max_len;
    while (i > 0 && (s[i] & 0xC0) == 0x80) i--;
    *out = sc_strndup(alloc, s, i);
    return *out ? SC_OK : SC_ERR_OUT_OF_MEMORY;
}

sc_error_t sc_doctor_check_config_semantics(sc_allocator_t *alloc,
    const sc_config_t *cfg,
    sc_diag_item_t **items, size_t *count) {
    if (!alloc || !cfg || !items || !count) return SC_ERR_INVALID_ARGUMENT;
    *items = NULL;
    *count = 0;

    size_t cap = 16;
    sc_diag_item_t *buf = (sc_diag_item_t *)alloc->alloc(alloc->ctx, sizeof(sc_diag_item_t) * cap);
    if (!buf) return SC_ERR_OUT_OF_MEMORY;

    size_t n = 0;
    sc_diag_item_t it;

    if (!cfg->default_provider || !cfg->default_provider[0]) {
        it = (sc_diag_item_t){ SC_DIAG_ERR, sc_strdup(alloc, "config"), sc_strdup(alloc, "no default_provider configured") };
        buf[n++] = it;
    } else {
        char *msg = sc_sprintf(alloc, "provider: %s", cfg->default_provider);
        if (msg) { it = (sc_diag_item_t){ SC_DIAG_OK, sc_strdup(alloc, "config"), msg }; buf[n++] = it; }
    }

    if (cfg->default_temperature < 0.0 || cfg->default_temperature > 2.0) {
        char *msg = sc_sprintf(alloc, "temperature %.1f is out of range (expected 0.0-2.0)", cfg->default_temperature);
        if (msg) { it = (sc_diag_item_t){ SC_DIAG_ERR, sc_strdup(alloc, "config"), msg }; buf[n++] = it; }
    } else {
        char *msg = sc_sprintf(alloc, "temperature %.1f (valid range 0.0-2.0)", cfg->default_temperature);
        if (msg) { it = (sc_diag_item_t){ SC_DIAG_OK, sc_strdup(alloc, "config"), msg }; buf[n++] = it; }
    }

    uint16_t gw_port = cfg->gateway.port;
    if (gw_port == 0) {
        it = (sc_diag_item_t){ SC_DIAG_ERR, sc_strdup(alloc, "config"), sc_strdup(alloc, "gateway port is 0 (invalid)") };
        buf[n++] = it;
    } else {
        char *msg = sc_sprintf(alloc, "gateway port: %u", (unsigned)gw_port);
        if (msg) { it = (sc_diag_item_t){ SC_DIAG_OK, sc_strdup(alloc, "config"), msg }; buf[n++] = it; }
    }

    bool has_ch = sc_channel_catalog_has_any_configured(cfg, false);
    if (has_ch) {
        it = (sc_diag_item_t){ SC_DIAG_OK, sc_strdup(alloc, "config"), sc_strdup(alloc, "at least one channel configured") };
        buf[n++] = it;
    } else {
        it = (sc_diag_item_t){ SC_DIAG_WARN, sc_strdup(alloc, "config"), sc_strdup(alloc, "no channels configured -- run onboard to set one up") };
        buf[n++] = it;
    }

    *items = buf;
    *count = n;
    return SC_OK;
}
