#include "human/agent/info_asymmetry.h"
#include "human/core/string.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define TOPIC_PREFIX     "- "
#define TOPIC_PREFIX_LEN 2

static bool topic_in_list(const char *topic, size_t topic_len, const char **list, size_t *lens,
                          size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (lens[i] == topic_len && memcmp(list[i], topic, topic_len) == 0)
            return true;
    }
    return false;
}

static size_t extract_topics(const char *text, size_t text_len, char **out_topics, size_t *out_lens,
                             size_t max_out, hu_allocator_t *alloc) {
    size_t count = 0;
    const char *p = text;
    const char *end = text + text_len;

    while (p < end && count < max_out) {
        const char *line_start = p;
        while (p < end && *p != '\n')
            p++;
        size_t line_len = (size_t)(p - line_start);
        if (p < end)
            p++;

        /* Skip leading whitespace */
        while (line_len > 0 && (line_start[0] == ' ' || line_start[0] == '\t')) {
            line_start++;
            line_len--;
        }
        if (line_len < TOPIC_PREFIX_LEN)
            continue;
        if (memcmp(line_start, TOPIC_PREFIX, TOPIC_PREFIX_LEN) != 0)
            continue;

        const char *topic_start = line_start + TOPIC_PREFIX_LEN;
        size_t topic_len = line_len - TOPIC_PREFIX_LEN;
        while (topic_len > 0 &&
               (topic_start[topic_len - 1] == ' ' || topic_start[topic_len - 1] == '\t'))
            topic_len--;

        if (topic_len == 0)
            continue;

        char *dup = hu_strndup(alloc, topic_start, topic_len);
        if (!dup)
            break;
        out_topics[count] = dup;
        out_lens[count] = topic_len;
        count++;
    }
    return count;
}

hu_error_t hu_info_asymmetry_analyze(hu_info_asymmetry_t *result, hu_allocator_t *alloc,
                                     const char *agent_context, size_t agent_context_len,
                                     const char *contact_context, size_t contact_context_len) {
    if (!result || !alloc)
        return HU_ERR_INVALID_ARGUMENT;

    memset(result, 0, sizeof(*result));

    char *agent_topics[HU_INFO_GAP_MAX];
    size_t agent_lens[HU_INFO_GAP_MAX];
    size_t agent_count = 0;
    if (agent_context && agent_context_len > 0) {
        agent_count = extract_topics(agent_context, agent_context_len, agent_topics, agent_lens,
                                     HU_INFO_GAP_MAX, alloc);
    }

    char *contact_topics[HU_INFO_GAP_MAX];
    size_t contact_lens[HU_INFO_GAP_MAX];
    size_t contact_count = 0;
    if (contact_context && contact_context_len > 0) {
        contact_count = extract_topics(contact_context, contact_context_len, contact_topics,
                                       contact_lens, HU_INFO_GAP_MAX, alloc);
    }

    /* Classify and add to result */
    for (size_t i = 0; i < agent_count && result->gap_count < HU_INFO_GAP_MAX; i++) {
        bool in_contact = topic_in_list(agent_topics[i], agent_lens[i],
                                        (const char **)contact_topics, contact_lens, contact_count);
        hu_info_gap_type_t type = in_contact ? HU_GAP_SHARED : HU_GAP_AGENT_KNOWS;

        result->gaps[result->gap_count].topic = agent_topics[i];
        result->gaps[result->gap_count].topic_len = agent_lens[i];
        result->gaps[result->gap_count].type = type;
        result->gaps[result->gap_count].disclosure_appropriate = true;
        result->gap_count++;
    }

    for (size_t i = 0; i < contact_count && result->gap_count < HU_INFO_GAP_MAX; i++) {
        bool in_agent = topic_in_list(contact_topics[i], contact_lens[i],
                                      (const char **)agent_topics, agent_lens, agent_count);
        if (in_agent)
            continue; /* already added as SHARED */

        result->gaps[result->gap_count].topic = contact_topics[i];
        result->gaps[result->gap_count].topic_len = contact_lens[i];
        result->gaps[result->gap_count].type = HU_GAP_CONTACT_KNOWS;
        result->gaps[result->gap_count].disclosure_appropriate = true;
        result->gap_count++;
    }

    /* Free agent/contact topics that were not transferred to result (SHARED case: agent owns) */
    for (size_t i = 0; i < agent_count; i++) {
        bool transferred = false;
        for (size_t j = 0; j < result->gap_count; j++) {
            if (result->gaps[j].topic == agent_topics[i]) {
                transferred = true;
                break;
            }
        }
        if (!transferred)
            hu_str_free(alloc, agent_topics[i]);
    }
    for (size_t i = 0; i < contact_count; i++) {
        bool transferred = false;
        for (size_t j = 0; j < result->gap_count; j++) {
            if (result->gaps[j].topic == contact_topics[i]) {
                transferred = true;
                break;
            }
        }
        if (!transferred)
            hu_str_free(alloc, contact_topics[i]);
    }

    return HU_OK;
}

