#include "human/agent/preferences.h"
#include "human/core/string.h"
#include <string.h>

static bool starts_with_ci(const char *s, size_t slen, const char *prefix, size_t plen) {
    if (slen < plen)
        return false;
    for (size_t i = 0; i < plen; i++) {
        char a = s[i];
        char b = prefix[i];
        if (a >= 'A' && a <= 'Z')
            a += 32;
        if (b >= 'A' && b <= 'Z')
            b += 32;
        if (a != b)
            return false;
    }
    return true;
}

bool hu_preferences_is_correction(const char *message, size_t message_len) {
    if (!message || message_len < 3)
        return false;

    static const struct {
        const char *prefix;
        size_t len;
    } markers[] = {
        {"no,", 3},      {"no ", 3},           {"actually,", 9}, {"actually ", 9},
        {"i prefer", 8}, {"i'd prefer", 10},   {"don't ", 6},    {"do not ", 7},
        {"stop ", 5},    {"please don't", 12}, {"never ", 6},    {"always ", 7},
        {"instead,", 8}, {"instead ", 8},
    };
    size_t n = sizeof(markers) / sizeof(markers[0]);
    for (size_t i = 0; i < n; i++) {
        if (starts_with_ci(message, message_len, markers[i].prefix, markers[i].len))
            return true;
    }
    return false;
}

char *hu_preferences_extract(hu_allocator_t *alloc, const char *user_msg, size_t user_msg_len,
                             size_t *out_len) {
    if (!alloc || !user_msg || user_msg_len == 0)
        return NULL;

    /* Truncate overly long messages */
    size_t cap = user_msg_len > 200 ? 200 : user_msg_len;

    /* Strip leading whitespace */
    size_t start = 0;
    while (start < cap && (user_msg[start] == ' ' || user_msg[start] == '\t'))
        start++;

    size_t plen = cap - start;
    if (plen == 0)
        return NULL;

    char *result = hu_strndup(alloc, user_msg + start, plen);
    if (result && out_len)
        *out_len = plen;
    return result;
}

hu_error_t hu_preferences_store(hu_memory_t *memory, hu_allocator_t *alloc, const char *preference,
                                size_t preference_len) {
    (void)alloc;
    if (!memory || !memory->vtable || !memory->vtable->store || !preference)
        return HU_ERR_INVALID_ARGUMENT;

    char key[64];
    size_t klen = HU_PREF_KEY_PREFIX_LEN;
    memcpy(key, HU_PREF_KEY_PREFIX, klen);
    size_t copy = preference_len > 40 ? 40 : preference_len;
    memcpy(key + klen, preference, copy);
    klen += copy;
    key[klen] = '\0';

    return memory->vtable->store(memory->ctx, key, klen, preference, preference_len, NULL, "", 0);
}

hu_error_t hu_preferences_load(hu_memory_t *memory, hu_allocator_t *alloc, char **out,
                               size_t *out_len) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    if (out_len)
        *out_len = 0;

    if (!memory || !memory->vtable || !memory->vtable->recall)
        return HU_OK;

    hu_memory_entry_t *entries = NULL;
    size_t count = 0;
    hu_error_t err =
        memory->vtable->recall(memory->ctx, alloc, HU_PREF_KEY_PREFIX, HU_PREF_KEY_PREFIX_LEN,
                               HU_PREF_MAX_LOAD, "", 0, &entries, &count);
    if (err != HU_OK || !entries || count == 0)
        return HU_OK;

    /* Format preferences as bullet list */
    size_t total = 0;
    for (size_t i = 0; i < count; i++)
        total += 2 + entries[i].content_len + 1; /* "- " + content + "\n" */

    char *buf = (char *)alloc->alloc(alloc->ctx, total + 1);
    if (!buf) {
        for (size_t i = 0; i < count; i++) {
            if (entries[i].key)
                alloc->free(alloc->ctx, (void *)entries[i].key, entries[i].key_len + 1);
            if (entries[i].content)
                alloc->free(alloc->ctx, (void *)entries[i].content, entries[i].content_len + 1);
            if (entries[i].id)
                alloc->free(alloc->ctx, (void *)entries[i].id, entries[i].id_len + 1);
            if (entries[i].timestamp)
                alloc->free(alloc->ctx, (void *)entries[i].timestamp, entries[i].timestamp_len + 1);
            if (entries[i].session_id)
                alloc->free(alloc->ctx, (void *)entries[i].session_id,
                            entries[i].session_id_len + 1);
        }
        alloc->free(alloc->ctx, entries, count * sizeof(hu_memory_entry_t));
        return HU_ERR_OUT_OF_MEMORY;
    }

    size_t pos = 0;
    for (size_t i = 0; i < count; i++) {
        buf[pos++] = '-';
        buf[pos++] = ' ';
        if (entries[i].content && entries[i].content_len > 0) {
            memcpy(buf + pos, entries[i].content, entries[i].content_len);
            pos += entries[i].content_len;
        }
        buf[pos++] = '\n';
    }
    buf[pos] = '\0';

    /* Free entries */
    for (size_t i = 0; i < count; i++) {
        if (entries[i].key)
            alloc->free(alloc->ctx, (void *)entries[i].key, entries[i].key_len + 1);
        if (entries[i].content)
            alloc->free(alloc->ctx, (void *)entries[i].content, entries[i].content_len + 1);
        if (entries[i].id)
            alloc->free(alloc->ctx, (void *)entries[i].id, entries[i].id_len + 1);
        if (entries[i].timestamp)
            alloc->free(alloc->ctx, (void *)entries[i].timestamp, entries[i].timestamp_len + 1);
        if (entries[i].session_id)
            alloc->free(alloc->ctx, (void *)entries[i].session_id, entries[i].session_id_len + 1);
    }
    alloc->free(alloc->ctx, entries, count * sizeof(hu_memory_entry_t));

    *out = buf;
    if (out_len)
        *out_len = pos;
    return HU_OK;
}
