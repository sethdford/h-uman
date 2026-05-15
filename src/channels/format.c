/*
 * Per-channel outbound message formatting (plain text / mrkdwn / minimal HTML).
 */
#include "human/channels/format.h"
#include "human/agent/output_validator_chain.h"
#include "human/agent/validators/builtin.h"
#include "human/context/conversation.h"
#include "human/core/allocator.h"
#include "human/core/error.h"

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool channel_name_eq(const char *n, size_t nl, const char *lit, size_t lit_len) {
    return nl == lit_len && memcmp(n, lit, lit_len) == 0;
}

/* Same stripping loop as iMessage send() — drops emphasis/list/header/backtick markers. */
static size_t strip_markdown_core(const char *message, size_t message_len, char *clean) {
    size_t out_i = 0;
    size_t i = 0;
    while (i < message_len) {
        if (message[i] == '*') {
            while (i < message_len && message[i] == '*')
                i++;
            continue;
        }
        if ((i == 0 || message[i - 1] == '\n') && message[i] == '#') {
            while (i < message_len && message[i] == '#')
                i++;
            if (i < message_len && message[i] == ' ')
                i++;
            continue;
        }
        if ((i == 0 || message[i - 1] == '\n') && i + 1 < message_len &&
            (message[i] == '-' || message[i] == '*') && message[i + 1] == ' ') {
            i += 2;
            continue;
        }
        if (message[i] == '`') {
            i++;
            continue;
        }
        clean[out_i++] = message[i];
        i++;
    }
    clean[out_i] = '\0';
    return out_i;
}

/* Remove _word_ italic markers (alphanumeric words only; preserves snake_case mid-token). */
static size_t strip_underscore_italic(char *buf, size_t len) {
    size_t wi = 0;
    size_t ri = 0;
    while (ri < len) {
        if (buf[ri] == '_' && ri + 1 < len && isalnum((unsigned char)buf[ri + 1])) {
            size_t j = ri + 1;
            while (j < len && isalnum((unsigned char)buf[j]))
                j++;
            if (j < len && buf[j] == '_') {
                for (size_t k = ri + 1; k < j; k++)
                    buf[wi++] = buf[k];
                ri = j + 1;
                continue;
            }
        }
        buf[wi++] = buf[ri++];
    }
    buf[wi] = '\0';
    return wi;
}

