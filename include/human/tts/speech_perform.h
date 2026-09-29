#ifndef HU_TTS_SPEECH_PERFORM_H
#define HU_TTS_SPEECH_PERFORM_H
/*
 * Voice direction D1 (spec 2026-09-27): one LLM call casts the model as the
 * speaker in a scene and returns the spoken line with Cartesia tags. The line
 * is parsed and validated (D2) and its words must pass the drift guard (S3)
 * against the intent; anything else is reported, never spoken.
 */
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include "human/tts/speech_direction.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct hu_perform_scene {
    const char *speaker;      /* persona name, NULL = "the speaker" */
    const char *listener;     /* contact name, NULL = "a friend" */
    const char *relationship; /* e.g. "sister", NULL = omit */
    int hour_local;           /* 0-23 */
    int weekday;              /* 0 = Sunday .. 6, -1 = omit */
    const char *inbound;      /* what they last said, may be NULL */
    size_t inbound_len;
} hu_perform_scene_t;

typedef struct {
    bool ok;            /* dir is valid and its words pass the drift guard */
    const char *reason; /* static: ok, no_provider, provider_error, empty, a D2 verdict or a drift
                           name */
    hu_direction_t dir;
} hu_perform_result_t;

size_t hu_speech_perform_system_prompt(char *out, size_t cap);
size_t hu_speech_perform_user_message(const hu_perform_scene_t *scene, const char *intent,
                                      size_t intent_len, char *out, size_t cap);
/* Never fails for content reasons; see out->ok / out->reason. */
hu_error_t hu_speech_perform(hu_allocator_t *alloc, const hu_provider_t *provider,
                             const char *model, size_t model_len, const hu_perform_scene_t *scene,
                             const char *intent, size_t intent_len, hu_perform_result_t *out);
#endif
