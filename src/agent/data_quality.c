#include "human/agent/data_quality.h"
#include "human/core/log.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

bool hu_dq_is_valid_utf8(const char *s, size_t len) {
    if (!s)
        return true;
    const unsigned char *p = (const unsigned char *)s;
    const unsigned char *end = p + len;

    while (p < end) {
        if (*p < 0x80) {
            p++;
        } else if ((*p & 0xE0) == 0xC0) {
            if (p + 1 >= end || (p[1] & 0xC0) != 0x80)
                return false;
            if (*p < 0xC2)
                return false;
            p += 2;
        } else if ((*p & 0xF0) == 0xE0) {
            if (p + 2 >= end || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80)
                return false;
            p += 3;
        } else if ((*p & 0xF8) == 0xF0) {
            if (p + 3 >= end || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 ||
                (p[3] & 0xC0) != 0x80)
                return false;
            p += 4;
        } else {
            return false;
        }
    }
    return true;
}

static void add_issue(hu_dq_result_t *out, hu_dq_issue_type_t type, size_t idx, const char *desc) {
    if (out->issue_count >= HU_DQ_MAX_ISSUES)
        return;
    hu_dq_issue_t *issue = &out->issues[out->issue_count];
    issue->type = type;
    issue->fragment_index = idx;
    size_t dlen = strlen(desc);
    if (dlen >= sizeof(issue->description))
        dlen = sizeof(issue->description) - 1;
    memcpy(issue->description, desc, dlen);
    issue->description[dlen] = '\0';
    out->issue_count++;
}

hu_error_t hu_dq_check(const hu_dq_config_t *config, const hu_dq_fragment_t *fragments,
                       size_t fragment_count, hu_dq_result_t *out) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    if (!config || !config->enabled || !fragments || fragment_count == 0) {
        if (config && !config->enabled) {
            static atomic_bool warned_data_quality_disabled = false;
            hu_log_info_once(&warned_data_quality_disabled, "data_quality", NULL,
                             "data quality checks disabled — set "
                             "data_quality.enabled=true in config.json to enable "
                             "fragment validation (currently default-pass)");
        }
        out->passed = true;
        out->fragments_checked = fragment_count;
        out->fragments_passed = fragment_count;
        return HU_OK;
    }

    out->fragments_checked = fragment_count;

    for (size_t i = 0; i < fragment_count; i++) {
        const hu_dq_fragment_t *f = &fragments[i];
        bool frag_ok = true;

        if (!f->content || f->content_len == 0) {
            add_issue(out, HU_DQ_EMPTY, i, "empty fragment");
            frag_ok = false;
        }

        if (config->max_fragment_len > 0 && f->content_len > config->max_fragment_len) {
            add_issue(out, HU_DQ_TOO_LONG, i, "fragment exceeds max length");
            frag_ok = false;
        }

        if (config->check_encoding && f->content && f->content_len > 0) {
            if (!hu_dq_is_valid_utf8(f->content, f->content_len)) {
                add_issue(out, HU_DQ_ENCODING_ERROR, i, "invalid UTF-8 encoding");
                frag_ok = false;
            }
        }

        if (config->deduplicate && f->content && f->content_len > 0) {
            for (size_t j = 0; j < i; j++) {
                if (fragments[j].content_len == f->content_len && fragments[j].content &&
                    memcmp(fragments[j].content, f->content, f->content_len) == 0) {
                    add_issue(out, HU_DQ_DUPLICATE, i, "exact duplicate of earlier fragment");
                    out->duplicates_found++;
                    frag_ok = false;
                    break;
                }
            }
        }

        if (frag_ok)
            out->fragments_passed++;
    }

    out->passed = (out->issue_count == 0);
    return HU_OK;
}

size_t hu_dq_report(const hu_dq_result_t *result, char *buf, size_t buf_size) {
    if (!result || !buf || buf_size == 0)
        return 0;
    int n = snprintf(buf, buf_size,
                     "Data Quality Report\n"
                     "===================\n"
                     "Fragments: %zu checked, %zu passed\n"
                     "Issues: %zu\n"
                     "Duplicates: %zu\n"
                     "Status: %s\n",
                     result->fragments_checked, result->fragments_passed, result->issue_count,
                     result->duplicates_found, result->passed ? "PASS" : "FAIL");
    if (n < 0)
        return 0;
    return (size_t)n < buf_size ? (size_t)n : buf_size - 1;
}
