#ifndef HU_STM_H
#define HU_STM_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/slice.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HU_STM_MAX_TURNS    20
#define HU_STM_MAX_ENTITIES 50
#define HU_STM_MAX_TOPICS   10
#define HU_STM_MAX_EMOTIONS 5

typedef enum hu_emotion_tag {
    HU_EMOTION_NEUTRAL,
    HU_EMOTION_JOY,
    HU_EMOTION_SADNESS,
    HU_EMOTION_ANGER,
    HU_EMOTION_FEAR,
    HU_EMOTION_SURPRISE,
    HU_EMOTION_FRUSTRATION,
    HU_EMOTION_EXCITEMENT,
    HU_EMOTION_ANXIETY,
} hu_emotion_tag_t;

typedef struct hu_stm_entity {
    char *name;
    size_t name_len;
    char *type; /* "person", "place", "org", "date", "topic" */
    size_t type_len;
    uint32_t mention_count;
    double importance;
} hu_stm_entity_t;

typedef struct hu_stm_emotion {
    hu_emotion_tag_t tag;
    double intensity; /* 0.0-1.0 */
} hu_stm_emotion_t;

typedef struct hu_stm_turn {
    char *role;
    char *content;
    size_t content_len;
    hu_stm_entity_t entities[HU_STM_MAX_ENTITIES];
    size_t entity_count;
    hu_stm_emotion_t emotions[HU_STM_MAX_EMOTIONS];
    size_t emotion_count;
    char *primary_topic;
    uint64_t timestamp_ms;
    bool occupied;
} hu_stm_turn_t;

typedef struct hu_stm_buffer {
    hu_allocator_t alloc;
    hu_stm_turn_t turns[HU_STM_MAX_TURNS];
    size_t turn_count;
    size_t head;
    char *topics[HU_STM_MAX_TOPICS];
    size_t topic_count;
    char *session_id;
    size_t session_id_len;
} hu_stm_buffer_t;

hu_error_t hu_stm_init(hu_stm_buffer_t *buf, hu_allocator_t alloc, const char *session_id,
                       size_t session_id_len);
hu_error_t hu_stm_record_turn(hu_stm_buffer_t *buf, const char *role, size_t role_len,
                              const char *content, size_t content_len, uint64_t timestamp_ms);
hu_error_t hu_stm_turn_add_entity(hu_stm_buffer_t *buf, size_t turn_idx, const char *name,
                                  size_t name_len, const char *type, size_t type_len,
                                  uint32_t mention_count);
hu_error_t hu_stm_turn_add_emotion(hu_stm_buffer_t *buf, size_t turn_idx, hu_emotion_tag_t tag,
                                   double intensity);
hu_error_t hu_stm_turn_set_primary_topic(hu_stm_buffer_t *buf, size_t turn_idx, const char *topic,
                                          size_t topic_len);
size_t hu_stm_count(const hu_stm_buffer_t *buf);
const hu_stm_turn_t *hu_stm_get(const hu_stm_buffer_t *buf, size_t idx);
hu_error_t hu_stm_build_context(const hu_stm_buffer_t *buf, hu_allocator_t *alloc, char **out,
                                size_t *out_len);
void hu_stm_clear(hu_stm_buffer_t *buf);
void hu_stm_deinit(hu_stm_buffer_t *buf);

typedef struct hu_mood_entry {
    hu_emotion_tag_t tag;
    double intensity;
    uint64_t timestamp_ms;
    char *contact_id;
    size_t contact_id_len;
} hu_mood_entry_t;

#endif
