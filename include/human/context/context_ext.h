#ifndef HU_CONTEXT_EXT_H
#define HU_CONTEXT_EXT_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- F47: Content Forwarding --- */
typedef struct hu_shareable_content {
    char *content;
    size_t content_len;
    char *source;
    size_t source_len; /* "contact:alice", "web", "rss" */
    char *topic;
    size_t topic_len;
    uint64_t received_at;
    double share_score;
} hu_shareable_content_t;

hu_error_t hu_forwarding_query_for_contact_sql(const char *contact_id, size_t len, char *buf,
                                               size_t cap, size_t *out_len);
/* --- F51: Weather Context --- */
typedef struct hu_weather_state {
    char *condition;
    size_t condition_len; /* "sunny", "raining", "snowing" */
    double temp_f;
    char *location;
    size_t location_len;
    bool is_notable; /* extreme temp, storm, first nice day */
} hu_weather_state_t;

/* --- F52: Current Events --- */
typedef struct hu_current_event {
    char *topic;
    size_t topic_len;
    char *summary;
    size_t summary_len;
    char *source;
    size_t source_len;
    uint64_t published_at;
    double relevance;
} hu_current_event_t;

hu_error_t hu_events_create_table_sql(char *buf, size_t cap, size_t *out_len);
/* --- F55-F57: Group Chat --- */
typedef struct hu_group_chat_state {
    uint32_t total_messages;  /* messages in last hour */
    uint32_t our_messages;    /* our messages in last hour */
    double response_rate;     /* configured from persona */
    bool was_mentioned;       /* @ mentioned */
    bool has_direct_question; /* question directed at us */
    uint32_t active_participants;
} hu_group_chat_state_t;

#endif /* HU_CONTEXT_EXT_H */
