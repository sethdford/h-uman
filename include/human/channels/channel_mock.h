/* Shared test-only mock harness for channel implementations.
 *
 * Every channel used to carry its own copy of this struct plus an
 * inject/get-last-message pair inside `#if HU_IS_TEST` — 27 byte-identical
 * copies, ~600 lines of test scaffolding living in production files
 * (2026-09-10 code review). Channels now embed one `hu_channel_mock_t` and
 * forward their `hu_<channel>_test_inject_mock` entry point here.
 *
 * Compiles to nothing outside test builds. */
#ifndef HUMAN_CHANNELS_CHANNEL_MOCK_H
#define HUMAN_CHANNELS_CHANNEL_MOCK_H

#include "human/core/error.h"
#include <stddef.h>

#if defined(HU_IS_TEST) && HU_IS_TEST

#define HU_CHANNEL_MOCK_MAX_MSGS    8
#define HU_CHANNEL_MOCK_KEY_CAP     128
#define HU_CHANNEL_MOCK_CONTENT_CAP 4096

typedef struct hu_channel_mock_msg {
    char session_key[HU_CHANNEL_MOCK_KEY_CAP];
    char content[HU_CHANNEL_MOCK_CONTENT_CAP];
} hu_channel_mock_msg_t;

typedef struct hu_channel_mock {
    char last_message[HU_CHANNEL_MOCK_CONTENT_CAP];
    size_t last_message_len;
    hu_channel_mock_msg_t msgs[HU_CHANNEL_MOCK_MAX_MSGS];
    size_t count;
} hu_channel_mock_t;

/* Queue an inbound message for the channel's next poll. Truncates the key to
 * 127 bytes and the content to 4095 bytes (the historical per-channel caps);
 * returns HU_ERR_OUT_OF_MEMORY when all 8 slots are taken. */
hu_error_t hu_channel_mock_inject(hu_channel_mock_t *m, const char *session_key,
                                  size_t session_key_len, const char *content, size_t content_len);

/* Record the most recent outbound message (the send path calls this). */
void hu_channel_mock_record_send(hu_channel_mock_t *m, const char *text, size_t len);

#endif /* HU_IS_TEST */

#endif /* HUMAN_CHANNELS_CHANNEL_MOCK_H */
