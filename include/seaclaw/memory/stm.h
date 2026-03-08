#ifndef SC_STM_H
#define SC_STM_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/slice.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SC_STM_MAX_TURNS    20
#define SC_STM_MAX_ENTITIES 50
#define SC_STM_MAX_TOPICS   10
#define SC_STM_MAX_EMOTIONS 5

typedef enum sc_emotion_tag {
    SC_EMOTION_NEUTRAL,
    SC_EMOTION_JOY,
    SC_EMOTION_SADNESS,
    SC_EMOTION_ANGER,
    SC_EMOTION_FEAR,
    SC_EMOTION_SURPRISE,
    SC_EMOTION_FRUSTRATION,
    SC_EMOTION_EXCITEMENT,
    SC_EMOTION_ANXIETY,
} sc_emotion_tag_t;

typedef struct sc_stm_entity {
    char *name;
    size_t name_len;
    char *type; /* "person", "place", "org", "date", "topic" */
    size_t type_len;
    uint32_t mention_count;
    double importance;
} sc_stm_entity_t;

typedef struct sc_stm_emotion {
    sc_emotion_tag_t tag;
    double intensity; /* 0.0-1.0 */
} sc_stm_emotion_t;

typedef struct sc_stm_turn {
    char *role;
    char *content;
    size_t content_len;
    sc_stm_entity_t entities[SC_STM_MAX_ENTITIES];
    size_t entity_count;
    sc_stm_emotion_t emotions[SC_STM_MAX_EMOTIONS];
    size_t emotion_count;
    char *primary_topic;
    uint64_t timestamp_ms;
    bool occupied;
} sc_stm_turn_t;

typedef struct sc_stm_buffer {
    sc_allocator_t alloc;
    sc_stm_turn_t turns[SC_STM_MAX_TURNS];
    size_t turn_count;
    size_t head;
    char *topics[SC_STM_MAX_TOPICS];
    size_t topic_count;
    char *session_id;
    size_t session_id_len;
} sc_stm_buffer_t;

sc_error_t sc_stm_init(sc_stm_buffer_t *buf, sc_allocator_t alloc, const char *session_id,
                       size_t session_id_len);
sc_error_t sc_stm_record_turn(sc_stm_buffer_t *buf, const char *role, size_t role_len,
                              const char *content, size_t content_len, uint64_t timestamp_ms);
sc_error_t sc_stm_turn_add_entity(sc_stm_buffer_t *buf, size_t turn_idx, const char *name,
                                  size_t name_len, const char *type, size_t type_len,
                                  uint32_t mention_count);
sc_error_t sc_stm_turn_add_emotion(sc_stm_buffer_t *buf, size_t turn_idx, sc_emotion_tag_t tag,
                                   double intensity);
size_t sc_stm_count(const sc_stm_buffer_t *buf);
const sc_stm_turn_t *sc_stm_get(const sc_stm_buffer_t *buf, size_t idx);
sc_error_t sc_stm_build_context(const sc_stm_buffer_t *buf, sc_allocator_t *alloc, char **out,
                                size_t *out_len);
void sc_stm_clear(sc_stm_buffer_t *buf);
void sc_stm_deinit(sc_stm_buffer_t *buf);

typedef struct sc_mood_entry {
    sc_emotion_tag_t tag;
    double intensity;
    uint64_t timestamp_ms;
    char *contact_id;
    size_t contact_id_len;
} sc_mood_entry_t;

#endif
