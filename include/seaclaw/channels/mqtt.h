#ifndef SC_CHANNELS_MQTT_H
#define SC_CHANNELS_MQTT_H
#include "seaclaw/channel.h"
#include "seaclaw/channel_loop.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stddef.h>

sc_error_t sc_mqtt_poll(void *channel_ctx, sc_allocator_t *alloc, sc_channel_loop_msg_t *msgs,
                        size_t max_msgs, size_t *out_count);

sc_error_t sc_mqtt_create(sc_allocator_t *alloc, const char *broker_url, size_t broker_url_len,
                          const char *inbound_topic, size_t inbound_topic_len,
                          const char *outbound_topic, size_t outbound_topic_len,
                          const char *username, size_t username_len, const char *password,
                          size_t password_len, int qos, sc_channel_t *out);
void sc_mqtt_destroy(sc_channel_t *ch, sc_allocator_t *alloc);

#if SC_IS_TEST
sc_error_t sc_mqtt_test_inject_mock(sc_channel_t *ch, const char *session_key,
                                    size_t session_key_len, const char *content,
                                    size_t content_len);
const char *sc_mqtt_test_get_last_message(sc_channel_t *ch, size_t *out_len);
#endif
#endif
