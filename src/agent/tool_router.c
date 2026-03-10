/*
 * Tool relevance scorer — keyword-based selection for semantic routing.
 */
#include "human/agent/tool_router.h"
#include "human/core/string.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define HU_TOOL_ROUTER_MAX_WORDS 128
#define HU_TOOL_ROUTER_MAX_SCORED 256
#define HU_TOOL_ROUTER_ALWAYS_COUNT 8

static const char *ALWAYS_TOOLS[] = {"memory_store", "memory_recall", "message", "shell",
                                     "file_read",   "file_write",    "file_edit", "web_search",
                                     NULL};

static bool is_always_tool(const char *name) {
    for (size_t i = 0; ALWAYS_TOOLS[i]; i++) {
        if (strcmp(name, ALWAYS_TOOLS[i]) == 0)
            return true;
    }
    return false;
}

typedef struct {
    const char *start;
    size_t len;
} word_ref_t;

typedef struct {
    size_t idx;
    double score;
} scored_entry_t;

static size_t tokenize_refs(const char *text, size_t text_len,
                            word_ref_t refs[HU_TOOL_ROUTER_MAX_WORDS]) {
    size_t count = 0;
    size_t i = 0;
    while (i < text_len && count < HU_TOOL_ROUTER_MAX_WORDS) {
        while (i < text_len && !isalnum((unsigned char)text[i]))
            i++;
        if (i >= text_len)
            break;
        size_t start = i;
        while (i < text_len && isalnum((unsigned char)text[i]))
            i++;
        refs[count].start = text + start;
        refs[count].len = i - start;
        count++;
    }
    return count;
}

static bool word_eq_ci(const char *a, size_t a_len, const char *b, size_t b_len) {
    if (a_len != b_len)
        return false;
    for (size_t i = 0; i < a_len; i++) {
        char ca = (char)((unsigned char)a[i] | 0x20);
        char cb = (char)((unsigned char)b[i] | 0x20);
        if (ca != cb)
            return false;
    }
    return true;
}

static double score_tool(const word_ref_t *msg_refs, size_t msg_count,
                         const char *name, const char *desc) {
    if (!name)
        return 0.0;

    char tool_text[512];
    size_t pos = 0;
    size_t name_len = strlen(name);
    if (name_len < sizeof(tool_text) - 1) {
        memcpy(tool_text + pos, name, name_len);
        pos += name_len;
    }
    if (desc && pos < sizeof(tool_text) - 2) {
        tool_text[pos++] = ' ';
        size_t desc_len = strlen(desc);
        size_t rem = sizeof(tool_text) - pos - 1;
        if (desc_len > rem)
            desc_len = rem;
        memcpy(tool_text + pos, desc, desc_len);
        pos += desc_len;
    }
    tool_text[pos] = '\0';

    char tool_words[HU_TOOL_ROUTER_MAX_WORDS][64];
    size_t tool_word_count = 0;
    size_t j = 0;
    while (j < pos && tool_word_count < HU_TOOL_ROUTER_MAX_WORDS) {
        while (j < pos && !isalnum((unsigned char)tool_text[j]))
            j++;
        if (j >= pos)
            break;
        size_t start = j;
        while (j < pos && isalnum((unsigned char)tool_text[j]) && (j - start) < 63)
            j++;
        size_t len = j - start;
        if (len > 0) {
            memcpy(tool_words[tool_word_count], tool_text + start, len);
            tool_words[tool_word_count][len] = '\0';
            for (size_t k = 0; k < len; k++)
                tool_words[tool_word_count][k] =
                    (char)((unsigned char)tool_words[tool_word_count][k] | 0x20);
            tool_word_count++;
        }
    }

    if (tool_word_count == 0)
        return 0.0;

    size_t matches = 0;
    for (size_t m = 0; m < msg_count; m++) {
        const char *w = msg_refs[m].start;
        size_t wlen = msg_refs[m].len;
        if (!w || wlen == 0)
            continue;
        for (size_t t = 0; t < tool_word_count; t++) {
            size_t tlen = strlen(tool_words[t]);
            if (word_eq_ci(w, wlen, tool_words[t], tlen)) {
                matches++;
                break;
            }
        }
    }

    return (double)matches / (double)tool_word_count;
}

hu_error_t hu_tool_router_select(hu_allocator_t *alloc, const char *message, size_t message_len,
                                  hu_tool_t *all_tools, size_t all_tools_count,
                                  hu_tool_selection_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    if (!all_tools || all_tools_count == 0)
        return HU_OK;

    /* Tokenize message */
    word_ref_t msg_refs[HU_TOOL_ROUTER_MAX_WORDS];
    size_t msg_count = 0;
    if (message && message_len > 0)
        msg_count = tokenize_refs(message, message_len, msg_refs);

    /* Always-include tools */
    for (size_t i = 0; i < all_tools_count && out->count < HU_TOOL_ROUTER_MAX_SELECTED; i++) {
        const hu_tool_t *t = &all_tools[i];
        if (!t->vtable || !t->vtable->name)
            continue;
        const char *name = t->vtable->name(t->ctx);
        if (name && is_always_tool(name)) {
            out->indices[out->count++] = i;
        }
    }

    /* Score and select remaining tools */
    scored_entry_t scored[HU_TOOL_ROUTER_MAX_SCORED];
    size_t scored_count = 0;
    for (size_t i = 0; i < all_tools_count && scored_count < HU_TOOL_ROUTER_MAX_SCORED; i++) {
        const hu_tool_t *t = &all_tools[i];
        if (!t->vtable || !t->vtable->name)
            continue;
        const char *name = t->vtable->name(t->ctx);
        if (!name || is_always_tool(name))
            continue;
        const char *desc = t->vtable->description ? t->vtable->description(t->ctx) : NULL;
        double s = score_tool(msg_refs, msg_count, name, desc);
        scored[scored_count].idx = i;
        scored[scored_count].score = s;
        scored_count++;
    }

    /* Sort by score descending */
    for (size_t i = 0; i < scored_count; i++) {
        for (size_t j = i + 1; j < scored_count; j++) {
            if (scored[j].score > scored[i].score) {
                scored_entry_t tmp = scored[i];
                scored[i] = scored[j];
                scored[j] = tmp;
            }
        }
    }

    /* Take top (MAX_SELECTED - always_count) */
    size_t take = HU_TOOL_ROUTER_MAX_SELECTED - out->count;
    if (take > scored_count)
        take = scored_count;
    for (size_t i = 0; i < take && out->count < HU_TOOL_ROUTER_MAX_SELECTED; i++) {
        out->indices[out->count++] = scored[i].idx;
    }

    return HU_OK;
}
