/* Keep message text and handles out of production logs. See log_redact.h. */
#include "human/core/log_redact.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int g_content_override = -1;

bool hu_log_content_enabled(void) {
    if (g_content_override >= 0)
        return g_content_override != 0;
    const char *v = getenv("HU_LOG_CONTENT");
    return v && v[0] == '1' && v[1] == '\0';
}

void hu_log_content_set_for_test(int on_or_minus1) {
    g_content_override = on_or_minus1;
}

unsigned hu_log_contact_tag(const char *handle, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; handle && i < len; i++)
        h = (h ^ (unsigned char)handle[i]) * 16777619u;
    return (unsigned)((h ^ (h >> 16)) & 0xffffu);
}

const char *hu_log_who(const char *handle, size_t len, char *buf, size_t cap) {
    if (!buf || cap == 0)
        return "";
    if (!handle || len == 0)
        snprintf(buf, cap, "-");
    else if (hu_log_content_enabled())
        snprintf(buf, cap, "%.*s", (int)(len > 24 ? 24 : len), handle);
    else
        snprintf(buf, cap, "#%04x", hu_log_contact_tag(handle, len));
    return buf;
}

const char *hu_log_text(const char *text, size_t len, size_t max, char *buf, size_t cap) {
    if (!buf || cap == 0)
        return "";
    if (!text)
        len = 0;
    if (hu_log_content_enabled())
        snprintf(buf, cap, "%.*s", (int)(len > max ? max : len), text ? text : "");
    else
        snprintf(buf, cap, "<%zu chars>", len);
    return buf;
}
