#include "human/channels/channel_mock.h"

#if defined(HU_IS_TEST) && HU_IS_TEST

#include <string.h>

hu_error_t hu_channel_mock_inject(hu_channel_mock_t *m, const char *session_key,
                                  size_t session_key_len, const char *content, size_t content_len) {
    if (!m)
        return HU_ERR_INVALID_ARGUMENT;
    if (m->count >= HU_CHANNEL_MOCK_MAX_MSGS)
        return HU_ERR_OUT_OF_MEMORY;
    size_t i = m->count++;
    size_t sk = session_key_len > HU_CHANNEL_MOCK_KEY_CAP - 1 ? HU_CHANNEL_MOCK_KEY_CAP - 1
                                                              : session_key_len;
    size_t ct = content_len > HU_CHANNEL_MOCK_CONTENT_CAP - 1 ? HU_CHANNEL_MOCK_CONTENT_CAP - 1
                                                              : content_len;
    if (session_key && sk > 0)
        memcpy(m->msgs[i].session_key, session_key, sk);
    m->msgs[i].session_key[sk] = '\0';
    if (content && ct > 0)
        memcpy(m->msgs[i].content, content, ct);
    m->msgs[i].content[ct] = '\0';
    return HU_OK;
}

void hu_channel_mock_record_send(hu_channel_mock_t *m, const char *text, size_t len) {
    if (!m)
        return;
    size_t n = len > HU_CHANNEL_MOCK_CONTENT_CAP - 1 ? HU_CHANNEL_MOCK_CONTENT_CAP - 1 : len;
    if (text && n > 0)
        memcpy(m->last_message, text, n);
    m->last_message[n] = '\0';
    m->last_message_len = n;
}

#else

/* ISO C forbids an empty translation unit. */
typedef int hu_channel_mock_not_a_test_build_t;

#endif /* HU_IS_TEST */