hu_error_t hu_info_asymmetry_build_guidance(const hu_info_asymmetry_t *asym, hu_allocator_t *alloc,
                                            char **out, size_t *out_len) {
    if (!asym || !alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    *out = NULL;
    *out_len = 0;

    if (asym->gap_count == 0)
        return HU_OK;

    char buf[4096];
    size_t pos = 0;

    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "### Information Awareness\n");

    /* You know but they don't */
    bool first = true;
    for (size_t i = 0; i < asym->gap_count; i++) {
        if (asym->gaps[i].type != HU_GAP_AGENT_KNOWS)
            continue;
        if (first) {
            pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "You know but they don't: ");
            first = false;
        } else {
            pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, ", ");
        }
        size_t n = asym->gaps[i].topic_len;
        if (pos + n + 2 < sizeof(buf)) {
            memcpy(buf + pos, asym->gaps[i].topic, n);
            pos += n;
        }
    }
    if (!first)
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "\n");

    /* They know but you don't */
    first = true;
    for (size_t i = 0; i < asym->gap_count; i++) {
        if (asym->gaps[i].type != HU_GAP_CONTACT_KNOWS)
            continue;
        if (first) {
            pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "They know but you don't: ");
            first = false;
        } else {
            pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, ", ");
        }
        size_t n = asym->gaps[i].topic_len;
        if (pos + n + 2 < sizeof(buf)) {
            memcpy(buf + pos, asym->gaps[i].topic, n);
            pos += n;
        }
    }
    if (!first)
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "\n");

    /* Shared knowledge */
    first = true;
    for (size_t i = 0; i < asym->gap_count; i++) {
        if (asym->gaps[i].type != HU_GAP_SHARED)
            continue;
        if (first) {
            pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "Shared knowledge: ");
            first = false;
        } else {
            pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, ", ");
        }
        size_t n = asym->gaps[i].topic_len;
        if (pos + n + 2 < sizeof(buf)) {
            memcpy(buf + pos, asym->gaps[i].topic, n);
            pos += n;
        }
    }
    if (!first)
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "\n");

    *out = hu_strndup(alloc, buf, pos);
    if (!*out)
        return HU_ERR_OUT_OF_MEMORY;
    *out_len = pos;
    return HU_OK;
}

void hu_info_asymmetry_deinit(hu_info_asymmetry_t *asym, hu_allocator_t *alloc) {
    if (!asym || !alloc)
        return;
    for (size_t i = 0; i < asym->gap_count; i++) {
        if (asym->gaps[i].topic) {
            hu_str_free(alloc, asym->gaps[i].topic);
            asym->gaps[i].topic = NULL;
        }
        asym->gaps[i].topic_len = 0;
    }
    asym->gap_count = 0;
}
