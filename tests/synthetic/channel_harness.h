#ifndef HU_CHANNEL_HARNESS_H
#define HU_CHANNEL_HARNESS_H
#include "human/channel.h"
#include "human/channel_loop.h"
#include "synthetic_harness.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum hu_chaos_mode {
    HU_CHAOS_NONE = 0,
    HU_CHAOS_MESSAGE = 1,
    HU_CHAOS_INFRA = 2,
    HU_CHAOS_ALL = 3,
} hu_chaos_mode_t;

typedef struct hu_channel_test_config {
    const char *binary_path;
    const char *gemini_api_key;
    const char *gemini_model;
    uint16_t gateway_port;
    int tests_per_channel;
    int concurrency;
    int duration_secs;
    hu_chaos_mode_t chaos;
    const char *regression_dir;
    const char *real_imessage_target;
    const char **channels;
    size_t channel_count;
    bool all_channels;
    bool verbose;
} hu_channel_test_config_t;

typedef struct hu_conversation_turn {
    char user_message[4096];
    char expect_pattern[512];
} hu_conversation_turn_t;

typedef struct hu_conversation_scenario {
    char channel_name[32];
    char session_key[128];
    hu_conversation_turn_t turns[16];
    size_t turn_count;
} hu_conversation_scenario_t;

typedef struct hu_channel_test_entry {
    const char *name;
    hu_error_t (*create)(hu_allocator_t *alloc, hu_channel_t *out);
    void (*destroy)(hu_channel_t *ch);
    hu_error_t (*inject)(hu_channel_t *ch, const char *session_key, size_t sk_len,
                         const char *content, size_t c_len);
    hu_error_t (*poll)(void *ctx, hu_allocator_t *alloc, hu_channel_loop_msg_t *msgs, size_t max,
                       size_t *count);
    const char *(*get_last)(hu_channel_t *ch, size_t *out_len);
} hu_channel_test_entry_t;

hu_error_t hu_channel_run_conversations(hu_allocator_t *alloc, const hu_channel_test_config_t *cfg,
                                        hu_synth_gemini_ctx_t *gemini, hu_synth_metrics_t *metrics);
hu_error_t hu_channel_run_chaos(hu_allocator_t *alloc, const hu_channel_test_config_t *cfg,
                                hu_synth_gemini_ctx_t *gemini, hu_synth_metrics_t *metrics);
hu_error_t hu_channel_run_pressure(hu_allocator_t *alloc, const hu_channel_test_config_t *cfg,
                                   hu_synth_gemini_ctx_t *gemini, hu_synth_metrics_t *metrics);
hu_error_t hu_channel_run_real_imessage(hu_allocator_t *alloc, const hu_channel_test_config_t *cfg,
                                        hu_synth_gemini_ctx_t *gemini, hu_synth_metrics_t *metrics);

const hu_channel_test_entry_t *hu_channel_test_registry(size_t *count);
const hu_channel_test_entry_t *hu_channel_test_find(const char *name);

#define HU_CH_LOG(fmt, ...) fprintf(stderr, "[channel] " fmt "\n", ##__VA_ARGS__)
#define HU_CH_VERBOSE(cfg, fmt, ...)                           \
    do {                                                       \
        if ((cfg)->verbose)                                    \
            fprintf(stderr, "  [v] " fmt "\n", ##__VA_ARGS__); \
    } while (0)

#endif
