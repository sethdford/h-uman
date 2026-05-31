#ifndef HU_CHANNELS_FORMAT_H
#define HU_CHANNELS_FORMAT_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>

/*
 * Per-channel outbound message formatting.
 *
 * On success, *out is NUL-terminated; *out_len is the byte length excluding the
 * terminator. Caller frees with: alloc->free(alloc->ctx, *out, *out_len + 1).
 */

hu_error_t hu_channel_format_outbound(hu_allocator_t *alloc, const char *channel_name,
                                      size_t channel_name_len, const char *text, size_t text_len,
                                      char **out, size_t *out_len);

hu_error_t hu_channel_strip_markdown(hu_allocator_t *alloc, const char *text, size_t text_len,
                                     char **out, size_t *out_len);

hu_error_t hu_channel_strip_ai_phrases(hu_allocator_t *alloc, const char *text, size_t text_len,
                                       char **out, size_t *out_len);

/*
 * Plaintext-ify outbound text for the pre-split sanitize (daemon bubble path).
 *
 * For plaintext channels (everything except slack/email) runs the FULL channel
 * sanitizer (strip_markdown + outbound validator chain: ai-phrase + assistant-
 * closer strip), bringing bubbled / choreographed replies to parity with the
 * single whole-reply fallback. For markup channels (slack→mrkdwn, email→HTML)
 * runs only strip_markdown — format_outbound would CONVERT those to markup, which
 * must never be fed to the bubble splitter.
 *
 * On an empty result (validator chain REJECT, or all-markup input) returns HU_OK
 * with *out == NULL / *out_len == 0 so the caller keeps the raw text. On success
 * *out is NUL-terminated; free with alloc->free(ctx, *out, *out_len + 1).
 */
hu_error_t hu_channel_plaintext_for_split(hu_allocator_t *alloc, const char *channel_name,
                                          size_t channel_name_len, const char *text,
                                          size_t text_len, char **out, size_t *out_len);

#endif /* HU_CHANNELS_FORMAT_H */
