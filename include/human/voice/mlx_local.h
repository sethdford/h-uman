#ifndef HU_VOICE_MLX_LOCAL_H
#define HU_VOICE_MLX_LOCAL_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/voice/provider.h"

/*
 * MLX Local Voice Provider — real-time voice via local Gemma on Apple Silicon.
 *
 * Connects to a local MLX inference server (scripts/mlx-server.py) and streams
 * text responses via SSE. The voice pipeline handles STT → text → MLX → text → TTS.
 *
 * Designed for fine-tuned Gemma E4B (~110 tok/s) with optional speculative
 * decoding using E2B as draft model (~2x speedup).
 */

typedef struct hu_voice_mlx_local_config {
    const char *endpoint;      /* MLX server URL (default: http://127.0.0.1:8741) */
    const char *model;         /* model name (informational, shown in logs) */
    const char *system_prompt; /* system instruction for the conversation */
    const char *voice_id;      /* TTS voice ID for downstream synthesis */
    int sample_rate;           /* output sample rate (0 = default 24000) */
    int max_tokens;            /* max generation tokens (0 = 256) */
    float temperature;         /* generation temperature (0 = 0.7) */
} hu_voice_mlx_local_config_t;

hu_error_t hu_voice_provider_mlx_local_create(hu_allocator_t *alloc,
                                              const hu_voice_mlx_local_config_t *config,
                                              hu_voice_provider_t *out);

#endif
