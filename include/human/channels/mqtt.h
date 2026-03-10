#ifndef HU_CHANNELS_MQTT_H
#define HU_CHANNELS_MQTT_H
#include "human/channel.h"
#include "human/channel_loop.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>

hu_error_t hu_mqtt_poll(void *channel_ctx, hu_allocator_t *alloc, hu_channel_loop_msg_t *msgs,
                        size_t max_msgs, size_t *out_count);

hu_error_t hu_mqtt_create(hu_allocator_t *alloc, const char *broker_url, size_t broker_url_len,
                          const char *inbound_topic, size_t inbound_topic_len,
                          const char *outbound_topic, size_t outbound_topic_len,
                          const char *username, size_t username_len, const char *password,
                          size_t password_len, int qos, hu_channel_t *out);
void hu_mqtt_destroy(hu_channel_t *ch, hu_allocator_t *alloc);

#if HU_IS_TEST
hu_error_t hu_mqtt_test_inject_mock(hu_channel_t *ch, const char *session_key,
                                    size_t session_key_len, const char *content,
                                    size_t content_len);
const char *hu_mqtt_test_get_last_message(hu_channel_t *ch, size_t *out_len);
#endif
#endif
