/* daemon_outbound_sanitize.c — pre-split cleanup of the reactive reply.
 * Contract: include/human/daemon/outbound_sanitize.h.
 *
 * Moved verbatim from daemon.c's send block (2026-10-02) so the replay
 * harness and the daemon run one implementation. The only change: the
 * all-reasoning fallback now checks the buffer can hold it. */
#include "human/daemon/outbound_sanitize.h"

#include "human/core/log.h"
#include <string.h>

/* Strip invalid UTF-8 and surrogate-encoded garbage.
 * Keeps: ASCII printable, newlines, valid multi-byte UTF-8 (including
 * emoji). Strips: invalid sequences, lone surrogates (U+D800-U+DFFF encoded
 * as 3-byte). */
static void strip_invalid_utf8(char *response, size_t *response_len) {
    size_t len = *response_len;
    size_t w = 0;
    for (size_t r = 0; r < len;) {
        unsigned char b = (unsigned char)response[r];
        if (b < 0x80) {
            if (b >= 0x20 || b == '\n' || b == '\t')
                response[w++] = response[r];
            r++;
        } else {
            size_t seq = 0;
            if ((b & 0xE0) == 0xC0)
                seq = 2;
            else if ((b & 0xF0) == 0xE0)
                seq = 3;
            else if ((b & 0xF8) == 0xF0)
                seq = 4;
            if (seq == 0 || r + seq > len) {
                r++;
                continue;
            }
            bool valid = true;
            for (size_t k = 1; k < seq; k++) {
                if (((unsigned char)response[r + k] & 0xC0) != 0x80) {
                    valid = false;
                    break;
                }
            }
            /* Reject 3-byte sequences encoding surrogates (U+D800-U+DFFF) */
            if (valid && seq == 3) {
                unsigned int s_cp = ((b & 0x0F) << 12) |
                                    (((unsigned char)response[r + 1] & 0x3F) << 6) |
                                    ((unsigned char)response[r + 2] & 0x3F);
                if (s_cp >= 0xD800 && s_cp <= 0xDFFF)
                    valid = false;
            }
            if (valid) {
                if (w != r)
                    memmove(response + w, response + r, seq);
                w += seq;
            }
            r += valid ? seq : 1;
        }
    }
    if (w < len) {
        response[w] = '\0';
        *response_len = w;
    }
}

/* Strip meta-reasoning: local models sometimes emit (parenthetical
 * analysis) instead of just the message. Remove any leading text
 * up to and including the last ')' if the response starts with '('. */
static void strip_meta_reasoning(char *response, size_t *response_len, size_t cap,
                                 hu_observer_t *observer) {
    size_t len = *response_len;
    if (len == 0 || response[0] != '(')
        return;
    char *last_paren = NULL;
    for (size_t ri = 0; ri < len; ri++) {
        if (response[ri] == ')')
            last_paren = response + ri;
    }
    if (!last_paren)
        return;
    char *clean = last_paren + 1;
    while (*clean == ' ' || *clean == '\n' || *clean == '\r')
        clean++;
    size_t new_len = len - (size_t)(clean - response);
    if (new_len > 0 && new_len < len) {
        memmove(response, clean, new_len);
        response[new_len] = '\0';
        *response_len = new_len;
        hu_log_info("human", observer, "stripped meta-reasoning, clean len=%zu", new_len);
    } else if (new_len == 0) {
        /* Entire response was meta-reasoning; use a fallback */
        static const char fb[] = "hey whats up";
        if (cap >= sizeof(fb)) {
            memcpy(response, fb, sizeof(fb));
            *response_len = sizeof(fb) - 1;
        } else {
            response[0] = '\0';
            *response_len = 0;
        }
        hu_log_info("human", observer, "meta-reasoning fallback (entire response was reasoning)");
    }
}

void hu_daemon_outbound_sanitize(char *response, size_t *response_len, size_t cap, bool llm_decides,
                                 hu_observer_t *observer) {
    if (!response || !response_len || *response_len == 0)
        return;
    strip_invalid_utf8(response, response_len);
    if (llm_decides)
        strip_meta_reasoning(response, response_len, cap, observer);
}