hu_error_t hu_channel_strip_markdown(hu_allocator_t *alloc, const char *text, size_t text_len,
                                     char **out, size_t *out_len) {
    if (!alloc || !alloc->alloc || !alloc->free || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    if (text_len > 0 && !text)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    if (text_len == SIZE_MAX)
        return HU_ERR_INVALID_ARGUMENT;

    char *buf = (char *)alloc->alloc(alloc->ctx, text_len + 1);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    size_t n = strip_markdown_core(text, text_len, buf);
    n = strip_underscore_italic(buf, n);
    *out = buf;
    *out_len = n;
    return HU_OK;
}

/* Collapse double spaces and trim ends (phrases handled in hu_conversation_strip_ai_phrases). */
static size_t strip_channel_output_whitespace(char *buf, size_t len) {
    if (!buf || len == 0)
        return 0;

    for (size_t i = 1; i < len; i++) {
        if (buf[i] == ' ' && buf[i - 1] == ' ') {
            memmove(buf + i, buf + i + 1, len - i);
            len--;
            i--;
        }
    }

    while (len > 0 && (buf[0] == ' ' || buf[0] == '\t')) {
        memmove(buf, buf + 1, len);
        len--;
    }

    while (len > 0 && (buf[len - 1] == ' ' || buf[len - 1] == '\t')) {
        buf[--len] = '\0';
    }

    return len;
}

hu_error_t hu_channel_strip_ai_phrases(hu_allocator_t *alloc, const char *text, size_t text_len,
                                       char **out, size_t *out_len) {
    if (!alloc || !alloc->alloc || !alloc->free || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    if (text_len > 0 && !text)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    if (text_len == SIZE_MAX)
        return HU_ERR_INVALID_ARGUMENT;

    char *buf = (char *)alloc->alloc(alloc->ctx, text_len + 1);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    if (text_len > 0)
        memcpy(buf, text, text_len);
    buf[text_len] = '\0';

    /* Delegate to the canonical AI phrase list in conversation.c, then
     * run the format.c-specific extras (double space, trim) for compat. */
    size_t n = hu_conversation_strip_ai_phrases(buf, text_len);
    n = strip_channel_output_whitespace(buf, n);
    *out = buf;
    *out_len = n;
    return HU_OK;
}

static size_t truncate_segment_300(const char *msg, size_t message_len) {
    if (message_len <= 300)
        return message_len;

    size_t cut = 300;
    while (cut > 100 && cut < message_len && msg[cut] != '.' && msg[cut] != '!' && msg[cut] != '?')
        cut--;
    if (cut > 100)
        return cut + 1;

    size_t space_cut = 300;
    while (space_cut > 100 && space_cut < message_len && msg[space_cut] != ' ')
        space_cut--;
    if (space_cut > 100)
        return space_cut;
    return 300;
}

static hu_error_t cap_lines_at_300(hu_allocator_t *alloc, const char *text, size_t text_len,
                                   char **out, size_t *out_len) {
    if (text_len == 0) {
        char *empty = (char *)alloc->alloc(alloc->ctx, 1);
        if (!empty)
            return HU_ERR_OUT_OF_MEMORY;
        empty[0] = '\0';
        *out = empty;
        *out_len = 0;
        return HU_OK;
    }

    /* Output is never longer than input (truncation only) plus final NUL. */
    char *buf = (char *)alloc->alloc(alloc->ctx, text_len + 1);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    size_t w = 0;
    size_t line_start = 0;
    for (size_t i = 0; i <= text_len; i++) {
        if (i == text_len || text[i] == '\n') {
            size_t line_len = i - line_start;
            size_t seglen = truncate_segment_300(text + line_start, line_len);
            memcpy(buf + w, text + line_start, seglen);
            w += seglen;
            if (i < text_len)
                buf[w++] = '\n';
            line_start = i + 1;
        }
    }
    buf[w] = '\0';
    *out = buf;
    *out_len = w;
    return HU_OK;
}

static hu_error_t dup_trim_trailing_ws(hu_allocator_t *alloc, const char *text, size_t text_len,
                                       char **out, size_t *out_len) {
    while (text_len > 0 && isspace((unsigned char)text[text_len - 1]))
        text_len--;

    char *buf = (char *)alloc->alloc(alloc->ctx, text_len + 1);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    if (text_len > 0)
        memcpy(buf, text, text_len);
    buf[text_len] = '\0';
    *out = buf;
    *out_len = text_len;
    return HU_OK;
}

static hu_error_t buf_reserve(hu_allocator_t *alloc, char **buf, size_t *cap, size_t used,
                              size_t need) {
    if (need <= *cap)
        return HU_OK;
    size_t new_cap = *cap ? *cap * 2 : 256;
    while (new_cap < need) {
        if (new_cap > SIZE_MAX / 2)
            return HU_ERR_INVALID_ARGUMENT;
        new_cap *= 2;
    }
    if (alloc->realloc) {
        void *p = *buf ? alloc->realloc(alloc->ctx, *buf, *cap, new_cap)
                       : alloc->alloc(alloc->ctx, new_cap);
        if (!p)
            return HU_ERR_OUT_OF_MEMORY;
        *buf = (char *)p;
        *cap = new_cap;
        return HU_OK;
    }
    char *nb = (char *)alloc->alloc(alloc->ctx, new_cap);
    if (!nb)
        return HU_ERR_OUT_OF_MEMORY;
    if (*buf && used > 0)
        memcpy(nb, *buf, used);
    if (*buf)
        alloc->free(alloc->ctx, *buf, *cap);
    *buf = nb;
    *cap = new_cap;
    return HU_OK;
}

static hu_error_t append_bytes(hu_allocator_t *alloc, char **buf, size_t *len, size_t *cap,
                               const char *s, size_t slen) {
    hu_error_t e = buf_reserve(alloc, buf, cap, *len, *len + slen + 1);
    if (e != HU_OK)
        return e;
    memcpy(*buf + *len, s, slen);
    *len += slen;
    (*buf)[*len] = '\0';
    return HU_OK;
}

static hu_error_t slack_convert(const char *in, size_t in_len, hu_allocator_t *alloc, char **out,
                                size_t *out_len) {
    char *buf = NULL;
    size_t cap = 0;
    size_t w = 0;
    size_t i = 0;

    while (i < in_len) {
        if (in[i] == '[') {
            size_t close_bracket = i + 1;
            while (close_bracket < in_len && in[close_bracket] != ']')
                close_bracket++;
            if (close_bracket + 1 < in_len && in[close_bracket] == ']' &&
                in[close_bracket + 1] == '(') {
                size_t close_paren = close_bracket + 2;
                while (close_paren < in_len && in[close_paren] != ')')
                    close_paren++;
                if (close_paren < in_len) {
                    const char *link_text = in + (i + 1);
                    size_t link_text_len = close_bracket - (i + 1);
                    const char *url = in + (close_bracket + 2);
                    size_t url_len = close_paren - (close_bracket + 2);
                    hu_error_t e = append_bytes(alloc, &buf, &w, &cap, "<", 1);
                    if (e != HU_OK)
                        goto fail;
                    e = append_bytes(alloc, &buf, &w, &cap, url, url_len);
                    if (e != HU_OK)
                        goto fail;
                    e = append_bytes(alloc, &buf, &w, &cap, "|", 1);
                    if (e != HU_OK)
                        goto fail;
                    e = append_bytes(alloc, &buf, &w, &cap, link_text, link_text_len);
                    if (e != HU_OK)
                        goto fail;
                    e = append_bytes(alloc, &buf, &w, &cap, ">", 1);
                    if (e != HU_OK)
                        goto fail;
                    i = close_paren + 1;
                    continue;
                }
            }
        }

        if (i + 1 < in_len && in[i] == '*' && in[i + 1] == '*') {
            size_t j = i + 2;
            while (j + 1 < in_len && !(in[j] == '*' && in[j + 1] == '*'))
                j++;
            if (j + 1 < in_len) {
                const char *mid = in + (i + 2);
                size_t mid_len = j - (i + 2);
                hu_error_t e = append_bytes(alloc, &buf, &w, &cap, "*", 1);
                if (e != HU_OK)
                    goto fail;
                e = append_bytes(alloc, &buf, &w, &cap, mid, mid_len);
                if (e != HU_OK)
                    goto fail;
                e = append_bytes(alloc, &buf, &w, &cap, "*", 1);
                if (e != HU_OK)
                    goto fail;
                i = j + 2;
                continue;
            }
        }

        hu_error_t e2 = append_bytes(alloc, &buf, &w, &cap, in + i, 1);
        if (e2 != HU_OK)
            goto fail;
        i++;
    }

    if (!buf) {
        buf = (char *)alloc->alloc(alloc->ctx, 1);
        if (!buf)
            return HU_ERR_OUT_OF_MEMORY;
        buf[0] = '\0';
        *out = buf;
        *out_len = 0;
        return HU_OK;
    }

    *out = buf;
    *out_len = w;
    return HU_OK;

fail:
    if (buf)
        alloc->free(alloc->ctx, buf, cap);
    *out = NULL;
    *out_len = 0;
    return HU_ERR_OUT_OF_MEMORY;
}

static void html_escape_into(const char *s, size_t n, char *dst, size_t *wi) {
    for (size_t i = 0; i < n; i++) {
        switch (s[i]) {
        case '&':
            memcpy(dst + *wi, "&amp;", 5);
            *wi += 5;
            break;
        case '<':
            memcpy(dst + *wi, "&lt;", 4);
            *wi += 4;
            break;
        case '>':
            memcpy(dst + *wi, "&gt;", 4);
            *wi += 4;
            break;
        case '"':
            memcpy(dst + *wi, "&quot;", 6);
            *wi += 6;
            break;
        default:
            dst[(*wi)++] = s[i];
            break;
        }
    }
}

/* Apply **strong** and *em* within a span; assumes no nested markers of same kind. */
static hu_error_t email_inline_format(hu_allocator_t *alloc, const char *para, size_t para_len,
                                      char **out, size_t *out_len) {
    /* Worst case: each byte -> &quot; (6) plus inline tags. */
    size_t est = para_len * 12 + 256;
    if (est < para_len + 64)
        est = para_len + 64;
    if (est < 256)
        est = 256;

    char *buf = (char *)alloc->alloc(alloc->ctx, est);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    size_t w = 0;
    size_t i = 0;

    while (i < para_len) {
        if (i + 1 < para_len && para[i] == '*' && para[i + 1] == '*') {
            size_t j = i + 2;
            while (j + 1 < para_len && !(para[j] == '*' && para[j + 1] == '*'))
                j++;
            if (j + 1 < para_len) {
                memcpy(buf + w, "<strong>", 8);
                w += 8;
                html_escape_into(para + i + 2, j - (i + 2), buf, &w);
                memcpy(buf + w, "</strong>", 9);
                w += 9;
                i = j + 2;
                continue;
            }
        }
        if (para[i] == '*') {
            size_t j = i + 1;
            while (j < para_len && para[j] != '*')
                j++;
            if (j < para_len) {
                memcpy(buf + w, "<em>", 4);
                w += 4;
                html_escape_into(para + i + 1, j - (i + 1), buf, &w);
                memcpy(buf + w, "</em>", 5);
                w += 5;
                i = j + 1;
                continue;
            }
        }
        if (para[i] == '_') {
            size_t j = i + 1;
            while (j < para_len && para[j] != '_')
                j++;
            if (j < para_len) {
                memcpy(buf + w, "<em>", 4);
                w += 4;
                html_escape_into(para + i + 1, j - (i + 1), buf, &w);
                memcpy(buf + w, "</em>", 5);
                w += 5;
                i = j + 1;
                continue;
            }
        }
        size_t run = 1;
        while (i + run < para_len && para[i + run] != '*' && para[i + run] != '_')
            run++;
        for (size_t k = 0; k < run; k++) {
            if (para[i + k] == '\n') {
                const char *br = "<br/>";
                if (w + 5 + 1 > est) {
                    size_t ncap = (est + 5) * 2;
                    char *nb = (char *)alloc->alloc(alloc->ctx, ncap);
                    if (!nb) {
                        alloc->free(alloc->ctx, buf, est);
                        return HU_ERR_OUT_OF_MEMORY;
                    }
                    memcpy(nb, buf, w);
                    alloc->free(alloc->ctx, buf, est);
                    buf = nb;
                    est = ncap;
                }
                memcpy(buf + w, br, 5);
                w += 5;
            } else {
                if (w + 8 + 1 > est) {
                    size_t ncap = est * 2;
                    if (ncap < w + 8 + 1)
                        ncap = w + 8 + 256;
                    char *nb = (char *)alloc->alloc(alloc->ctx, ncap);
                    if (!nb) {
                        alloc->free(alloc->ctx, buf, est);
                        return HU_ERR_OUT_OF_MEMORY;
                    }
                    memcpy(nb, buf, w);
                    alloc->free(alloc->ctx, buf, est);
                    buf = nb;
                    est = ncap;
                }
                html_escape_into(para + i + k, 1, buf, &w);
            }
        }
        i += run;
    }

    buf[w] = '\0';
    *out = buf;
    *out_len = w;
    return HU_OK;
}

static hu_error_t format_email_html(hu_allocator_t *alloc, const char *text, size_t text_len,
                                    char **out, size_t *out_len) {
    char *acc = NULL;
    size_t acc_len = 0;
    size_t acc_cap = 0;

    size_t para_start = 0;
    for (size_t i = 0; i <= text_len; i++) {
        bool boundary =
            (i == text_len) || (i + 1 < text_len && text[i] == '\n' && text[i + 1] == '\n');
        if (boundary) {
            size_t para_end = i;
            while (para_start < para_end && (text[para_start] == '\n' || text[para_start] == '\r'))
                para_start++;
            while (para_end > para_start &&
                   (text[para_end - 1] == '\n' || text[para_end - 1] == '\r'))
                para_end--;

            if (para_end > para_start) {
                char *inline_html = NULL;
                size_t inline_len = 0;
                hu_error_t e = email_inline_format(alloc, text + para_start, para_end - para_start,
                                                   &inline_html, &inline_len);
                if (e != HU_OK) {
                    if (acc)
                        alloc->free(alloc->ctx, acc, acc_cap);
                    return e;
                }
                hu_error_t e2 = append_bytes(alloc, &acc, &acc_len, &acc_cap, "<p>", 3);
                if (e2 != HU_OK) {
                    alloc->free(alloc->ctx, inline_html, inline_len + 1);
                    if (acc)
                        alloc->free(alloc->ctx, acc, acc_cap);
                    return e2;
                }
                e2 = append_bytes(alloc, &acc, &acc_len, &acc_cap, inline_html, inline_len);
                alloc->free(alloc->ctx, inline_html, inline_len + 1);
                if (e2 != HU_OK) {
                    if (acc)
                        alloc->free(alloc->ctx, acc, acc_cap);
                    return e2;
                }
                e2 = append_bytes(alloc, &acc, &acc_len, &acc_cap, "</p>\n", 5);
                if (e2 != HU_OK) {
                    if (acc)
                        alloc->free(alloc->ctx, acc, acc_cap);
                    return e2;
                }
            }
            if (i + 1 < text_len && text[i] == '\n' && text[i + 1] == '\n') {
                para_start = i + 2;
                i++;
            }
        }
    }

    if (!acc) {
        const char *empty = "<p></p>\n";
        char *buf = (char *)alloc->alloc(alloc->ctx, strlen(empty) + 1);
        if (!buf)
            return HU_ERR_OUT_OF_MEMORY;
        memcpy(buf, empty, strlen(empty) + 1);
        *out = buf;
        *out_len = strlen(empty);
        return HU_OK;
    }

    *out = acc;
    *out_len = acc_len;
    return HU_OK;
}

static hu_error_t dup_exact(hu_allocator_t *alloc, const char *text, size_t text_len, char **out,
                            size_t *out_len) {
    char *buf = (char *)alloc->alloc(alloc->ctx, text_len + 1);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    if (text_len > 0)
        memcpy(buf, text, text_len);
    buf[text_len] = '\0';
    *out = buf;
    *out_len = text_len;
    return HU_OK;
}

hu_error_t hu_channel_format_outbound(hu_allocator_t *alloc, const char *channel_name,
                                      size_t channel_name_len, const char *text, size_t text_len,
                                      char **out, size_t *out_len) {
    if (!alloc || !alloc->alloc || !alloc->free || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    if (text_len > 0 && !text)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    if (channel_name_eq(channel_name, channel_name_len, "discord", 7) ||
        channel_name_eq(channel_name, channel_name_len, "telegram", 8)) {
        /* Run outbound validator chain (Pattern C — closes assistant-closer
         * escape path), then trim trailing whitespace. On REJECT, suppress
         * (return empty string). On chain-build failure, fall back to legacy
         * ai-phrase strip before the trim step. */
        char *san = NULL;
        size_t san_len = 0;
        hu_error_t e = HU_OK;
        {
            hu_output_validator_chain_t *out_chain = NULL;
            if (hu_validators_build_default_outbound_chain(alloc, NULL, 0, &out_chain) == HU_OK) {
                hu_chain_result_t cr;
                memset(&cr, 0, sizeof(cr));
                if (hu_output_validator_chain_execute(out_chain, alloc, NULL, text, text_len,
                                                      &cr) == HU_OK) {
                    if (cr.final_decision != HU_VALIDATOR_REJECT && cr.final_text &&
                        cr.final_text_len > 0) {
                        san_len = cr.final_text_len;
                        san = (char *)alloc->alloc(alloc->ctx, san_len + 1);
                        if (san) {
                            memcpy(san, cr.final_text, san_len);
                            san[san_len] = '\0';
                        } else {
                            hu_chain_result_free(alloc, &cr);
                            hu_output_validator_chain_destroy(out_chain);
                            return HU_ERR_OUT_OF_MEMORY;
                        }
                    }
                    /* On REJECT: san stays NULL/0 — message suppressed. */
                }
                hu_chain_result_free(alloc, &cr);
                hu_output_validator_chain_destroy(out_chain);
            } else {
                /* Chain build failed: fall back to legacy ai-phrase strip. */
                e = hu_channel_strip_ai_phrases(alloc, text, text_len, &san, &san_len);
            }
        }
        if (e != HU_OK)
            return e;
        if (!san || san_len == 0) {
            *out = (char *)alloc->alloc(alloc->ctx, 1);
            if (!*out)
                return HU_ERR_OUT_OF_MEMORY;
            (*out)[0] = '\0';
            *out_len = 0;
            return HU_OK;
        }
        hu_error_t trim_e = dup_trim_trailing_ws(alloc, san, san_len, out, out_len);
        alloc->free(alloc->ctx, san, san_len + 1);
        return trim_e;
    }

    if (channel_name_eq(channel_name, channel_name_len, "slack", 5)) {
        /* Run outbound validator chain (Pattern C — closes assistant-closer
         * escape path), then apply Slack mrkdwn conversion. On REJECT,
         * suppress (return empty string). On chain-build failure, fall back to
         * legacy ai-phrase strip before Slack conversion. */
        char *san = NULL;
        size_t san_len = 0;
        hu_error_t e = HU_OK;
        {
            hu_output_validator_chain_t *out_chain = NULL;
            if (hu_validators_build_default_outbound_chain(alloc, NULL, 0, &out_chain) == HU_OK) {
                hu_chain_result_t cr;
                memset(&cr, 0, sizeof(cr));
                if (hu_output_validator_chain_execute(out_chain, alloc, NULL, text, text_len,
                                                      &cr) == HU_OK) {
                    if (cr.final_decision != HU_VALIDATOR_REJECT && cr.final_text &&
                        cr.final_text_len > 0) {
                        san_len = cr.final_text_len;
                        san = (char *)alloc->alloc(alloc->ctx, san_len + 1);
                        if (san) {
                            memcpy(san, cr.final_text, san_len);
                            san[san_len] = '\0';
                        } else {
                            hu_chain_result_free(alloc, &cr);
                            hu_output_validator_chain_destroy(out_chain);
                            return HU_ERR_OUT_OF_MEMORY;
                        }
                    }
                    /* On REJECT: san stays NULL/0 — message suppressed. */
                }
                hu_chain_result_free(alloc, &cr);
                hu_output_validator_chain_destroy(out_chain);
            } else {
                /* Chain build failed: fall back to legacy ai-phrase strip. */
                e = hu_channel_strip_ai_phrases(alloc, text, text_len, &san, &san_len);
            }
        }
        if (e != HU_OK)
            return e;
        if (!san || san_len == 0) {
            *out = (char *)alloc->alloc(alloc->ctx, 1);
            if (!*out)
                return HU_ERR_OUT_OF_MEMORY;
            (*out)[0] = '\0';
            *out_len = 0;
            return HU_OK;
        }
        hu_error_t conv_e = slack_convert(san, san_len, alloc, out, out_len);
        alloc->free(alloc->ctx, san, san_len + 1);
        return conv_e;
    }

    if (channel_name_eq(channel_name, channel_name_len, "email", 5))
        return format_email_html(alloc, text, text_len, out, out_len);

    if (channel_name_eq(channel_name, channel_name_len, "imessage", 8)) {
        /* Step 1: strip markdown (formatting, not safety — kept as-is). */
        char *md = NULL;
        size_t md_len = 0;
        hu_error_t e = hu_channel_strip_markdown(alloc, text, text_len, &md, &md_len);
        if (e != HU_OK)
            return e;

        /* Step 2: run the outbound validator chain instead of bare
         * hu_channel_strip_ai_phrases.  This covers ai-phrase strip (F2
         * assistant-closer), channel-tag strip, and future safety validators,
         * closing the Pattern C escape path. On REJECT the message is
         * suppressed (returns empty string); on PASS/REWRITE the cleaned text
         * flows to the line-cap step below. */
        char *san = NULL;
        size_t san_len = 0;
        {
            hu_output_validator_chain_t *out_chain = NULL;
            if (hu_validators_build_default_outbound_chain(alloc, NULL, 0, &out_chain) == HU_OK) {
                hu_chain_result_t cr;
                memset(&cr, 0, sizeof(cr));
                if (hu_output_validator_chain_execute(out_chain, alloc, NULL, md, md_len, &cr) ==
                    HU_OK) {
                    if (cr.final_decision != HU_VALIDATOR_REJECT && cr.final_text &&
                        cr.final_text_len > 0) {
                        san_len = cr.final_text_len;
                        san = (char *)alloc->alloc(alloc->ctx, san_len + 1);
                        if (san) {
                            memcpy(san, cr.final_text, san_len);
                            san[san_len] = '\0';
                        } else {
                            hu_chain_result_free(alloc, &cr);
                            hu_output_validator_chain_destroy(out_chain);
                            alloc->free(alloc->ctx, md, md_len + 1);
                            return HU_ERR_OUT_OF_MEMORY;
                        }
                    }
                    /* On REJECT: san stays NULL/0 — message suppressed. */
                }
                hu_chain_result_free(alloc, &cr);
                hu_output_validator_chain_destroy(out_chain);
            } else {
                /* Chain build failed: fall back to legacy ai-phrase strip. */
                e = hu_channel_strip_ai_phrases(alloc, md, md_len, &san, &san_len);
            }
        }
        alloc->free(alloc->ctx, md, md_len + 1);
        if (e != HU_OK)
            return e;

        if (!san || san_len == 0) {
            /* Rejected or empty — return empty string. */
            *out = (char *)alloc->alloc(alloc->ctx, 1);
            if (!*out)
                return HU_ERR_OUT_OF_MEMORY;
            (*out)[0] = '\0';
            *out_len = 0;
            return HU_OK;
        }

        char *capped = NULL;
        size_t capped_len = 0;
        e = cap_lines_at_300(alloc, san, san_len, &capped, &capped_len);
        alloc->free(alloc->ctx, san, san_len + 1);
        if (e != HU_OK)
            return e;

        *out = capped;
        *out_len = capped_len;
        return HU_OK;
    }

    return dup_exact(alloc, text, text_len, out, out_len);
}
